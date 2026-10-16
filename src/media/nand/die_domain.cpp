#include "openhbx/media/nand/die_domain.h"

namespace openhbx::media::nand {

DieDomain::DieDomain(std::uint64_t total_blocks) : total_blocks_(total_blocks) {}

bool DieDomain::die_blocked(std::uint64_t flat_die_id) const noexcept {
  return blocked_dies_.count(flat_die_id) != 0;
}

void DieDomain::set_die_blocked(std::uint64_t flat_die_id, bool blocked) {
  if (blocked)
    blocked_dies_.insert(flat_die_id);
  else
    blocked_dies_.erase(flat_die_id);
}

bool DieDomain::capacity_unusable() const noexcept {
  return bbt_.size() >= total_blocks_;
}

}  // namespace openhbx::media::nand
