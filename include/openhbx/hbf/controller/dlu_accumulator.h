#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "openhbx/hbf/controller/controller_types.h"

namespace openhbx::hbf::controller {

enum class AccumulateCode { Accepted, Complete, Overlap, Limit, Invalid };
struct AccumulateResult { AccumulateCode code{AccumulateCode::Invalid}; PayloadHandle payload; };
enum class ProbeCode { Forward, PendingMissing, Miss };
struct ProbeResult { ProbeCode code{ProbeCode::Miss}; PayloadHandle payload; };
struct ExpiredDlu { DluKey key; std::vector<Token> tokens; };

class DluAccumulator {
 public:
  DluAccumulator(std::size_t max_pending, std::uint64_t timeout_cycles);
  AccumulateResult add_sector(const DluKey& key, address::SectorIndex sector,
                              PayloadHandle payload, Token token, Cycle now);
  ProbeResult probe_read(const DluKey& key, address::SectorIndex sector) const;
  std::vector<Token> tokens(const DluKey& key) const;
  std::vector<ExpiredDlu> expire(Cycle now);
  bool release(const DluKey& key);
  void reset();
  std::size_t pending() const noexcept { return pending_.size(); }

 private:
  struct Pending {
    std::uint64_t mask{0};
    bool complete{false};
    std::array<std::uint8_t, 4096> bytes{};
    std::array<Token, 64> tokens{};
    Cycle deadline;
  };
  // OCP per-channel subsystem alignment: max_pending_ bounds ONE channel's
  // pending DLUs; channels never share accumulator capacity.
  std::size_t max_pending_;
  std::uint64_t timeout_cycles_;
  std::map<DluKey, Pending> pending_;
  std::map<std::uint64_t, std::size_t> pending_by_channel_;
};

}  // namespace openhbx::hbf::controller
