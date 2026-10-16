#include "openhbx/media/nand/nand_channel_slice.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "openhbx/common/checked_math.h"

namespace openhbx::media::nand {
namespace {
Cycle later(Cycle a, Cycle b) { return a < b ? b : a; }
MediaStatus raw_status(ras::RawReadClass value) {
  switch (value) {
    case ras::RawReadClass::Clean: return MediaStatus::Success;
    case ras::RawReadClass::Correctable: return MediaStatus::RawCorrectable;
    case ras::RawReadClass::Uncorrectable: return MediaStatus::RawUncorrectable;
    case ras::RawReadClass::Retry: return MediaStatus::RetrySuggested;
    case ras::RawReadClass::Refresh: return MediaStatus::RefreshNotice;
  }
  return MediaStatus::IntegrityError;
}
}  // namespace

NandChannelSlice::NandChannelSlice(NandChannelSliceConfig config,
                                   DieDomain& die_domain, EventQueue& events,
                                   HandlerId handler, CompletionSink completion)
    : config_(std::move(config)), die_domain_(die_domain),
      topology_(config_.geometry), events_(events), handler_(handler),
      completion_(std::move(completion)), reliability_(config_.reliability) {
  if (config_.owned_banks.empty())
    throw std::invalid_argument("channel slice requires at least one owned bank");
  if (!events_.register_handler(handler_, [this](EventPayload payload) { on_event(std::move(payload)); }))
    throw std::invalid_argument("duplicate media slice event handler");
}

MediaStatus NandChannelSlice::validate(const FlashCommand& cmd) const {
  const auto page = topology_.flatten_page(cmd.address);
  const auto block = topology_.flatten_block(cmd.address);
  if (!page || !block) return MediaStatus::InvalidAddress;
  // CHAN-006 ownership hard invariant. The router dispatches by channel, so a
  // mismatch here means an integration bug or a forged command; reject with
  // zero side effects.
  if (cmd.channel != config_.channel ||
      !config_.owned_banks.count(topology_.bank_id(cmd.address)))
    return MediaStatus::InvalidAddress;
  if (die_domain_.capacity_unusable()) return MediaStatus::CapacityUnusable;
  if (die_domain_.bad_block_table().is_bad(*block)) return MediaStatus::BadBlock;
  if (cmd.kind == CommandKind::Program) {
    if (cmd.payload.size() != config_.geometry.page_bytes) return MediaStatus::InvalidPayload;
    if (pages_.count(*page)) return MediaStatus::InvalidState;
  }
  return MediaStatus::Success;
}

AdmissionResult NandChannelSlice::try_issue(FlashCommand command, Cycle now) {
  const MediaStatus status = validate(command);
  if (status != MediaStatus::Success) {
    return AdmissionResult::rejected(RejectionReason::InvalidArgument, "media command rejected");
  }
  if (contexts_.size() >= config_.max_inflight || contexts_.count(command.token.value()))
    return AdmissionResult::busy();
  if (command.generation != generation_)
    return AdmissionResult::rejected(RejectionReason::InvalidLifecycle, "stale generation");
  Stage first = Stage::ReadCommand;
  if (command.kind == CommandKind::Program) first = Stage::ProgramData;
  if (command.kind == CommandKind::Erase) first = Stage::EraseSetup;
  const auto bank = topology_.bank_id(command.address);
  const auto core = command.address.core_die;
  Cycle start = later(now, bank_eat_[bank]);
  if (uses_channel_path(first)) start = later(start, channel_media_path_eat_[core]);
  if (!checked_add(start.value(), duration(first)))
    return AdmissionResult::rejected(RejectionReason::CapacityImpossible,
                                     "media stage cycle overflow");
  Context context{std::move(command), first, now, 0, {}};
  const auto token = context.command.token.value();
  contexts_.emplace(token, std::move(context));
  if (!schedule(contexts_.at(token), now, first)) {
    contexts_.erase(token);
    return AdmissionResult::rejected(RejectionReason::CapacityImpossible,
                                     "media stage cycle overflow");
  }
  return AdmissionResult::accepted();
}

std::uint64_t NandChannelSlice::duration(Stage stage) const {
  switch (stage) {
    case Stage::ReadCommand: return config_.timing.read_command;
    case Stage::ReadSense: return config_.timing.read_sense;
    case Stage::ProgramData: return config_.timing.program_data_in;
    case Stage::ProgramArray: return config_.timing.program_array;
    case Stage::ProgramVerify: return config_.timing.program_verify;
    case Stage::EraseSetup: return config_.timing.erase_setup;
    case Stage::EraseArray: return config_.timing.erase_array;
    case Stage::EraseVerify: return config_.timing.erase_verify;
  }
  return 0;
}
bool NandChannelSlice::uses_channel_path(Stage stage) const {
  return stage == Stage::ReadCommand || stage == Stage::ProgramData || stage == Stage::EraseSetup;
}

bool NandChannelSlice::schedule(Context& context, Cycle now, Stage next) {
  const auto bank = topology_.bank_id(context.command.address);
  const auto core = context.command.address.core_die;
  Cycle start = later(now, bank_eat_[bank]);
  if (uses_channel_path(next)) start = later(start, channel_media_path_eat_[core]);
  const auto end_value = checked_add(start.value(), duration(next));
  if (!end_value) return false;
  const Cycle end(*end_value);
  bank_eat_[bank] = end;
  if (uses_channel_path(next)) channel_media_path_eat_[core] = end;
  context.stage = next;
  context.due = end;
  ++context.sequence;
  EventSpec event{end, EventPhase::MediaCommit, handler_, context.command.generation,
                  EventPayload(context.command.token)};
  const auto code = events_.schedule(event, now, EventPhase::Reset);
  if (code != ScheduleCode::Accepted) throw std::logic_error("media slice event schedule failed");
  return true;
}

void NandChannelSlice::on_event(EventPayload payload) {
  const auto token = std::get_if<Token>(&payload);
  if (!token) { ++stale_events_; return; }
  auto it = contexts_.find(token->value());
  if (it == contexts_.end()) { ++stale_events_; return; }
  Context& ctx = it->second;
  const Cycle now = ctx.due;
  if (die_domain_.die_blocked(topology_.die_id(ctx.command.address))) {
    terminal(ctx, MediaStatus::DieTemporarilyBlocked, now, false);
    return;
  }
  switch (ctx.stage) {
    case Stage::ReadCommand:
      if (!schedule(ctx, now, Stage::ReadSense)) terminal(ctx, MediaStatus::IntegrityError, now, false);
      return;
    case Stage::ReadSense: {
      const auto page = *topology_.flatten_page(ctx.command.address);
      const auto block = *topology_.flatten_block(ctx.command.address);
      const auto found = pages_.find(page);
      if (found == pages_.end()) { terminal(ctx, MediaStatus::ReadErasedPage, now, false); return; }
      ctx.read_snapshot = found->second.payload;
      const auto result = reliability_.evaluate_read(page, erase_counts_[block], 25.0);
      terminal(ctx, raw_status(result), now, false); return;
    }
    case Stage::ProgramData:
      if (!schedule(ctx, now, Stage::ProgramArray)) terminal(ctx, MediaStatus::IntegrityError, now, false);
      return;
    case Stage::ProgramArray:
      if (!schedule(ctx, now, Stage::ProgramVerify)) terminal(ctx, MediaStatus::IntegrityError, now, false);
      return;
    case Stage::ProgramVerify: {
      const auto block = *topology_.flatten_block(ctx.command.address);
      const bool fail = reliability_.fails(block, ras::ForcedFailure::Program);
      terminal(ctx, fail ? MediaStatus::ProgramFail : MediaStatus::Success, now, !fail); return;
    }
    case Stage::EraseSetup:
      if (!schedule(ctx, now, Stage::EraseArray)) terminal(ctx, MediaStatus::IntegrityError, now, false);
      return;
    case Stage::EraseArray:
      if (!schedule(ctx, now, Stage::EraseVerify)) terminal(ctx, MediaStatus::IntegrityError, now, false);
      return;
    case Stage::EraseVerify: {
      const auto block = *topology_.flatten_block(ctx.command.address);
      const bool fail = reliability_.fails(block, ras::ForcedFailure::Erase);
      terminal(ctx, fail ? MediaStatus::EraseFail : MediaStatus::Success, now, !fail); return;
    }
  }
}

void NandChannelSlice::terminal(Context context, MediaStatus status, Cycle now, bool commit) {
  const auto page = *topology_.flatten_page(context.command.address);
  const auto block = *topology_.flatten_block(context.command.address);
  if (commit && context.command.kind == CommandKind::Program) {
    PageRecord record;
    if (config_.payload_mode == PayloadMode::Sparse) record.payload = context.command.payload;
    else {
      for (auto byte : context.command.payload.bytes()) record.signature = record.signature * 131 + byte;
    }
    pages_[page] = std::move(record);
    ++committed_pages_;
  } else if (commit && context.command.kind == CommandKind::Erase) {
    const auto first = block * config_.geometry.pages_per_block;
    for (std::uint64_t i = 0; i < config_.geometry.pages_per_block; ++i) {
      if (pages_.erase(first + i)) --committed_pages_;
    }
    ++erase_counts_[block];
  }
  PayloadHandle result_payload;
  bool data_valid = false;
  if (context.command.kind == CommandKind::Read &&
      (status == MediaStatus::Success || status == MediaStatus::RawCorrectable ||
       status == MediaStatus::RefreshNotice)) {
    result_payload = context.read_snapshot;
    data_valid = config_.payload_mode == PayloadMode::Sparse;
  }
  contexts_.erase(context.command.token.value());
  completion_({context.command.token, status, data_valid, std::move(result_payload), now});
}

void NandChannelSlice::reset(Generation next, Cycle now) {
  std::vector<Context> aborted;
  for (const auto& item : contexts_) aborted.push_back(item.second);
  contexts_.clear();
  generation_ = next;
  events_.discard_generation(handler_, next);
  bank_eat_.clear();
  channel_media_path_eat_.clear();
  for (const auto& ctx : aborted)
    completion_({ctx.command.token, MediaStatus::Aborted, false, {}, now});
}
MediaSnapshot NandChannelSlice::snapshot() const {
  return {contexts_.size(), contexts_.size(), committed_pages_,
          die_domain_.bad_block_table().size(), stale_events_};
}
bool NandChannelSlice::page_is_valid(const PhysicalAddress& a) const {
  const auto page = topology_.flatten_page(a); return page && pages_.count(*page);
}
std::uint64_t NandChannelSlice::block_erase_count(const PhysicalAddress& a) const {
  const auto block = topology_.flatten_block(a);
  const auto it = block ? erase_counts_.find(*block) : erase_counts_.end();
  return it == erase_counts_.end() ? 0 : it->second;
}
}  // namespace openhbx::media::nand
