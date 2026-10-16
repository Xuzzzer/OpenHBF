#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace openhbx {

template <typename Tag>
class StrongId {
 public:
  using value_type = std::uint64_t;
  constexpr StrongId() noexcept = default;
  explicit constexpr StrongId(value_type value) noexcept : value_(value) {}
  constexpr value_type value() const noexcept { return value_; }
  friend constexpr bool operator==(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(StrongId lhs, StrongId rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(StrongId lhs, StrongId rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator<=(StrongId lhs, StrongId rhs) noexcept { return !(rhs < lhs); }
  friend constexpr bool operator>=(StrongId lhs, StrongId rhs) noexcept { return !(lhs < rhs); }
 private:
  value_type value_{0};
};

struct CycleTag;
struct TokenTag;
struct GenerationTag;
struct TxnIdTag;
struct HandlerIdTag;
struct ChannelIdTag;
using Cycle = StrongId<CycleTag>;
using Token = StrongId<TokenTag>;
using Generation = StrongId<GenerationTag>;
using TxnId = StrongId<TxnIdTag>;
using HandlerId = StrongId<HandlerIdTag>;
// Host Channel identity is used by every module 02..07, so its single
// authoritative definition lives in common (ADR-CHAN-006). The address module
// re-exports it as hbf::address::ChannelId for source compatibility.
using ChannelId = StrongId<ChannelIdTag>;

static_assert(!std::is_convertible<std::uint64_t, Cycle>::value, "strong IDs must not implicitly convert");

}  // namespace openhbx
