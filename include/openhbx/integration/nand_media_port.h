#pragma once

#include "openhbx/pal/media_port.h"

namespace openhbx::integration {

// Late-binding forwarder between 06 PAL and the 07 media entry point. The
// target is the channel-dispatching NandMediaRouter; this port exists so the
// assembly can construct the PAL before or after the media slices without
// ordering constraints.
class NandMediaPort final : public pal::IFlashMediaPort {
 public:
  NandMediaPort() = default;
  explicit NandMediaPort(pal::IFlashMediaPort& target) : target_(&target) {}
  void connect(pal::IFlashMediaPort& target) noexcept { target_ = &target; }
  AdmissionResult try_issue_media(media::FlashCommand command, Cycle now) override;

 private:
  pal::IFlashMediaPort* target_{nullptr};
};

}  // namespace openhbx::integration
