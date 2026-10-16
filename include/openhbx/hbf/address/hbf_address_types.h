#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "openhbx/common/strong_types.h"

namespace openhbx::hbf::address {

template <typename Tag>
class AddressValue {
 public:
  constexpr AddressValue() noexcept = default;
  explicit constexpr AddressValue(std::uint64_t value) noexcept : value_(value) {}
  constexpr std::uint64_t value() const noexcept { return value_; }
  friend constexpr bool operator==(AddressValue lhs, AddressValue rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(AddressValue lhs, AddressValue rhs) noexcept {
    return !(lhs == rhs);
  }
  friend constexpr bool operator<(AddressValue lhs, AddressValue rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }

 private:
  std::uint64_t value_{0};
};

struct LocalByteAddressTag;
struct A1Units64Tag;
struct DluIndexTag;
struct CoreDieIndexTag;
struct DieIndexTag;
struct BankIndexTag;
struct OwnedBankIndexTag;
struct BlockIndexTag;
struct PageIndexTag;
struct SectorIndexTag;
struct ZoneIdTag;
struct AddressViewEpochTag;

using LocalByteAddress = AddressValue<LocalByteAddressTag>;
using A1Units64 = AddressValue<A1Units64Tag>;
using DluIndex = AddressValue<DluIndexTag>;
// ChannelId is defined once in openhbx/common/strong_types.h (ADR-CHAN-006);
// this alias keeps the address module's public names unchanged.
using ChannelId = openhbx::ChannelId;
using CoreDieIndex = AddressValue<CoreDieIndexTag>;
using DieIndex = AddressValue<DieIndexTag>;
using BankIndex = AddressValue<BankIndexTag>;
using OwnedBankIndex = AddressValue<OwnedBankIndexTag>;
using BlockIndex = AddressValue<BlockIndexTag>;
using PageIndex = AddressValue<PageIndexTag>;
using SectorIndex = AddressValue<SectorIndexTag>;
using ZoneId = AddressValue<ZoneIdTag>;
using AddressViewEpoch = AddressValue<AddressViewEpochTag>;

struct PhysicalBank {
  CoreDieIndex core_die;
  DieIndex die;
  BankIndex bank;
  friend bool operator==(const PhysicalBank& lhs, const PhysicalBank& rhs) noexcept {
    return lhs.core_die == rhs.core_die && lhs.die == rhs.die && lhs.bank == rhs.bank;
  }
  friend bool operator<(const PhysicalBank& lhs, const PhysicalBank& rhs) noexcept {
    if (lhs.core_die != rhs.core_die) return lhs.core_die < rhs.core_die;
    if (lhs.die != rhs.die) return lhs.die < rhs.die;
    return lhs.bank < rhs.bank;
  }
};

struct HbfAddress {
  ChannelId channel;
  PhysicalBank physical_bank;
  OwnedBankIndex owned_bank;
  BlockIndex block;
  PageIndex page;
  SectorIndex sector;
  friend bool operator==(const HbfAddress& lhs, const HbfAddress& rhs) noexcept {
    return lhs.channel == rhs.channel && lhs.physical_bank == rhs.physical_bank &&
           lhs.owned_bank == rhs.owned_bank && lhs.block == rhs.block &&
           lhs.page == rhs.page && lhs.sector == rhs.sector;
  }
};

struct BlockKey {
  ChannelId channel;
  OwnedBankIndex owned_bank;
  BlockIndex block;
  friend bool operator==(const BlockKey& lhs, const BlockKey& rhs) noexcept {
    return lhs.channel == rhs.channel && lhs.owned_bank == rhs.owned_bank &&
           lhs.block == rhs.block;
  }
  friend bool operator<(const BlockKey& lhs, const BlockKey& rhs) noexcept {
    if (lhs.channel != rhs.channel) return lhs.channel < rhs.channel;
    if (lhs.owned_bank != rhs.owned_bank) return lhs.owned_bank < rhs.owned_bank;
    return lhs.block < rhs.block;
  }
};

enum class AddressError {
  None,
  Misaligned,
  OutOfRange,
  Overflow,
  WrongOwner,
  NonCanonical,
  Retired,
  InvalidGeometry,
};

struct MappedAddress {
  HbfAddress address;
  AddressViewEpoch epoch;
  A1Units64 a1;
  std::uint64_t l1;
  std::uint64_t b1;
  std::uint64_t p1;
  std::uint64_t bank_num;
  std::uint64_t l2;
};

struct MapResult {
  std::optional<MappedAddress> mapped;
  AddressError error{AddressError::None};
  std::string detail;
  explicit operator bool() const noexcept { return mapped.has_value(); }
};

struct ReverseResult {
  std::optional<LocalByteAddress> address;
  AddressError error{AddressError::None};
  explicit operator bool() const noexcept { return address.has_value(); }
};

const char* to_string(AddressError error) noexcept;

}  // namespace openhbx::hbf::address
