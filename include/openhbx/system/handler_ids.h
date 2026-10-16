#pragma once

#include <cstdint>

#include "openhbx/common/strong_types.h"

namespace openhbx {

// HandlerId allocation (CHAN-008). 02 system and 06 PAL keep small stable ids;
// media channel slices occupy
// [kMediaSliceHandlerBase, kMediaSliceHandlerBase + kMaxHostChannels), indexed
// by channel value. OCP HBF v0.7.0 caps a stack at 16 Host Channels.
inline constexpr HandlerId kSystemHandlerId{1};
inline constexpr HandlerId kPalHandlerId{2};
inline constexpr std::uint64_t kMaxHostChannels{16};
inline constexpr std::uint64_t kMediaSliceHandlerBase{1024};

inline constexpr HandlerId media_slice_handler(ChannelId channel) {
  return HandlerId{kMediaSliceHandlerBase + channel.value()};
}

}  // namespace openhbx
