#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "openhbx/hbf/controller/base_die_flash_controller.h"
#include "openhbx/interconnect/fabric.h"
#include "openhbx/interconnect/tsv_repair.h"
#include "openhbx/pal/media_port.h"
#include "openhbx/system/event_queue.h"

using namespace openhbx;
using namespace openhbx::hbf;
using namespace openhbx::hbf::address;
using namespace openhbx::hbf::controller;
using namespace openhbx::interconnect;

namespace {
class FakeMedia final : public pal::IFlashMediaPort {
 public:
  AdmissionResult try_issue_media(media::FlashCommand command, Cycle) override {
    commands.push_back(std::move(command)); return AdmissionResult::accepted();
  }
  std::vector<media::FlashCommand> commands;
};
FabricProfile profile() {
  FabricProfile p; p.source = "open-hbx:synthetic-test"; p.queue_depth = 4;
  p.active_lanes = 8; p.spare_lanes = 0; p.bits_per_lane_per_cycle = 8;
  p.efficiency_ppm = 1000000; p.arbitration_cycles = 1; p.propagation_cycles = 1;
  p.routes = {{1, 0, {1}, {0}}}; return p;
}
PhysicalBank bank() { return {CoreDieIndex(0), DieIndex(0), BankIndex(0)}; }
DluKey key(std::uint64_t page) {
  return {{ChannelId(0), OwnedBankIndex(0), BlockIndex(0)}, PageIndex(page)};
}
ControllerRequest write(std::uint64_t token, std::uint64_t sector, std::uint64_t page = 0) {
  return {Token(token), Generation(0), HostOperation::WriteSector, key(page), bank(),
          SectorIndex(sector), 0, ReadMode::Regular,
          PayloadHandle::from_bytes(std::vector<std::uint8_t>(64,
              static_cast<std::uint8_t>(sector)))};
}
void dispatch(EventQueue& events, std::uint64_t cycle, Generation generation = Generation(0)) {
  events.dispatch_due(Cycle(cycle), EventPhase::Interconnect, generation);
}
}

int main() {
  auto geometry = HbfGeometry::create({1, 1, 1, 1, 2, 1, 1, 2, 64, 1});
  assert(geometry);
  BlockSequence sequences(*geometry);
  EventQueue events; TsvRepairManager repair(8, 0); InterconnectFabric fabric(profile(), repair);
  FakeMedia media;
  std::vector<ControllerCompletion> completions;
  BaseDieFlashController* controller_ptr = nullptr;
  pal::FlashPal pal({1, 2, 2, 1, 512, 4096}, events, HandlerId(70), fabric, media,
      [&](pal::PalCompletion c) { if (controller_ptr) controller_ptr->on_pal_completion(std::move(c)); });
  BaseDieFlashController controller({2, 20, 8, 2, 2, 64, 2000}, {bank()}, sequences, pal,
      [&](ControllerCompletion c) { completions.push_back(std::move(c)); });
  controller_ptr = &controller;

  // Occupy the sole PAL credit. The complete DLU must retain its S4 reservation
  // while PAL Busy has no PAL/event/token side effects.
  assert(pal.try_issue({Token(999), Generation(0), media::CommandKind::Read, ChannelId(0), {}, 0, {}},
                       Cycle(0)).code == AdmissionCode::Accepted);
  for (std::uint64_t i = 0; i < 64; ++i)
    assert(controller.submit(write(i + 1, i), Cycle(1)).code == AdmissionCode::Accepted);
  assert(controller.snapshot().pending_programs == 1);
  assert(sequences.snapshot(key(0).block).outstanding);
  assert(pal.snapshot().accepted == 1 && pal.snapshot().inflight == 1);
  assert(completions.empty());

  auto queued = events.snapshot();
  assert(queued.events.size() == 1 && queued.events[0].due == Cycle(10));
  dispatch(events, 10);
  assert(media.commands.size() == 1 && media.commands[0].token == Token(999));
  auto read_page = PayloadHandle::from_bytes(std::vector<std::uint8_t>(4096, 0x11));
  pal.on_media_completion({Token(999), media::MediaStatus::Success, true, read_page, Cycle(21)});
  queued = events.snapshot();
  assert(queued.events.size() == 1 && queued.events[0].due == Cycle(543));
  dispatch(events, 543);
  controller.pump(Cycle(544));
  assert(controller.snapshot().pending_programs == 0);
  assert(completions.empty());
  queued = events.snapshot();
  assert(queued.events.size() == 1 && queued.events[0].due == Cycle(554));
  dispatch(events, 554);
  assert(media.commands.size() == 2 && media.commands.back().kind == media::CommandKind::Erase);
  const auto erase_token = media.commands.back().token;
  pal.on_media_completion({erase_token, media::MediaStatus::Success, false, {}, Cycle(555)});
  queued = events.snapshot();
  assert(queued.events.size() == 1 && queued.events[0].due == Cycle(565));
  dispatch(events, 565);
  queued = events.snapshot();
  assert(queued.events.size() == 1 && queued.events[0].due == Cycle(1087));
  dispatch(events, 1087);
  assert(media.commands.size() == 3 && media.commands.back().kind == media::CommandKind::Program);
  const auto program_token = media.commands.back().token;
  pal.on_media_completion({program_token, media::MediaStatus::Success, false, {}, Cycle(1088)});
  assert(completions.empty());
  queued = events.snapshot();
  assert(queued.events.size() == 1 && queued.events[0].due == Cycle(1098));
  dispatch(events, 1098);
  assert(completions.size() == 64);
  for (const auto& c : completions)
    assert(c.status == ControllerStatus::Success && !c.data_valid);
  const auto sequence = sequences.snapshot(key(0).block);
  assert(sequence.expected_page == 1 && !sequence.outstanding);
  assert(controller.snapshot().accepted == controller.snapshot().terminal);

  // A Media-backed sector read returns only its addressed 64 B while the
  // Bank cache retains the complete 4 KiB DLU.
  completions.clear();
  ControllerRequest media_sector{Token(90), Generation(0), HostOperation::ReadSector, key(0),
      bank(), SectorIndex(2), 0, ReadMode::Regular, {}};
  assert(controller.submit(media_sector, Cycle(1200)).code == AdmissionCode::Accepted);
  queued = events.snapshot(); assert(queued.events[0].due == Cycle(1210));
  dispatch(events, 1210);
  std::vector<std::uint8_t> sector_pattern(4096);
  for (std::size_t i = 0; i < sector_pattern.size(); ++i)
    sector_pattern[i] = static_cast<std::uint8_t>(i / 64);
  auto media_page = PayloadHandle::from_bytes(std::move(sector_pattern));
  const auto read_token = media.commands.back().token;
  pal.on_media_completion({read_token, media::MediaStatus::Success, true, media_page, Cycle(1211)});
  queued = events.snapshot(); assert(queued.events[0].due == Cycle(1733));
  dispatch(events, 1733);
  assert(completions.size() == 1 && completions[0].payload.size() == 64 &&
         completions[0].payload.bytes()[0] == 2);
  ControllerRequest cached_dlu{Token(91), Generation(0), HostOperation::ReadDlu, key(0),
      bank(), SectorIndex(0), 0, ReadMode::Batch, {}};
  assert(controller.submit(cached_dlu, Cycle(1734)).code == AdmissionCode::Accepted);
  assert(completions.back().payload.size() == 4096);
  assert(controller.submit(cached_dlu, Cycle(1735)).code == AdmissionCode::Rejected);
  auto invalid_sector = cached_dlu;
  invalid_sector.token = Token(93); invalid_sector.operation = HostOperation::ReadSector;
  invalid_sector.sector = SectorIndex(64);
  const auto completion_count = completions.size();
  assert(controller.submit(invalid_sector, Cycle(1735)).code == AdmissionCode::Rejected);
  assert(completions.size() == completion_count);
  assert(controller.submit_admin(Token(92), Generation(0), AdminOpcode::SecureErase,
                                 false, Cycle(1736)).code == AdmissionCode::Accepted);
  assert(completions.back().status == ControllerStatus::Unsupported);

  // Accumulator forwarding and pending-missing reads never issue PAL work.
  completions.clear();
  assert(controller.submit(write(100, 3, 1), Cycle(1800)).code == AdmissionCode::Accepted);
  ControllerRequest forwarded{Token(101), Generation(0), HostOperation::ReadSector, key(1),
      bank(), SectorIndex(3), 0, ReadMode::Regular, {}};
  assert(controller.submit(forwarded, Cycle(1801)).code == AdmissionCode::Accepted);
  assert(completions.size() == 1 && completions[0].data_valid && completions[0].payload.size() == 64);
  forwarded.token = Token(102); forwarded.sector = SectorIndex(4);
  assert(controller.submit(forwarded, Cycle(1802)).code == AdmissionCode::Accepted);
  assert(completions.size() == 2 && completions.back().status == ControllerStatus::PendingDataMissing &&
         !completions.back().data_valid);
  assert(media.commands.size() == 4);

  // Table 13 status generation is through the public Controller facade.
  assert(controller.submit(write(103, 3, 1), Cycle(1803)).code == AdmissionCode::Accepted);
  assert(static_cast<std::uint8_t>(completions.back().status) == 0x2);
  assert(controller.submit(write(104, 0, 0), Cycle(1804)).code == AdmissionCode::Accepted);
  assert(controller.submit(write(105, 0, 2), Cycle(1805)).code == AdmissionCode::Accepted);
  assert(static_cast<std::uint8_t>(completions.back().status) == 0x4);
  controller.pump(Cycle(1820));
  assert(completions.back().token == Token(100));
  assert(static_cast<std::uint8_t>(completions.back().status) == 0x5);
  assert(!completions.back().data_valid);

  controller.reset(Generation(1), Cycle(1900));
  assert(completions.back().token == Token(104) &&
         completions.back().generation == Generation(0) &&
         completions.back().status == ControllerStatus::Aborted);
  assert(controller.snapshot().accepted == controller.snapshot().terminal);
}
