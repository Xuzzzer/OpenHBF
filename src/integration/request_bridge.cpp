#include "openhbx/integration/request_bridge.h"

#include <limits>
#include <utility>
#include <vector>

namespace openhbx::integration {

RequestBridge::RequestBridge(OpenHbxSystem& system) : system_(system) {
  next_token_ = system_.allocate_request_token().value();
}

AdmissionResult RequestBridge::try_submit(BridgeRequest request) {
  if (request.address < 0 || request.size_bytes <= 0 ||
      request.size_bytes > 4096 || !request.completion || next_token_ == 0)
    return AdmissionResult::rejected(RejectionReason::InvalidArgument);
  if (request.ingress_id < 0 || request.ingress_id > 3 || request.source_id < 0)
    return AdmissionResult::rejected(RejectionReason::InvalidArgument);

  hbf::host::HostIngress ingress;
  ingress.generation = system_.generation();
  // CHAN-001: the flat->(channel, axi, local) decode lives in exactly one
  // place (OpenHbxSystem::decode_flat_address); downstream modules receive
  // channel explicitly.
  const auto decoded =
      system_.decode_flat_address(static_cast<std::uint64_t>(request.address));
  if (!decoded) return AdmissionResult::rejected(RejectionReason::InvalidArgument);
  ingress.channel = decoded->channel;
  ingress.axi_interface = decoded->axi_interface;
  ingress.address_bytes = decoded->axi_local_bytes;
  ingress.axi_id = hbf::host::AxiId(static_cast<std::uint64_t>(request.source_id) + 1);
  ingress.packet_type = hbf::host::PacketType::FlashIo;
  ingress.operation = request.type == BridgeRequestType::Read
      ? hbf::host::HostOperation::Read : hbf::host::HostOperation::Write;
  ingress.size_bytes = static_cast<std::uint32_t>(request.size_bytes);
  ingress.payload = request.payload;

  const Token token(next_token_);
  auto callback = std::move(request.completion);
  SystemRequest system_request{token, std::move(ingress),
      [callback = std::move(callback)](SystemCompletion completion) mutable {
        callback(completion);
      }};
  const auto result = system_.try_submit(std::move(system_request));
  if (result.code == AdmissionCode::Accepted) {
    next_token_ = system_.allocate_request_token().value();
  }
  return result;
}

}  // namespace openhbx::integration
