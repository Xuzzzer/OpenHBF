#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "openhbx/pal/flash_pal.h"

using namespace openhbx;
using namespace openhbx::interconnect;
using namespace openhbx::media;
using namespace openhbx::pal;

namespace {
FabricProfile profile(std::size_t depth = 8) {
  FabricProfile value;
  value.source = "open-hbx:synthetic-test";
  value.queue_depth = depth;
  value.active_lanes = 8;
  value.spare_lanes = 1;
  value.bits_per_lane_per_cycle = 8;
  value.efficiency_ppm = 1000000;
  value.arbitration_cycles = 1;
  value.propagation_cycles = 1;
  value.routes = {{1, 0, {1}, {0}}};
  return value;
}

class FakeMedia final : public IFlashMediaPort {
 public:
  AdmissionResult try_issue_media(FlashCommand command, Cycle now) override {
    ++calls;
    last_cycle = now;
    last = std::move(command);
    if (busy_count != 0) {
      --busy_count;
      return AdmissionResult::busy();
    }
    return reject ? AdmissionResult::rejected(RejectionReason::InvalidArgument)
                  : AdmissionResult::accepted();
  }
  std::uint64_t busy_count{0};
  bool reject{false};
  std::uint64_t calls{0};
  Cycle last_cycle;
  FlashCommand last;
};

void dispatch(EventQueue& events, Cycle cycle, Generation generation) {
  events.dispatch_due(cycle, EventPhase::Interconnect, generation);
}

FlashPhysicalRequest read_request(std::uint64_t token, Generation generation = Generation(0)) {
  return {Token(token), generation, CommandKind::Read, ChannelId(0), {}, 0, {}};
}

void verify_media_status_mapping(MediaStatus media_status, PalStatus pal_status,
                                 bool carries_read_data) {
  EventQueue events;
  TsvRepairManager repair(8, 0);
  InterconnectFabric fabric(profile(), repair);
  FakeMedia media;
  std::vector<PalCompletion> completions;
  FlashPal pal({1, 0, 0, 1, 512, 4096}, events, HandlerId(70), fabric, media,
               [&](PalCompletion completion) { completions.push_back(std::move(completion)); });
  assert(pal.try_issue(read_request(100), Cycle(0)).code == AdmissionCode::Accepted);
  dispatch(events, Cycle(10), Generation(0));
  PayloadHandle payload;
  if (carries_read_data)
    payload = PayloadHandle::from_bytes(std::vector<std::uint8_t>(4096, 0xa5));
  pal.on_media_completion({Token(100), media_status, carries_read_data, payload, Cycle(20)});
  dispatch(events, carries_read_data ? Cycle(542) : Cycle(30), Generation(0));
  assert(completions.size() == 1);
  assert(completions[0].status == pal_status);
  assert(completions[0].data_valid == carries_read_data);
}
}

int main() {
  EventQueue events;
  TsvRepairManager repair(8, 1);
  InterconnectFabric fabric(profile(), repair);
  FakeMedia media;
  std::vector<PalCompletion> completions;
  FlashPal pal({4, 2, 2, 1, 512, 4096}, events, HandlerId(61), fabric, media,
               [&](PalCompletion completion) { completions.push_back(std::move(completion)); });

  const auto rejected_payload = pal.try_issue(
      {Token(1), Generation(0), CommandKind::Read, ChannelId(0), {}, 0,
       PayloadHandle::from_bytes({1})}, Cycle(0));
  assert(rejected_payload.code == AdmissionCode::Rejected);
  assert(pal.snapshot().inflight == 0 && fabric.snapshot().reservation_count == 0);

  media.busy_count = 2;
  assert(pal.try_issue(read_request(2), Cycle(0)).code == AdmissionCode::Accepted);
  const auto forward_count = fabric.snapshot().reservation_count;
  assert(forward_count == 1);
  dispatch(events, Cycle(10), Generation(0));
  assert(media.calls == 1 && pal.snapshot().media_retries == 1);
  assert(fabric.snapshot().reservation_count == forward_count);
  dispatch(events, Cycle(11), Generation(0));
  assert(media.calls == 2 && pal.snapshot().media_retries == 2);
  assert(fabric.snapshot().reservation_count == forward_count);
  dispatch(events, Cycle(12), Generation(0));
  assert(media.calls == 3);
  std::vector<std::uint8_t> bytes(4096, 0x5a);
  auto payload = PayloadHandle::from_bytes(std::move(bytes));
  pal.on_media_completion({Token(2), MediaStatus::RawCorrectable, true, payload, Cycle(20)});
  assert(completions.empty());
  dispatch(events, Cycle(542), Generation(0));
  assert(completions.size() == 1);
  assert(completions[0].token == Token(2));
  assert(completions[0].status == PalStatus::RawCorrectable && completions[0].data_valid);
  assert(completions[0].payload.shares_storage_with(payload));
  auto snap = pal.snapshot();
  assert(snap.accepted == 1 && snap.terminal == 1 && snap.inflight == 0);

  pal.on_media_completion({Token(2), MediaStatus::Success, true, payload, Cycle(600)});
  assert(completions.size() == 1 && pal.snapshot().stale_events == 1);

  assert(pal.try_issue(read_request(3), Cycle(600)).code == AdmissionCode::Accepted);
  assert(repair.apply_fault(0).code == RepairCode::Repaired);
  dispatch(events, Cycle(610), Generation(0));
  assert(completions.size() == 2);
  assert(completions.back().status == PalStatus::PathUnavailable);
  assert(media.calls == 3);

  assert(pal.try_issue(read_request(4), Cycle(700)).code == AdmissionCode::Accepted);
  pal.reset(Generation(1), Cycle(701));
  assert(completions.size() == 3 && completions.back().status == PalStatus::Aborted);
  assert(!completions.back().data_valid);
  assert(pal.snapshot().inflight == 0 && fabric.snapshot().active_reservations == 0);
  assert(events.empty());
  dispatch(events, Cycle(1000), Generation(1));
  assert(completions.size() == 3);
  assert(pal.try_issue(read_request(4, Generation(1)), Cycle(1000)).code ==
         AdmissionCode::Rejected);

  FakeMedia always_busy;
  EventQueue retry_events;
  TsvRepairManager retry_repair(8, 0);
  InterconnectFabric retry_fabric(profile(), retry_repair);
  std::vector<PalCompletion> retry_completions;
  always_busy.busy_count = 9;
  FlashPal retry_pal({2, 1, 1, 1, 512, 4096}, retry_events, HandlerId(62), retry_fabric,
                     always_busy, [&](PalCompletion completion) {
                       retry_completions.push_back(std::move(completion));
                     });
  assert(retry_pal.try_issue(read_request(20), Cycle(0)).code == AdmissionCode::Accepted);
  dispatch(retry_events, Cycle(10), Generation(0));
  dispatch(retry_events, Cycle(11), Generation(0));
  assert(retry_completions.empty());
  dispatch(retry_events, Cycle(21), Generation(0));
  assert(retry_completions.size() == 1);
  assert(retry_completions[0].status == PalStatus::InternalError);
  assert(!retry_completions[0].data_valid);
  assert(retry_pal.snapshot().accepted == retry_pal.snapshot().terminal);

  EventQueue malformed_events;
  TsvRepairManager malformed_repair(8, 0);
  InterconnectFabric malformed_fabric(profile(), malformed_repair);
  FakeMedia malformed_media;
  std::vector<PalCompletion> malformed_completions;
  FlashPal malformed_pal({2, 1, 1, 1, 512, 4096}, malformed_events, HandlerId(63),
                         malformed_fabric, malformed_media,
                         [&](PalCompletion completion) {
                           malformed_completions.push_back(std::move(completion));
                         });
  assert(malformed_pal.try_issue(read_request(30), Cycle(0)).code == AdmissionCode::Accepted);
  dispatch(malformed_events, Cycle(10), Generation(0));
  malformed_pal.on_media_completion(
      {Token(30), MediaStatus::Success, false, {}, Cycle(20)});
  dispatch(malformed_events, Cycle(30), Generation(0));
  assert(malformed_completions.size() == 1);
  assert(malformed_completions[0].status == PalStatus::InternalError);
  assert(!malformed_completions[0].data_valid && malformed_completions[0].payload.empty());
  assert(malformed_pal.snapshot().integrity_errors == 1);

  auto program_payload = PayloadHandle::from_bytes(std::vector<std::uint8_t>(4096, 0x33));
  assert(malformed_pal.try_issue(
      {Token(31), Generation(0), CommandKind::Program, ChannelId(0), {}, 0, program_payload},
      Cycle(40)).code == AdmissionCode::Accepted);
  dispatch(malformed_events, Cycle(562), Generation(0));
  malformed_pal.on_media_completion(
      {Token(31), MediaStatus::Success, true, program_payload, Cycle(570)});
  dispatch(malformed_events, Cycle(580), Generation(0));
  assert(malformed_completions.size() == 2);
  assert(malformed_completions.back().status == PalStatus::InternalError);
  assert(!malformed_completions.back().data_valid && malformed_completions.back().payload.empty());
  assert(malformed_pal.snapshot().integrity_errors == 2);

  assert(malformed_pal.try_issue(read_request(32), Cycle(600)).code == AdmissionCode::Accepted);
  dispatch(malformed_events, Cycle(610), Generation(0));
  auto short_payload = PayloadHandle::from_bytes({0x7f});
  malformed_pal.on_media_completion(
      {Token(32), MediaStatus::RawCorrectable, true, short_payload, Cycle(620)});
  dispatch(malformed_events, Cycle(630), Generation(0));
  assert(malformed_completions.size() == 3);
  assert(malformed_completions.back().status == PalStatus::InternalError);
  assert(!malformed_completions.back().data_valid && malformed_completions.back().payload.empty());
  assert(malformed_pal.snapshot().integrity_errors == 3);

  EventQueue split_events;
  TsvRepairManager split_repair(8, 0);
  InterconnectFabric split_fabric(profile(1), split_repair);
  FakeMedia split_media;
  split_media.busy_count = 1;
  std::vector<PalCompletion> split_completions;
  FlashPal split_pal({2, 1, 2, 1, 512, 4096}, split_events, HandlerId(64), split_fabric,
                     split_media, [&](PalCompletion completion) {
                       split_completions.push_back(std::move(completion));
                     });
  assert(split_pal.try_issue(read_request(40), Cycle(0)).code == AdmissionCode::Accepted);
  dispatch(split_events, Cycle(10), Generation(0));
  dispatch(split_events, Cycle(11), Generation(0));
  auto split_route = split_fabric.route_for(0);
  assert(split_route);
  assert(split_fabric.try_reserve(
      {99, Direction::Forward, TrafficClass::Command, 640, *split_route}, Cycle(11)).code ==
         ReservationCode::Accepted);
  split_pal.on_media_completion({Token(40), MediaStatus::ReadErasedPage, false, {}, Cycle(11)});
  assert(split_pal.snapshot().media_retries == 1);
  assert(split_pal.snapshot().return_retries == 1);
  assert(split_fabric.release(99));
  dispatch(split_events, Cycle(12), Generation(0));
  // Forward and return directions are independent. The retried 512-bit
  // status return starts at cycle 12 and completes at cycle 22.
  dispatch(split_events, Cycle(22), Generation(0));
  assert(split_completions.size() == 1 &&
         split_completions[0].status == PalStatus::ReadErasedPage);
  assert(split_pal.snapshot().return_retries == 1);

  verify_media_status_mapping(MediaStatus::ReadErasedPage, PalStatus::ReadErasedPage, false);
  verify_media_status_mapping(MediaStatus::RawCorrectable, PalStatus::RawCorrectable, true);
  verify_media_status_mapping(MediaStatus::RawUncorrectable, PalStatus::RawUncorrectable, false);
  verify_media_status_mapping(MediaStatus::RetrySuggested, PalStatus::RetrySuggested, false);
  verify_media_status_mapping(MediaStatus::RefreshNotice, PalStatus::RefreshNotice, true);
  verify_media_status_mapping(MediaStatus::ProgramFail, PalStatus::ProgramFail, false);
  verify_media_status_mapping(MediaStatus::EraseFail, PalStatus::EraseFail, false);
  verify_media_status_mapping(MediaStatus::CapacityUnusable, PalStatus::CapacityUnusable, false);
  verify_media_status_mapping(MediaStatus::DieTemporarilyBlocked,
                              PalStatus::DieTemporarilyBlocked, false);
  verify_media_status_mapping(MediaStatus::BadBlock, PalStatus::BadBlock, false);
  verify_media_status_mapping(MediaStatus::InvalidAddress, PalStatus::InvalidAddress, false);
  verify_media_status_mapping(MediaStatus::InvalidState, PalStatus::InvalidState, false);
  verify_media_status_mapping(MediaStatus::InvalidPayload, PalStatus::InvalidPayload, false);
  verify_media_status_mapping(MediaStatus::IntegrityError, PalStatus::MediaIntegrityError, false);

  // CHAN-013: max_inflight bounds ONE channel's PAL slice. Filling channel 0
  // must not consume channel 1's in-flight budget, and a channel-0 terminal
  // reopens only channel 0.
  EventQueue iso_events;
  TsvRepairManager iso_repair(8, 0);
  InterconnectFabric iso_fabric(profile(), iso_repair);
  FakeMedia iso_media;
  std::vector<PalCompletion> iso_completions;
  FlashPal iso_pal({2, 1, 1, 1, 512, 4096}, iso_events, HandlerId(65), iso_fabric,
                   iso_media, [&](PalCompletion completion) {
                     iso_completions.push_back(std::move(completion));
                   });
  const auto iso_read = [](std::uint64_t token, std::uint64_t channel) {
    return FlashPhysicalRequest{Token(token), Generation(0), CommandKind::Read,
                                ChannelId(channel), {}, 0, {}};
  };
  assert(iso_pal.try_issue(iso_read(41, 0), Cycle(0)).code == AdmissionCode::Accepted);
  assert(iso_pal.try_issue(iso_read(42, 0), Cycle(0)).code == AdmissionCode::Accepted);
  assert(iso_pal.try_issue(iso_read(43, 0), Cycle(0)).code == AdmissionCode::Busy);
  assert(iso_pal.try_issue(iso_read(44, 1), Cycle(0)).code == AdmissionCode::Accepted);
  assert(iso_pal.try_issue(iso_read(45, 1), Cycle(0)).code == AdmissionCode::Accepted);
  assert(iso_pal.try_issue(iso_read(46, 1), Cycle(0)).code == AdmissionCode::Busy);
  assert(iso_pal.snapshot().inflight == 4);
  // Four 512-bit forwards serialize on the shared test route: ends 10/20/30/40.
  // The event queue dispatches in due order, so drain every forward first.
  dispatch(iso_events, Cycle(10), Generation(0));
  dispatch(iso_events, Cycle(20), Generation(0));
  dispatch(iso_events, Cycle(30), Generation(0));
  dispatch(iso_events, Cycle(40), Generation(0));
  iso_pal.on_media_completion(
      {Token(41), MediaStatus::Success, true,
       PayloadHandle::from_bytes(std::vector<std::uint8_t>(4096, 0x5a)), Cycle(10)});
  // Return direction has an independent EAT (full duplex): the 33280-bit
  // read-data return starts at cycle 10 and completes at 10 + 522 = 532.
  dispatch(iso_events, Cycle(532), Generation(0));
  assert(iso_completions.size() == 1 && iso_completions[0].token == Token(41));
  assert(iso_completions[0].status == PalStatus::Success &&
         iso_completions[0].data_valid);
  assert(iso_pal.snapshot().inflight == 3);
  assert(iso_pal.try_issue(iso_read(47, 0), Cycle(532)).code == AdmissionCode::Accepted);
  assert(iso_pal.try_issue(iso_read(48, 1), Cycle(532)).code == AdmissionCode::Busy);
  const auto iso_snap = iso_pal.snapshot();
  assert(iso_snap.accepted == 5 && iso_snap.terminal == 1 && iso_snap.inflight == 4);
}
