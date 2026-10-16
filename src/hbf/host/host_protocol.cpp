#include "openhbx/hbf/host/host_protocol.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "openhbx/common/checked_math.h"
#include "openhbx/hbf/host/packet_codec.h"

namespace openhbx::hbf::host {

HbfHostProtocol::HbfHostProtocol(HostProfile profile,
    const address::HbfAddressMapper& mapper, IControllerPort& controller,
    HostResponseSink response, ResetIntentSink reset_intent)
    : profile_(profile), mapper_(mapper), controller_(controller),
      response_(std::move(response)), validator_(profile),
      registers_(profile.channels, profile.axi_interfaces,
                 profile.channel_capacity_bytes / profile.axi_interfaces,
                 std::move(reset_intent)),
      links_(profile.channels, LinkState::Reset),
      interface_outstanding_(static_cast<std::size_t>(profile.channels) *
                             profile.axi_interfaces, 0) {
  if (profile.channels == 0 || profile.channels > 16 ||
      (profile.axi_interfaces != 1 && profile.axi_interfaces != 2 &&
       profile.axi_interfaces != 4) || profile.queue_depth_per_interface == 0 ||
      profile.channel_capacity_bytes == 0 ||
      profile.channel_capacity_bytes % profile.axi_interfaces != 0 ||
      (profile.channel_capacity_bytes / profile.axi_interfaces) % 4096 != 0 || !response_)
    throw std::invalid_argument("invalid Host profile");
}

bool HbfHostProtocol::gate_open(const HostIngress& request) const noexcept {
  return !resetting_ && request.generation == generation_ &&
         request.channel.value() < links_.size() &&
         links_[request.channel.value()] == LinkState::Ready &&
         registers_.ready(request.channel) && registers_.enabled(request.channel);
}

AdmissionResult HbfHostProtocol::submit(const HostIngress& request, Cycle now) {
  return submit_tracked(request, now).admission;
}

HostAdmission HbfHostProtocol::submit_tracked(const HostIngress& request, Cycle now) {
  const auto validation = validator_.validate(request);
  if (!validation) { ++rejected_; return {AdmissionResult::rejected(RejectionReason::InvalidArgument), {}}; }
  if (!gate_open(request)) { ++rejected_; return {AdmissionResult::rejected(RejectionReason::InvalidLifecycle), {}}; }
  if (request.packet_type == PacketType::FlashIo &&
      !registers_.allows_flash(request.channel, request.axi_interface,
                               request.address_bytes, request.size_bytes)) {
    ++rejected_; return {AdmissionResult::rejected(RejectionReason::InvalidArgument,
                                                   "outside per-AXI FIOSA/FIOSZ"), {}};
  }
  if (request.packet_type == PacketType::ScratchpadIo &&
      !registers_.allows_scratchpad(request.channel, request.axi_interface,
                                    request.address_bytes, request.size_bytes)) {
    ++rejected_; return {AdmissionResult::rejected(RejectionReason::InvalidArgument,
                                                   "outside per-AXI SMEMSA/SMEMSZ"), {}};
  }
  const auto channel = static_cast<std::size_t>(request.channel.value());
  const auto interface = channel * profile_.axi_interfaces + request.axi_interface;
  if (interface_outstanding_[interface] >= profile_.queue_depth_per_interface) {
    ++busy_; return {AdmissionResult::busy(), {}};
  }
  if (request.packet_type == PacketType::ScratchpadIo) {
    ++rejected_; return {AdmissionResult::rejected(RejectionReason::InvalidArgument,
                                                   "Scratchpad Host port is not implemented by S6"), {}};
  }
  const Token token(next_token_);
  const OrderingKey ordering{request.channel, request.axi_interface, request.axi_id};
  if (!scoreboard_.reserve(token, ordering, request.address_bytes, request.size_bytes)) {
    ++rejected_; return {AdmissionResult::rejected(RejectionReason::InvalidLifecycle), {}};
  }
  Record record; record.request = request;
  if (request.operation == HostOperation::Read) record.read_bytes.resize(request.size_bytes);
  const auto axi_capacity = validator_.axi_capacity_bytes();
  const auto axi_base = checked_mul(static_cast<std::uint64_t>(request.axi_interface),
                                    axi_capacity);
  const auto channel_address = axi_base ? checked_add(*axi_base, request.address_bytes)
                                        : std::nullopt;
  if (!channel_address) {
    scoreboard_.cancel(token); ++rejected_;
    return {AdmissionResult::rejected(RejectionReason::CapacityImpossible), {}};
  }
  if (request.packet_type == PacketType::CsrAdmin) {
    const auto opcode = decode_admin_opcode(request.admin_opcode);
    if (!opcode || !supported_admin_opcode(*opcode)) {
      scoreboard_.cancel(token); ++rejected_;
      return {AdmissionResult::rejected(RejectionReason::InvalidArgument,
                                       "Admin opcode unsupported by S6"), {}};
    }
    controller::ControllerRequest child;
    child.token = token; child.generation = request.generation;
    // Admin children are markers; issue_children routes them through submit_admin.
    child.endpoint = request.admin_opcode;
    record.children.push_back(std::move(child));
  } else {
    const bool dlu_read = request.operation == HostOperation::Read && request.size_bytes == 4096;
    const std::size_t child_count = dlu_read ? 1 : request.size_bytes / 64;
    for (std::size_t index = 0; index < child_count; ++index) {
      const auto offset = checked_mul(static_cast<std::uint64_t>(index), 64);
      const auto child_address = offset ? checked_add(*channel_address, *offset) : std::nullopt;
      if (!child_address) {
        scoreboard_.cancel(token); ++rejected_;
        return {AdmissionResult::rejected(RejectionReason::CapacityImpossible), {}};
      }
      const auto mapped = mapper_.map(request.channel, address::LocalByteAddress(*child_address));
      if (!mapped) {
        scoreboard_.cancel(token); ++rejected_;
        return {AdmissionResult::rejected(RejectionReason::InvalidArgument,
                                         address::to_string(mapped.error)), {}};
      }
      const auto& a = mapped.mapped->address;
      controller::ControllerRequest child;
      child.token = Token(next_token_ + index); child.generation = request.generation;
      child.operation = request.operation == HostOperation::Write
          ? controller::HostOperation::WriteSector
          : (dlu_read ? controller::HostOperation::ReadDlu
                      : controller::HostOperation::ReadSector);
      child.dlu = {{a.channel, a.owned_bank, a.block}, a.page};
      child.bank = a.physical_bank; child.sector = a.sector;
      child.endpoint = request.channel.value() * profile_.axi_interfaces +
                       request.axi_interface;
      child.read_mode = request.batch ? controller::ReadMode::Batch
                                      : controller::ReadMode::Regular;
      if (request.operation == HostOperation::Write && request.payload.size() == request.size_bytes) {
        const auto begin = request.payload.bytes().begin() +
            static_cast<std::ptrdiff_t>(index * 64);
        child.payload = PayloadHandle::from_bytes(std::vector<std::uint8_t>(begin, begin + 64));
      } else {
        child.payload = request.payload;
      }
      record.children.push_back(std::move(child));
    }
  }
  if (record.children.empty() ||
      next_token_ > std::numeric_limits<std::uint64_t>::max() - record.children.size()) {
    scoreboard_.cancel(token); ++rejected_;
    return {AdmissionResult::rejected(RejectionReason::CapacityImpossible), {}};
  }
  records_.emplace(token.value(), std::move(record));
  ++interface_outstanding_[interface];
  dispatching_ = true;
  const auto result = issue_children(token, now, true);
  dispatching_ = false;
  if (result.code != AdmissionCode::Accepted) {
    // The S6 contract guarantees no completion on Busy/Rejected.
    scoreboard_.cancel(token); records_.erase(token.value()); --interface_outstanding_[interface];
    if (result.code == AdmissionCode::Busy) ++busy_; else ++rejected_;
    return {result, {}};
  }
  next_token_ += records_.at(token.value()).children.size();
  ++accepted_; finish_if_ready(token); drain_responses();
  return {AdmissionResult::accepted(), token};
}

AdmissionResult HbfHostProtocol::issue_children(Token root, Cycle now, bool first_issue) {
  auto found = records_.find(root.value());
  if (found == records_.end()) return AdmissionResult::rejected(RejectionReason::InvalidLifecycle);
  auto& record = found->second;
  while (record.next_issue < record.children.size() && !record.failure) {
    auto& child = record.children[record.next_issue];
    children_[child.token.value()] = {root, record.next_issue};
    AdmissionResult result;
    if (record.request.packet_type == PacketType::CsrAdmin) {
      result = controller_.submit_admin(child.token, child.generation,
                                        static_cast<std::uint8_t>(child.endpoint), now);
    } else {
      result = controller_.submit(child, now);
    }
    if (result.code == AdmissionCode::Accepted) {
      ++record.next_issue; ++record.accepted_children;
      continue;
    }
    children_.erase(child.token.value());
    if (first_issue && record.accepted_children == 0) return result;
    if (result.code == AdmissionCode::Busy) return AdmissionResult::accepted();
    record.failure = controller::ControllerCompletion{root, record.request.generation,
        controller::ControllerStatus::Invalid, false, {}, now,
        controller::ControllerErrorInfo::None};
    finish_if_ready(root); return AdmissionResult::accepted();
  }
  finish_if_ready(root);
  return AdmissionResult::accepted();
}

void HbfHostProtocol::on_controller_completion(controller::ControllerCompletion completion) {
  const auto child = children_.find(completion.token.value());
  if (child == children_.end()) { ++stale_; return; }
  const auto root = child->second.root;
  const auto index = child->second.index;
  auto record = records_.find(root.value());
  if (record == records_.end()) { children_.erase(child); ++stale_; return; }
  if (completion.generation != record->second.request.generation ||
      completion.generation != generation_) { ++stale_; return; }
  children_.erase(child);
  ++record->second.completed_children;
  if (completion.completed_at > record->second.last_terminal_cycle)
    record->second.last_terminal_cycle = completion.completed_at;
  if (completion.status != controller::ControllerStatus::Success &&
      completion.status != controller::ControllerStatus::Corrected) {
    if (!record->second.failure) record->second.failure = completion;
  } else if (record->second.request.operation == HostOperation::Read &&
             (!completion.data_valid || completion.payload.empty())) {
    if (!record->second.failure) {
      completion.status = controller::ControllerStatus::Invalid;
      completion.data_valid = false; completion.payload = {};
      record->second.failure = completion;
    }
  } else if (record->second.request.operation == HostOperation::Read) {
    const std::size_t offset = record->second.children.size() == 1
        ? 0 : index * 64;
    if (offset > record->second.read_bytes.size() ||
        completion.payload.size() > record->second.read_bytes.size() - offset) {
      completion.status = controller::ControllerStatus::Invalid;
      completion.data_valid = false; completion.payload = {};
      record->second.failure = completion;
    } else {
      std::copy(completion.payload.bytes().begin(), completion.payload.bytes().end(),
                record->second.read_bytes.begin() + static_cast<std::ptrdiff_t>(offset));
      record->second.completion.completed_at = completion.completed_at;
      if (completion.status == controller::ControllerStatus::Corrected)
        record->second.completion.status = controller::ControllerStatus::Corrected;
      if (completion.error_info != controller::ControllerErrorInfo::None)
        record->second.completion.error_info = completion.error_info;
    }
  } else {
    record->second.completion.completed_at = completion.completed_at;
    record->second.completion.error_info = completion.error_info;
  }
  if (!dispatching_ && !record->second.failure) {
    dispatching_ = true;
    issue_children(root, completion.completed_at, false);
    dispatching_ = false;
  }
  finish_if_ready(root);
  if (!dispatching_) drain_responses();
}

void HbfHostProtocol::finish_if_ready(Token root) {
  auto found = records_.find(root.value());
  if (found == records_.end() || found->second.has_completion) return;
  auto& record = found->second;
  if (record.failure) {
    if (record.completed_children != record.accepted_children) return;
    record.completion = *record.failure;
    record.completion.token = root; record.has_completion = true;
  } else {
    if (record.next_issue != record.children.size() ||
        record.completed_children != record.children.size()) return;
    record.completion.token = root; record.completion.generation = record.request.generation;
    if (record.completion.status != controller::ControllerStatus::Corrected)
      record.completion.status = controller::ControllerStatus::Success;
    record.completion.data_valid = record.request.operation == HostOperation::Read;
    if (record.completion.data_valid)
      record.completion.payload = PayloadHandle::from_bytes(std::move(record.read_bytes));
    record.has_completion = true;
  }
  if (record.last_terminal_cycle > record.completion.completed_at)
    record.completion.completed_at = record.last_terminal_cycle;
  scoreboard_.mark_ready(root);
}

void HbfHostProtocol::pump(Cycle now) {
  if (resetting_) return;
  std::vector<Token> roots;
  for (const auto& entry : records_)
    if (!entry.second.failure && entry.second.next_issue < entry.second.children.size())
      roots.emplace_back(entry.first);
  dispatching_ = true;
  for (auto root : roots) issue_children(root, now, false);
  dispatching_ = false;
  for (auto root : roots) finish_if_ready(root);
  drain_responses();
}

void HbfHostProtocol::drain_responses() {
  for (const auto token : scoreboard_.release_ready()) {
    const auto it = records_.find(token.value());
    if (it == records_.end() || !it->second.has_completion) { ++duplicate_; continue; }
    auto response = encode_response(it->second.request, token, it->second.completion);
    if (!response) {
      response = HostResponse{token, it->second.request.generation,
          it->second.request.channel, it->second.request.axi_interface,
          it->second.request.axi_id, it->second.request.packet_type,
          it->second.request.operation, 0xF, false, {},
          it->second.completion.completed_at,
          controller::ControllerErrorInfo::None};
    }
    const auto response_interface = static_cast<std::size_t>(
        it->second.request.channel.value()) * profile_.axi_interfaces +
        it->second.request.axi_interface;
    --interface_outstanding_[response_interface];
    records_.erase(it); ++terminal_; response_(std::move(*response));
  }
}

LinkTransitionResult HbfHostProtocol::set_channel_state(
    address::ChannelId channel, LinkState state, Generation generation) {
  if (channel.value() >= links_.size()) return LinkTransitionResult::InvalidChannel;
  if (generation != generation_) return LinkTransitionResult::StaleGeneration;
  const auto current = links_[channel.value()];
  const bool valid =
      (current == LinkState::Reset && state == LinkState::InitializationPhase1) ||
      (current == LinkState::InitializationPhase1 && state == LinkState::InitializationPhase2) ||
      (current == LinkState::InitializationPhase2 && state == LinkState::InitializationPhase3) ||
      (current == LinkState::InitializationPhase3 && state == LinkState::InitializationPhase4) ||
      (current == LinkState::InitializationPhase4 && state == LinkState::Ready) ||
      (current == LinkState::Ready &&
       (state == LinkState::Disabled || state == LinkState::Faulted)) ||
      ((current == LinkState::Disabled || current == LinkState::Faulted) &&
       state == LinkState::InitializationPhase1);
  if (!valid) return LinkTransitionResult::InvalidTransition;
  links_[channel.value()] = state;
  registers_.set_ready(channel, state == LinkState::Ready);
  return LinkTransitionResult::Accepted;
}

void HbfHostProtocol::reset(Generation next, Cycle now) {
  if (next <= generation_) return;
  resetting_ = true;
  dispatching_ = true;
  controller_.reset(next, now);
  dispatching_ = false;
  for (auto& entry : records_) finish_if_ready(Token(entry.first));
  drain_responses();
  // A conforming S6 reset terminally completes every accepted request. This
  // fallback prevents a broken downstream from leaking Host credits.
  for (const auto& entry : records_) {
    HostResponse response;
    response.token = Token(entry.first); response.generation = entry.second.request.generation;
    response.channel = entry.second.request.channel;
    response.axi_interface = entry.second.request.axi_interface;
    response.axi_id = entry.second.request.axi_id;
    response.packet_type = entry.second.request.packet_type;
    response.operation = entry.second.request.operation;
    response.command_status = 0xE; response.completed_at = now;
    response_(std::move(response)); ++terminal_;
  }
  records_.clear(); children_.clear(); scoreboard_.clear();
  std::fill(interface_outstanding_.begin(), interface_outstanding_.end(), 0);
  generation_ = next;
  for (std::size_t i = 0; i < links_.size(); ++i) {
    links_[i] = LinkState::Reset; registers_.reset_hbf(address::ChannelId(i));
  }
  resetting_ = false;
}

bool HbfHostProtocol::supported_admin_opcode(AdminOpcode opcode) noexcept {
  switch (opcode) {
    case AdminOpcode::SetFeature:
    case AdminOpcode::GetFeature:
    case AdminOpcode::SecureErase:
    case AdminOpcode::GetLogPage:
    case AdminOpcode::Bist:
    case AdminOpcode::ZoneRemapping:
    case AdminOpcode::ReadUcieErrors:
    case AdminOpcode::ReducedCapacity:
    case AdminOpcode::RegisterAccess: return true;
  }
  return false;
}

HostSnapshot HbfHostProtocol::snapshot() const noexcept {
  return {accepted_, terminal_, busy_, rejected_, stale_, duplicate_, records_.size()};
}

}  // namespace openhbx::hbf::host
