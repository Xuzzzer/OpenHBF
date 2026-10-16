#include "openhbx/integration/nand_media_port.h"

#include <utility>

namespace openhbx::integration {

AdmissionResult NandMediaPort::try_issue_media(media::FlashCommand command, Cycle now) {
  if (target_ == nullptr)
    return AdmissionResult::rejected(RejectionReason::InvalidLifecycle,
                                     "NAND media port is not connected");
  return target_->try_issue_media(std::move(command), now);
}

}  // namespace openhbx::integration
