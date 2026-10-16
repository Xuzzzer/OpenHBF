#pragma once

#include <vector>

#include "openhbx/pal/media_port.h"

namespace openhbx::media::nand {

class NandChannelSlice;

// 07's implementation of the 06 media port (ADR-CHAN-003): dispatches each
// FlashCommand to the slice of its explicit channel (CHAN-002). The router is
// stateless and does NOT duplicate 04's ownership table — the slice's
// owned-bank membership check is the ownership enforcement (CHAN-006,
// ADR-CHAN-004).
class NandMediaRouter : public pal::IFlashMediaPort {
 public:
  // Slices are indexed by channel value and not owned here; the system
  // assembly (02) owns them and guarantees they outlive the router.
  explicit NandMediaRouter(std::vector<NandChannelSlice*> slices_by_channel)
      : slices_(std::move(slices_by_channel)) {}

  AdmissionResult try_issue_media(FlashCommand command, Cycle now) override {
    if (command.channel.value() >= slices_.size())
      return AdmissionResult::rejected(RejectionReason::InvalidArgument,
                                       "flash command channel out of range");
    return slices_[command.channel.value()]->try_issue(std::move(command), now);
  }

  std::size_t slice_count() const noexcept { return slices_.size(); }

 private:
  std::vector<NandChannelSlice*> slices_;
};

}  // namespace openhbx::media::nand
