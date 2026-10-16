#pragma once

#include <cstdint>
#include <set>

#include "openhbx/ras/bad_block_table.h"

namespace openhbx::media::nand {

// Die-level shared physical state of one NAND stack (CHAN-005). Blocked dies
// and the bad-block table use physical (flat) die/block identity and are
// deliberately NOT replicated per channel: a die blocked by temperature or
// recovery affects every channel that owns banks on it, and a physical block
// is bad for the whole stack. Per-channel slice engines hold a reference and
// query it at stage boundaries; only this object owns writes.
//
// The bad-block table and blocked-die set intentionally survive media reset,
// matching the previous monolithic device behavior: physical bad-block
// identity and injected die blocking are not runtime command state.
// When MEDIA-008 unblocks, the die temperature timeline (LLD_07's planned
// DieEnvironment role) belongs here, not in a channel slice.
class DieDomain {
 public:
  explicit DieDomain(std::uint64_t total_blocks);

  bool die_blocked(std::uint64_t flat_die_id) const noexcept;
  void set_die_blocked(std::uint64_t flat_die_id, bool blocked);

  ras::BadBlockTable& bad_block_table() noexcept { return bbt_; }
  const ras::BadBlockTable& bad_block_table() const noexcept { return bbt_; }

  // CapacityUnusable is a stack-wide judgment and therefore needs this global
  // view (CHAN-009), not a per-slice one.
  bool capacity_unusable() const noexcept;

  std::uint64_t total_blocks() const noexcept { return total_blocks_; }

 private:
  std::uint64_t total_blocks_{0};
  std::set<std::uint64_t> blocked_dies_;
  ras::BadBlockTable bbt_;
};

}  // namespace openhbx::media::nand
