#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <vector>
#include "openhbx/media/nand/die_domain.h"
#include "openhbx/media/nand/nand_channel_slice.h"

using namespace openhbx;
using namespace openhbx::media;
using namespace openhbx::media::nand;

namespace {
config::HbfGeometry make_geometry() {
  return {1, 1, 1, 2, 2, 4, 4096, 16, 65536};
}
std::uint64_t total_blocks(const config::HbfGeometry& g) {
  return g.core_dies * g.dies_per_core * g.banks_per_die * g.blocks_per_bank;
}
// Single-channel assembly: the one slice owns every flat bank id.
NandChannelSliceConfig make_config() {
  const config::HbfGeometry g = make_geometry();
  const MediaTopology topology(g);
  NandChannelSliceConfig config;
  config.geometry = g;
  config.channel = ChannelId(0);
  for (std::uint64_t bank = 0; bank < g.banks_per_die; ++bank)
    config.owned_banks.insert(topology.bank_id({0, 0, bank, 0, 0}));
  config.payload_mode = PayloadMode::Sparse;
  config.timing = StageTiming{};
  config.reliability = {7, 0};
  config.max_inflight = 8;
  return config;
}
void dispatch(EventQueue& q, Generation gen, std::uint64_t first, std::uint64_t last) {
  for (std::uint64_t c = first; c <= last; ++c)
    q.dispatch_due(Cycle(c), EventPhase::MediaCommit, gen);
}
bool status(const std::vector<MediaCompletion>& done, std::size_t i, MediaStatus s) {
  return done.size() > i && done[i].status == s;
}
}  // namespace

int main() {
  EventQueue q;
  std::vector<MediaCompletion> done;
  DieDomain domain(total_blocks(make_geometry()));
  NandChannelSlice media(make_config(), domain, q, HandlerId(71), [&](MediaCompletion c) { done.push_back(std::move(c)); });
  const PhysicalAddress a{0, 0, 0, 0, 0};
  const PhysicalAddress other{0, 0, 0, 1, 0};
  const std::vector<std::uint8_t> bytes(4096, 0xa5);

  if (media.try_issue({Token(1), Generation(0), CommandKind::Program, ChannelId(0), a,
                       PayloadHandle::from_bytes(bytes)}, Cycle(0)).code != AdmissionCode::Accepted)
    return EXIT_FAILURE;
  // Program has 2 + 20 + 3 cycles; completion must not occur early.
  dispatch(q, Generation(0), 0, 24);
  if (!done.empty() || media.page_is_valid(a)) return EXIT_FAILURE;
  dispatch(q, Generation(0), 25, 25);
  if (!status(done, 0, MediaStatus::Success) || !media.page_is_valid(a)) return EXIT_FAILURE;

  // Duplicate program is rejected without queue or state mutation.
  const auto before = media.snapshot();
  if (media.try_issue({Token(2), Generation(0), CommandKind::Program, ChannelId(0), a,
                       PayloadHandle::from_bytes(bytes)}, Cycle(26)).code != AdmissionCode::Rejected)
    return EXIT_FAILURE;
  const auto after = media.snapshot();
  if (before.inflight != after.inflight || before.queued_events != after.queued_events) return EXIT_FAILURE;

  if (media.try_issue({Token(3), Generation(0), CommandKind::Program, ChannelId(0), other,
                       PayloadHandle::from_bytes(bytes)}, Cycle(26)).code != AdmissionCode::Accepted)
    return EXIT_FAILURE;
  dispatch(q, Generation(0), 26, 60);
  if (!status(done, 1, MediaStatus::Success) || !media.page_is_valid(other)) return EXIT_FAILURE;

  // Erase only invalidates pages in its target block.
  if (media.try_issue({Token(4), Generation(0), CommandKind::Erase, ChannelId(0), a, {}}, Cycle(61)).code != AdmissionCode::Accepted)
    return EXIT_FAILURE;
  dispatch(q, Generation(0), 61, 106);
  if (!status(done, 2, MediaStatus::Success) || media.page_is_valid(a) || !media.page_is_valid(other) ||
      media.block_erase_count(a) != 1)
    return EXIT_FAILURE;

  // A read of an erased page is terminal but carries invalid data.
  if (media.try_issue({Token(5), Generation(0), CommandKind::Read, ChannelId(0), a, {}}, Cycle(107)).code != AdmissionCode::Accepted)
    return EXIT_FAILURE;
  dispatch(q, Generation(0), 107, 118);
  if (!status(done, 3, MediaStatus::ReadErasedPage) || done.back().data_valid || !done.back().payload.empty())
    return EXIT_FAILURE;
  // Verify raw outcomes against an independent status/data table.
  const ras::RawReadClass raw[] = {ras::RawReadClass::Clean, ras::RawReadClass::Correctable,
      ras::RawReadClass::Uncorrectable, ras::RawReadClass::Retry, ras::RawReadClass::Refresh};
  const MediaStatus expected[] = {MediaStatus::Success, MediaStatus::RawCorrectable,
      MediaStatus::RawUncorrectable, MediaStatus::RetrySuggested, MediaStatus::RefreshNotice};
  for (std::size_t i = 0; i < 5; ++i) {
    media.reliability().force_read(*media.topology().flatten_page(other), raw[i]);
    const auto start = 120 + i * 12;
    if (media.try_issue({Token(10 + i), Generation(0), CommandKind::Read, ChannelId(0), other, {}}, Cycle(start)).code != AdmissionCode::Accepted)
      return EXIT_FAILURE;
    const auto count = done.size();
    dispatch(q, Generation(0), start, start + 10);
    if (done.size() != count) return EXIT_FAILURE;
    dispatch(q, Generation(0), start + 11, start + 11);
    const bool valid = i == 0 || i == 1 || i == 4;
    if (!status(done, count, expected[i]) || done.back().completed_at != Cycle(start + 11) ||
        done.back().data_valid != valid || (valid ? done.back().payload.bytes() != bytes : !done.back().payload.empty()))
      return EXIT_FAILURE;
  }
  if (media.snapshot().inflight || media.snapshot().queued_events || done.size() != 9) return EXIT_FAILURE;

  // Reset before every Program stage boundary, then prove resource reuse.
  for (const auto reset_cycle : {1U, 2U, 21U, 22U, 24U, 25U}) {
    EventQueue events;
    std::vector<MediaCompletion> completions;
    DieDomain reset_domain(total_blocks(make_geometry()));
    auto cfg = make_config();
    cfg.max_inflight = 1;
    NandChannelSlice device(cfg, reset_domain, events, HandlerId(72), [&](MediaCompletion c) { completions.push_back(std::move(c)); });
    if (device.try_issue({Token(1), Generation(0), CommandKind::Program, ChannelId(0), a,
                          PayloadHandle::from_bytes(bytes)}, Cycle(0)).code != AdmissionCode::Accepted) return EXIT_FAILURE;
    const auto snapshot = device.snapshot();
    if (device.try_issue({Token(2), Generation(0), CommandKind::Read, ChannelId(0), other, {}}, Cycle(0)).code != AdmissionCode::Busy ||
        device.snapshot().inflight != snapshot.inflight || device.snapshot().queued_events != snapshot.queued_events)
      return EXIT_FAILURE;
    dispatch(events, Generation(0), 0, reset_cycle - 1);
    device.reset(Generation(1), Cycle(reset_cycle));
    dispatch(events, Generation(1), reset_cycle, 60);
    if (completions.size() != 1 || completions[0].status != MediaStatus::Aborted || device.page_is_valid(a) ||
        device.snapshot().inflight || device.snapshot().queued_events) return EXIT_FAILURE;
    if (device.try_issue({Token(1), Generation(1), CommandKind::Program, ChannelId(0), a,
                          PayloadHandle::from_bytes(bytes)}, Cycle(61)).code != AdmissionCode::Accepted) return EXIT_FAILURE;
    dispatch(events, Generation(1), 61, 86);
    if (completions.size() != 2 || completions.back().status != MediaStatus::Success || !device.page_is_valid(a)) return EXIT_FAILURE;
  }
  std::cout << "EXP-FLASH-SEMANTICS-001 PASS: commit, rejection, erase isolation, raw outcomes, backpressure, stage reset\n";
  return EXIT_SUCCESS;
}
