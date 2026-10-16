#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "openhbx/common/payload_handle.h"
#include "openhbx/common/strong_types.h"

namespace openhbx::media {

struct PhysicalAddress {
  std::uint64_t core_die{0}, die{0}, bank{0}, block{0}, page{0};
  friend bool operator==(const PhysicalAddress& a, const PhysicalAddress& b) {
    return a.core_die == b.core_die && a.die == b.die && a.bank == b.bank &&
           a.block == b.block && a.page == b.page;
  }
};

enum class CommandKind { Read, Program, Erase };
enum class MediaStatus {
  Success, ReadErasedPage, RawCorrectable, RawUncorrectable, RetrySuggested,
  RefreshNotice, ProgramFail, EraseFail, CapacityUnusable,
  DieTemporarilyBlocked, BadBlock, Aborted, InvalidAddress, InvalidState,
  InvalidPayload, IntegrityError
};

struct FlashCommand {
  Token token;
  Generation generation;
  CommandKind kind{CommandKind::Read};
  // Owning Host Channel (CHAN-002). Legal iff
  // channel == owner_channel(address.bank); enforced structurally by the
  // channel slice's owned-bank membership check (CHAN-006).
  ChannelId channel{};
  PhysicalAddress address;
  PayloadHandle payload;
};

struct MediaCompletion {
  Token token;
  MediaStatus status{MediaStatus::Success};
  bool data_valid{false};
  PayloadHandle payload;
  Cycle completed_at;
};
using CompletionSink = std::function<void(MediaCompletion)>;

}  // namespace openhbx::media
