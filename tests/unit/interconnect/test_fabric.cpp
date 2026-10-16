#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "openhbx/interconnect/fabric.h"

using namespace openhbx;
using namespace openhbx::interconnect;

namespace {
FabricProfile profile(std::size_t depth = 4) {
  FabricProfile value;
  value.source = "open-hbx:synthetic-test";
  value.queue_depth = depth;
  value.active_lanes = 4;
  value.spare_lanes = 1;
  value.bits_per_lane_per_cycle = 16;
  value.efficiency_ppm = 1000000;
  value.arbitration_cycles = 1;
  value.propagation_cycles = 2;
  value.routes = {{1, 7, {10, 11}, {0, 1}}, {2, 8, {12}, {2}}};
  return value;
}
}

int main() {
  TsvRepairManager repair(4, 1);
  InterconnectFabric fabric(profile(), repair);
  auto route_a = fabric.route_for(7);
  auto route_b = fabric.route_for(8);
  assert(route_a && route_b);
  assert(route_a->physical_lanes == std::vector<std::uint64_t>({0, 1}));

  auto first = fabric.try_reserve({1, Direction::Forward, TrafficClass::Command,
                                   128, *route_a}, Cycle(3));
  assert(first.code == ReservationCode::Accepted);
  assert(first.start == Cycle(3));
  assert(first.end == Cycle(8));
  auto shared = fabric.try_reserve({2, Direction::Forward, TrafficClass::Command,
                                    64, *route_a}, Cycle(4));
  assert(shared.start == Cycle(8) && shared.end == Cycle(12));
  auto full_duplex = fabric.try_reserve({6, Direction::Return, TrafficClass::ReadData,
                                         64, *route_a}, Cycle(4));
  assert(full_duplex.start == Cycle(4) && full_duplex.end == Cycle(8));
  assert(fabric.release(6));
  auto independent = fabric.try_reserve({3, Direction::Forward, TrafficClass::Command,
                                         64, *route_b}, Cycle(4));
  assert(independent.start == Cycle(4) && independent.end == Cycle(8));

  const auto before_invalid = fabric.snapshot();
  Route forged = *route_a;
  forged.resources = {99};
  const auto invalid = fabric.try_reserve({4, Direction::Forward, TrafficClass::Command,
                                           64, forged}, Cycle(4));
  assert(invalid.code == ReservationCode::InvalidRoute);
  const auto after_invalid = fabric.snapshot();
  assert(before_invalid.resource_eat == after_invalid.resource_eat);
  assert(before_invalid.active_reservations == after_invalid.active_reservations);
  assert(before_invalid.reservation_count == after_invalid.reservation_count);

  fabric.reset();
  const auto clean = fabric.snapshot();
  assert(clean.active_reservations == 0 && clean.resource_eat.empty());
  const auto before_overflow = fabric.snapshot();
  const auto overflow = fabric.try_reserve(
      {4, Direction::Forward, TrafficClass::Command,
       std::numeric_limits<std::uint64_t>::max(), *route_a},
      Cycle(std::numeric_limits<std::uint64_t>::max() - 1));
  assert(overflow.code == ReservationCode::Overflow);
  const auto after_overflow = fabric.snapshot();
  assert(before_overflow.resource_eat == after_overflow.resource_eat);
  assert(before_overflow.active_reservations == after_overflow.active_reservations);

  const auto old_epoch = route_a->repair_epoch;
  const auto repaired = repair.apply_fault(0);
  assert(repaired.code == RepairCode::Repaired && repaired.epoch == old_epoch + 1);
  assert(!repair.is_operational(*route_a));
  auto new_route = fabric.route_for(7);
  assert(new_route && new_route->repair_epoch == repaired.epoch);
  assert(new_route->physical_lanes[0] == 4);
  const auto old_reservation = fabric.try_reserve(
      {5, Direction::Forward, TrafficClass::Command, 64, *route_a}, Cycle(0));
  assert(old_reservation.code == ReservationCode::PathUnavailable);
  assert(fabric.try_reserve({5, Direction::Forward, TrafficClass::Command,
                             64, *new_route}, Cycle(0)).code == ReservationCode::Accepted);

  FabricProfile hybrid = profile();
  hybrid.technology = FabricTechnology::TsvHybridBonding;
  assert(!hybrid.validate().empty());
  hybrid.source = "vendor:qualified-profile-v1";
  assert(hybrid.validate().empty());
  assert(hybrid.theoretical_bits_per_cycle() == 64);

  FabricProfile grouped_profile = profile();
  grouped_profile.channel_count = 2;
  grouped_profile.routes = {
      {1, 0, {1}, {0, 1, 2, 3}},
      {2, 1, {2}, {4, 5, 6, 7}},
  };
  assert(grouped_profile.validate().empty());
  assert(grouped_profile.total_active_lanes() == 8);
  TsvRepairManager grouped_repair(2, 4, 1);
  const auto initial_grouped = grouped_repair.snapshot();
  assert(initial_grouped.active_to_physical.at(0) == 0);
  assert(initial_grouped.active_to_physical.at(4) == 5);
  assert(initial_grouped.free_spares == std::set<std::uint64_t>({4, 9}));
  const auto channel_one_repair = grouped_repair.apply_fault(5);
  assert(channel_one_repair.code == RepairCode::Repaired);
  assert(channel_one_repair.replacement == 9);
  const auto after_grouped = grouped_repair.snapshot();
  assert(after_grouped.free_spares == std::set<std::uint64_t>({4}));
  Route grouped_route;
  assert(grouped_repair.resolve(grouped_profile.routes[1], grouped_route));
  assert(grouped_route.logical_lanes.size() == 4);
  assert(grouped_route.physical_lanes.front() == 9);

  // CHAN-013: queue_depth is charged per route resource (in production one
  // channel path per route), so saturating route A never consumes route B's
  // transfer budget.
  TsvRepairManager isolation_repair(4, 1);
  InterconnectFabric isolation_fabric(profile(1), isolation_repair);
  const auto iso_a = isolation_fabric.route_for(7);
  const auto iso_b = isolation_fabric.route_for(8);
  assert(isolation_fabric.try_reserve({10, Direction::Forward, TrafficClass::Command,
                                       64, *iso_a}, Cycle(0)).code == ReservationCode::Accepted);
  assert(isolation_fabric.try_reserve({11, Direction::Forward, TrafficClass::Command,
                                       64, *iso_a}, Cycle(0)).code == ReservationCode::Busy);
  assert(isolation_fabric.try_reserve({12, Direction::Forward, TrafficClass::Command,
                                       64, *iso_b}, Cycle(0)).code == ReservationCode::Accepted);
  assert(isolation_fabric.try_reserve({13, Direction::Forward, TrafficClass::Command,
                                       64, *iso_b}, Cycle(0)).code == ReservationCode::Busy);
  assert(isolation_fabric.release(10));
  assert(isolation_fabric.try_reserve({14, Direction::Forward, TrafficClass::Command,
                                       64, *iso_a}, Cycle(0)).code == ReservationCode::Accepted);
  assert(isolation_fabric.try_reserve({15, Direction::Forward, TrafficClass::Command,
                                       64, *iso_b}, Cycle(0)).code == ReservationCode::Busy);

  bool threw = false;
  try {
    FabricProfile bad = profile();
    bad.routes[0].resources = {10, 10};
    InterconnectFabric unused(std::move(bad), repair);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
}
