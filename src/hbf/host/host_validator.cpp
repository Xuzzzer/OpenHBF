#include "openhbx/hbf/host/host_validator.h"

#include <limits>

namespace openhbx::hbf::host {

ValidationResult HostValidator::validate(const HostIngress& request) const noexcept {
  if (profile_.channels == 0 || profile_.channels > 16 ||
      (profile_.axi_interfaces != 1 && profile_.axi_interfaces != 2 &&
       profile_.axi_interfaces != 4) || profile_.queue_depth_per_interface == 0) {
    return {HostError::InvalidLifecycle};
  }
  if (request.channel.value() >= profile_.channels) return {HostError::InvalidChannel};
  if (request.axi_interface >= profile_.axi_interfaces) return {HostError::InvalidAxiInterface};
  if (request.packet_type == PacketType::Reserved) return {HostError::ReservedPacket};
  if (request.address_bytes % 64 != 0) return {HostError::Misaligned};
  if (request.size_bytes == 0 || request.size_bytes > 4096 || request.size_bytes % 64 != 0)
    return {HostError::InvalidSize};
  if (request.address_bytes > std::numeric_limits<std::uint64_t>::max() - request.size_bytes)
    return {HostError::OutOfRange};
  if ((request.address_bytes % 4096) + request.size_bytes > 4096)
    return {HostError::CrossesDlu};
  const auto axi_capacity = axi_capacity_bytes();
  if (axi_capacity == 0 || profile_.channel_capacity_bytes % profile_.axi_interfaces != 0 ||
      request.address_bytes + request.size_bytes > axi_capacity)
    return {HostError::OutOfRange};
  if (request.packet_type == PacketType::FlashIo) {
    if (request.operation == HostOperation::Admin) return {HostError::InvalidLifecycle};
    if (request.operation == HostOperation::Write &&
        request.payload.size() != request.size_bytes)
      return {HostError::InvalidPayload};
    if (request.operation == HostOperation::Read && !request.payload.empty())
      return {HostError::InvalidPayload};
  }
  if (request.packet_type == PacketType::CsrAdmin &&
      request.operation != HostOperation::Admin) return {HostError::InvalidLifecycle};
  return {};
}

}  // namespace openhbx::hbf::host
