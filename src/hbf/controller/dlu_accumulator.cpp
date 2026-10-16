#include "openhbx/hbf/controller/dlu_accumulator.h"

#include <algorithm>
#include <limits>

namespace openhbx::hbf::controller {
DluAccumulator::DluAccumulator(std::size_t max_pending, std::uint64_t timeout_cycles)
    : max_pending_(max_pending), timeout_cycles_(timeout_cycles) {}

// Controller 聚合粒度：4 KiB DLU， DLU = device logical unit
AccumulateResult DluAccumulator::add_sector(const DluKey& key,
    address::SectorIndex sector, PayloadHandle payload, Token token, Cycle now) {
  const auto index = sector.value();
  if (index >= 64 || payload.size() != 64 || token.value() == 0 ||
      timeout_cycles_ == 0) return {AccumulateCode::Invalid, {}};
  auto it = pending_.find(key);
  if (it == pending_.end()) {
    const auto channel = key.block.channel.value();
    if (pending_by_channel_[channel] >= max_pending_ ||
        now.value() > std::numeric_limits<std::uint64_t>::max() - timeout_cycles_)
      return {AccumulateCode::Limit, {}};
    Pending value;
    value.deadline = Cycle(now.value() + timeout_cycles_);
    it = pending_.emplace(key, std::move(value)).first;
    ++pending_by_channel_[channel];
  }
  const auto bit = std::uint64_t{1} << index;
  if ((it->second.mask & bit) != 0) return {AccumulateCode::Overlap, {}};
  std::copy(payload.bytes().begin(), payload.bytes().end(),
            it->second.bytes.begin() + static_cast<std::ptrdiff_t>(index * 64));
  it->second.tokens[index] = token;
  it->second.mask |= bit;
  if (it->second.mask == std::numeric_limits<std::uint64_t>::max()) {
    it->second.complete = true;
    return {AccumulateCode::Complete,
            PayloadHandle::from_bytes(std::vector<std::uint8_t>(
                it->second.bytes.begin(), it->second.bytes.end()))};
  }
  return {AccumulateCode::Accepted, {}};
}

ProbeResult DluAccumulator::probe_read(const DluKey& key,
                                       address::SectorIndex sector) const {
  if (sector.value() >= 64) return {};
  const auto it = pending_.find(key);
  if (it == pending_.end()) return {};
  const auto bit = std::uint64_t{1} << sector.value();
  if ((it->second.mask & bit) == 0) return {ProbeCode::PendingMissing, {}};
  const auto begin = it->second.bytes.begin() +
      static_cast<std::ptrdiff_t>(sector.value() * 64);
  return {ProbeCode::Forward,
          PayloadHandle::from_bytes(std::vector<std::uint8_t>(begin, begin + 64))};
}

std::vector<Token> DluAccumulator::tokens(const DluKey& key) const {
  std::vector<Token> result;
  const auto it = pending_.find(key);
  if (it == pending_.end()) return result;
  for (std::size_t i = 0; i < 64; ++i)
    if ((it->second.mask & (std::uint64_t{1} << i)) != 0) result.push_back(it->second.tokens[i]);
  return result;
}

std::vector<ExpiredDlu> DluAccumulator::expire(Cycle now) {
  std::vector<ExpiredDlu> result;
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (!it->second.complete && it->second.deadline <= now) {
      auto current = it++;
      result.push_back({current->first, tokens(current->first)});
      const auto channel = current->first.block.channel.value();
      auto used = pending_by_channel_.find(channel);
      if (used != pending_by_channel_.end() && used->second != 0) {
        if (--used->second == 0) pending_by_channel_.erase(used);
      }
      pending_.erase(current);
    } else ++it;
  }
  return result;
}
bool DluAccumulator::release(const DluKey& key) {
  const auto it = pending_.find(key);
  if (it == pending_.end()) return false;
  const auto channel = key.block.channel.value();
  auto used = pending_by_channel_.find(channel);
  if (used != pending_by_channel_.end() && used->second != 0) {
    if (--used->second == 0) pending_by_channel_.erase(used);
  }
  pending_.erase(it);
  return true;
}
void DluAccumulator::reset() { pending_.clear(); pending_by_channel_.clear(); }
}  // namespace openhbx::hbf::controller
