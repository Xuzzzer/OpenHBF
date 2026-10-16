#include "openhbx/pal/flash_pal.h"

#include <stdexcept>
#include <utility>

#include "openhbx/common/checked_math.h"

namespace openhbx::pal {
namespace {
constexpr std::uint64_t kBitsPerByte = 8;
}

FlashPal::FlashPal(FlashPalConfig config, EventQueue& events, HandlerId handler,
                   interconnect::InterconnectFabric& fabric, IFlashMediaPort& media,
                   PalCompletionSink completion)
    : config_(config), events_(events), handler_(handler), fabric_(fabric), media_(media),
      completion_(std::move(completion)) {
  if (config_.max_inflight == 0 || config_.page_bytes == 0 || config_.command_bits == 0 ||
      config_.retry_delay_cycles == 0)
    throw std::invalid_argument("invalid Flash PAL configuration");
  if (!events_.register_handler(handler_, [this](EventPayload payload) {
        on_event(std::move(payload));
      }))
    throw std::invalid_argument("duplicate PAL event handler");
}

AdmissionResult FlashPal::try_issue(FlashPhysicalRequest request, Cycle now) {
  const auto token = request.token.value();
  if (token == 0 || request.generation != generation_)
    return AdmissionResult::rejected(RejectionReason::InvalidLifecycle,
                                     "invalid token or generation");
  if (seen_tokens_.count(token) != 0)
    return AdmissionResult::rejected(RejectionReason::InvalidLifecycle,
                                     "token must be globally unique");
  const bool program = request.kind == media::CommandKind::Program;
  if ((program && request.payload.size() != config_.page_bytes) ||
      (!program && !request.payload.empty()))
    return AdmissionResult::rejected(RejectionReason::InvalidArgument,
                                     "operation payload contract violated");
  if (inflight_by_channel_[request.channel.value()] >= config_.max_inflight)
    return AdmissionResult::busy();

  auto route = fabric_.route_for(request.endpoint);
  if (!route) return AdmissionResult::busy();
  std::uint64_t bits = config_.command_bits;
  if (program) {
    const auto payload_bits = checked_mul(config_.page_bytes, kBitsPerByte);
    std::optional<std::uint64_t> total;
    if (payload_bits) total = checked_add(bits, *payload_bits);
    if (!total)
      return AdmissionResult::rejected(RejectionReason::CapacityImpossible,
                                       "forward transfer size overflow");
    bits = *total;
  }
  if (next_transfer_id_ == 0)
    return AdmissionResult::rejected(RejectionReason::CapacityImpossible,
                                     "transfer id exhausted");
  const interconnect::Transfer transfer{next_transfer_id_, interconnect::Direction::Forward,
      program ? interconnect::TrafficClass::ProgramData
              : interconnect::TrafficClass::Command,
      bits, *route};
  const auto reservation = fabric_.try_reserve(transfer, now);
  if (reservation.code == interconnect::ReservationCode::Busy) return AdmissionResult::busy();
  if (reservation.code != interconnect::ReservationCode::Accepted) {
    const auto reason = reservation.code == interconnect::ReservationCode::Overflow
                            ? RejectionReason::CapacityImpossible
                            : RejectionReason::InvalidArgument;
    return AdmissionResult::rejected(reason, "forward route reservation failed");
  }

  Context context{std::move(request), Stage::ForwardInFlight, next_transfer_id_, *route,
                  reservation.end, 0, 0, PalStatus::InternalError, false, {}};
  ++inflight_by_channel_[context.request.channel.value()];
  contexts_.emplace(token, std::move(context));
  seen_tokens_.insert(token);
  ++next_transfer_id_;
  ++accepted_;
  if (!schedule(Token(token), reservation.end, now))
    throw std::logic_error("PAL could not schedule accepted forward transfer");
  return AdmissionResult::accepted();
}

bool FlashPal::schedule(Token token, Cycle due, Cycle now) {
  EventSpec event{due, EventPhase::Interconnect, handler_, generation_, EventPayload(token)};
  return events_.schedule(event, now, EventPhase::Reset) == ScheduleCode::Accepted;
}

void FlashPal::on_event(EventPayload payload) {
  const auto token = std::get_if<Token>(&payload);
  if (!token) {
    ++integrity_errors_;
    return;
  }
  auto found = contexts_.find(token->value());
  if (found == contexts_.end()) {
    ++stale_events_;
    return;
  }
  Context& context = found->second;
  const Cycle now = context.due;
  switch (context.stage) {
    case Stage::ForwardInFlight:
      if (!fabric_.release(context.transfer_id)) {
        ++integrity_errors_;
        context.terminal_status = PalStatus::InternalError;
        terminal(token->value(), now);
        return;
      }
      if (!fabric_.route_is_operational(context.frozen_route)) {
        context.terminal_status = PalStatus::PathUnavailable;
        terminal(token->value(), now);
        return;
      }
      issue_media(context, now);
      return;
    case Stage::MediaPending:
      issue_media(context, now);
      return;
    case Stage::ReturnPending:
      begin_return(context, now);
      return;
    case Stage::ReturnInFlight:
      if (!fabric_.release(context.transfer_id)) ++integrity_errors_;
      terminal(token->value(), now);
      return;
    case Stage::MediaInFlight:
      ++integrity_errors_;
      return;
  }
}

void FlashPal::issue_media(Context& context, Cycle now) {
  media::FlashCommand command{context.request.token, context.request.generation,
                              context.request.kind, context.request.channel,
                              context.request.address, context.request.payload};
  const auto result = media_.try_issue_media(std::move(command), now);
  if (result.code == AdmissionCode::Accepted) {
    context.stage = Stage::MediaInFlight;
    return;
  }
  if (result.code == AdmissionCode::Busy &&
      context.media_retries < config_.media_retry_budget) {
    ++context.media_retries;
    ++media_retries_;
    const auto due = checked_add(now.value(), config_.retry_delay_cycles);
    if (!due) {
      context.terminal_status = PalStatus::InternalError;
      terminal(context.request.token.value(), now);
      return;
    }
    context.stage = Stage::MediaPending;
    context.due = Cycle(*due);
    if (!schedule(context.request.token, context.due, now))
      throw std::logic_error("PAL could not schedule media retry");
    return;
  }
  context.terminal_status = result.code == AdmissionCode::Rejected
                                ? PalStatus::MediaRejected
                                : PalStatus::InternalError;
  context.data_valid = false;
  context.result_payload.reset();
  begin_return(context, now);
}

void FlashPal::on_media_completion(media::MediaCompletion completion) {
  auto found = contexts_.find(completion.token.value());
  if (found == contexts_.end()) {
    ++stale_events_;
    return;
  }
  Context& context = found->second;
  if (context.stage != Stage::MediaInFlight) {
    ++integrity_errors_;
    return;
  }
  context.terminal_status = map_status(completion.status);
  const bool read_data_status = completion.status == media::MediaStatus::Success ||
      completion.status == media::MediaStatus::RawCorrectable ||
      completion.status == media::MediaStatus::RefreshNotice;
  const bool is_read = context.request.kind == media::CommandKind::Read;
  const bool valid_read_payload = completion.data_valid &&
                                  completion.payload.size() == config_.page_bytes;
  const bool has_return_data = completion.data_valid || !completion.payload.empty();
  const bool malformed_read = is_read &&
      ((read_data_status && !valid_read_payload) || (!read_data_status && has_return_data));
  const bool malformed_non_read = !is_read &&
                                  has_return_data;
  if (malformed_read || malformed_non_read) {
    ++integrity_errors_;
    context.terminal_status = PalStatus::InternalError;
    context.data_valid = false;
    context.result_payload.reset();
    begin_return(context, completion.completed_at);
    return;
  }
  context.data_valid = is_read && read_data_status && valid_read_payload;
  context.result_payload = context.data_valid ? completion.payload : PayloadHandle{};
  begin_return(context, completion.completed_at);
}

void FlashPal::begin_return(Context& context, Cycle now) {
  auto route = fabric_.route_for(context.request.endpoint);
  if (!route) {
    context.terminal_status = PalStatus::PathUnavailable;
    context.data_valid = false;
    context.result_payload.reset();
    terminal(context.request.token.value(), now);
    return;
  }
  std::uint64_t bits = config_.command_bits;
  if (context.data_valid) {
    const auto payload_bits = checked_mul(config_.page_bytes, kBitsPerByte);
    std::optional<std::uint64_t> total;
    if (payload_bits) total = checked_add(bits, *payload_bits);
    if (!total) {
      context.terminal_status = PalStatus::InternalError;
      context.data_valid = false;
      context.result_payload.reset();
      terminal(context.request.token.value(), now);
      return;
    }
    bits = *total;
  }
  if (next_transfer_id_ == 0) {
    context.terminal_status = PalStatus::InternalError;
    terminal(context.request.token.value(), now);
    return;
  }
  const interconnect::Transfer transfer{next_transfer_id_, interconnect::Direction::Return,
      context.data_valid ? interconnect::TrafficClass::ReadData
                         : interconnect::TrafficClass::Status,
      bits, *route};
  const auto reservation = fabric_.try_reserve(transfer, now);
  if (reservation.code == interconnect::ReservationCode::Busy &&
      context.return_retries < config_.return_retry_budget) {
    ++context.return_retries;
    ++return_retries_;
    const auto due = checked_add(now.value(), config_.retry_delay_cycles);
    if (!due) {
      context.terminal_status = PalStatus::InternalError;
      terminal(context.request.token.value(), now);
      return;
    }
    context.stage = Stage::ReturnPending;
    context.due = Cycle(*due);
    if (!schedule(context.request.token, context.due, now))
      throw std::logic_error("PAL could not schedule return retry");
    return;
  }
  if (reservation.code != interconnect::ReservationCode::Accepted) {
    context.terminal_status = reservation.code == interconnect::ReservationCode::PathUnavailable
                                  ? PalStatus::PathUnavailable
                                  : PalStatus::InternalError;
    context.data_valid = false;
    context.result_payload.reset();
    terminal(context.request.token.value(), now);
    return;
  }
  context.frozen_route = *route;
  context.transfer_id = next_transfer_id_++;
  context.stage = Stage::ReturnInFlight;
  context.due = reservation.end;
  if (!schedule(context.request.token, context.due, now))
    throw std::logic_error("PAL could not schedule accepted return transfer");
}

void FlashPal::terminal(std::uint64_t token, Cycle now) {
  auto found = contexts_.find(token);
  if (found == contexts_.end()) {
    ++integrity_errors_;
    return;
  }
  Context context = std::move(found->second);
  contexts_.erase(found);
  const auto channel = context.request.channel.value();
  auto used = inflight_by_channel_.find(channel);
  if (used != inflight_by_channel_.end() && used->second != 0) {
    if (--used->second == 0) inflight_by_channel_.erase(used);
  }
  ++terminal_;
  completion_({context.request.token, context.terminal_status, context.data_valid,
               std::move(context.result_payload), now});
}

void FlashPal::reset(Generation next, Cycle now) {
  if (next <= generation_)
    throw std::invalid_argument("PAL generation must increase on reset");
  generation_ = next;
  fabric_.reset();
  events_.discard_generation(next);
  std::map<std::uint64_t, Context> aborted;
  aborted.swap(contexts_);
  inflight_by_channel_.clear();
  for (auto& item : aborted) {
    ++terminal_;
    completion_({item.second.request.token, PalStatus::Aborted, false, {}, now});
  }
}

FlashPalSnapshot FlashPal::snapshot() const {
  return {contexts_.size(), accepted_, terminal_, media_retries_, return_retries_,
          stale_events_, integrity_errors_};
}

PalStatus FlashPal::map_status(media::MediaStatus status) {
  switch (status) {
    case media::MediaStatus::Success:
      return PalStatus::Success;
    case media::MediaStatus::ReadErasedPage:
      return PalStatus::ReadErasedPage;
    case media::MediaStatus::RawCorrectable:
      return PalStatus::RawCorrectable;
    case media::MediaStatus::RawUncorrectable:
      return PalStatus::RawUncorrectable;
    case media::MediaStatus::RetrySuggested:
      return PalStatus::RetrySuggested;
    case media::MediaStatus::RefreshNotice:
      return PalStatus::RefreshNotice;
    case media::MediaStatus::ProgramFail:
      return PalStatus::ProgramFail;
    case media::MediaStatus::EraseFail:
      return PalStatus::EraseFail;
    case media::MediaStatus::Aborted:
      return PalStatus::Aborted;
    case media::MediaStatus::CapacityUnusable:
      return PalStatus::CapacityUnusable;
    case media::MediaStatus::DieTemporarilyBlocked:
      return PalStatus::DieTemporarilyBlocked;
    case media::MediaStatus::BadBlock:
      return PalStatus::BadBlock;
    case media::MediaStatus::InvalidAddress:
      return PalStatus::InvalidAddress;
    case media::MediaStatus::InvalidState:
      return PalStatus::InvalidState;
    case media::MediaStatus::InvalidPayload:
      return PalStatus::InvalidPayload;
    case media::MediaStatus::IntegrityError:
      return PalStatus::MediaIntegrityError;
  }
  return PalStatus::InternalError;
}

}  // namespace openhbx::pal
