// CHAN-T06/T07/T09/T11/T12: channel-partitioned media component verification.
// Assembly under test: one shared DieDomain + two NandChannelSlice instances
// behind the NandMediaRouter (the same shape OpenHbxSystem builds).
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <vector>
#include "openhbx/media/nand/die_domain.h"
#include "openhbx/media/nand/nand_channel_slice.h"
#include "openhbx/media/nand/nand_media_router.h"

using namespace openhbx;
using namespace openhbx::media;
using namespace openhbx::media::nand;

namespace {
// Two Host Channels on one core die; synthetic ownership bank % channels.
config::HbfGeometry make_geometry() {
  return {2, 1, 1, 4, 2, 4, 4096, 32, 131072};
}
std::uint64_t total_blocks(const config::HbfGeometry& g) {
  return g.core_dies * g.dies_per_core * g.banks_per_die * g.blocks_per_bank;
}
NandChannelSliceConfig make_slice_config(std::uint64_t channel) {
  const config::HbfGeometry g = make_geometry();
  const MediaTopology topology(g);
  NandChannelSliceConfig config;
  config.geometry = g;
  config.channel = ChannelId(channel);
  for (std::uint64_t bank = channel; bank < g.banks_per_die; bank += g.host_channels)
    config.owned_banks.insert(topology.bank_id({0, 0, bank, 0, 0}));
  config.payload_mode = PayloadMode::Sparse;
  config.timing = StageTiming{};
  config.reliability = {42, 0};
  config.max_inflight = 8;
  return config;
}
void dispatch(EventQueue& q, Generation gen, std::uint64_t first, std::uint64_t last) {
  for (std::uint64_t c = first; c <= last; ++c)
    q.dispatch_due(Cycle(c), EventPhase::MediaCommit, gen);
}
struct Assembly {
  EventQueue queue;
  DieDomain domain{total_blocks(make_geometry())};
  std::vector<MediaCompletion> done0, done1;
  std::vector<std::unique_ptr<NandChannelSlice>> owned;
  NandMediaRouter router{{nullptr, nullptr}};  // rebound below once slices exist
  Assembly() {
    owned.push_back(std::make_unique<NandChannelSlice>(
        make_slice_config(0), domain, queue, HandlerId(80),
        [this](MediaCompletion c) { done0.push_back(std::move(c)); }));
    owned.push_back(std::make_unique<NandChannelSlice>(
        make_slice_config(1), domain, queue, HandlerId(81),
        [this](MediaCompletion c) { done1.push_back(std::move(c)); }));
    router = NandMediaRouter({owned[0].get(), owned[1].get()});
  }
  NandChannelSlice& slice(std::uint64_t channel) { return *owned[channel]; }
};
const std::vector<std::uint8_t> kBytes(4096, 0x5a);
FlashCommand program(std::uint64_t token, Generation gen, std::uint64_t channel,
                     std::uint64_t bank, std::uint64_t block) {
  return {Token(token), gen, CommandKind::Program, ChannelId(channel),
          {0, 0, bank, block, 0}, PayloadHandle::from_bytes(kBytes)};
}
}  // namespace

// CHAN-T06: ownership hard invariant. Wrong-channel and foreign-bank commands
// are typed rejects with zero media side effects.
bool test_ownership_reject() {
  Assembly a;
  const auto snap0 = a.slice(0).snapshot();
  const auto snap1 = a.slice(1).snapshot();
  // Router rejects a channel with no slice.
  if (a.router.try_issue_media(program(1, Generation(0), 2, 0, 0), Cycle(0)).code !=
      AdmissionCode::Rejected)
    return false;
  // Slice-level channel mismatch (bypassing the router) is rejected.
  if (a.slice(0).try_issue(program(2, Generation(0), 1, 1, 0), Cycle(0)).code !=
      AdmissionCode::Rejected)
    return false;
  // Correct channel but a bank owned by the other channel is rejected.
  if (a.router.try_issue_media(program(3, Generation(0), 0, 1, 0), Cycle(0)).code !=
      AdmissionCode::Rejected)
    return false;
  if (a.router.try_issue_media(program(4, Generation(0), 1, 2, 0), Cycle(0)).code !=
      AdmissionCode::Rejected)
    return false;
  const auto after0 = a.slice(0).snapshot();
  const auto after1 = a.slice(1).snapshot();
  return snap0.inflight == after0.inflight && snap0.queued_events == after0.queued_events &&
         snap0.committed_pages == after0.committed_pages &&
         snap1.inflight == after1.inflight && snap1.queued_events == after1.queued_events &&
         snap1.committed_pages == after1.committed_pages;
}

// CHAN-I2 (timing half of T04): both channels saturated on the same core die
// complete on identical timelines — no shared die bus couples them.
bool test_path_isolation() {
  Assembly a;
  if (a.router.try_issue_media(program(1, Generation(0), 0, 0, 0), Cycle(0)).code !=
      AdmissionCode::Accepted)
    return false;
  if (a.router.try_issue_media(program(2, Generation(0), 1, 1, 0), Cycle(0)).code !=
      AdmissionCode::Accepted)
    return false;
  dispatch(a.queue, Generation(0), 0, 30);
  // Program = data_in 2 + array 20 + verify 3 = 25 cycles on both channels.
  return a.done0.size() == 1 && a.done1.size() == 1 &&
         a.done0[0].status == MediaStatus::Success &&
         a.done1[0].status == MediaStatus::Success &&
         a.done0[0].completed_at == Cycle(25) && a.done1[0].completed_at == Cycle(25);
}

// CHAN-T07: resetting one slice aborts only its own in-flight commands and
// discards only its own queued events; the other slice completes undisturbed.
bool test_reset_isolation() {
  Assembly a;
  if (a.router.try_issue_media(program(1, Generation(0), 0, 0, 0), Cycle(0)).code !=
      AdmissionCode::Accepted)
    return false;
  if (a.router.try_issue_media(program(2, Generation(0), 1, 1, 0), Cycle(0)).code !=
      AdmissionCode::Accepted)
    return false;
  dispatch(a.queue, Generation(0), 0, 4);
  a.slice(0).reset(Generation(1), Cycle(5));
  dispatch(a.queue, Generation(0), 5, 30);
  if (a.done0.size() != 1 || a.done0[0].status != MediaStatus::Aborted) return false;
  if (a.done1.size() != 1 || a.done1[0].status != MediaStatus::Success ||
      a.done1[0].completed_at != Cycle(25))
    return false;
  if (a.slice(0).snapshot().inflight != 0 || a.slice(1).snapshot().inflight != 0)
    return false;
  // Slice 0 is reusable under the new generation; page state survived reset.
  if (!a.slice(1).page_is_valid({0, 0, 1, 0, 0})) return false;
  if (a.router.try_issue_media(program(3, Generation(1), 0, 2, 0), Cycle(31)).code !=
      AdmissionCode::Accepted)
    return false;
  dispatch(a.queue, Generation(1), 31, 60);
  return a.done0.size() == 2 && a.done0[1].status == MediaStatus::Success &&
         a.slice(0).page_is_valid({0, 0, 2, 0, 0});
}

// CHAN-T09: the bad-block table and CapacityUnusable are stack-wide judgments
// living in the shared DieDomain, visible identically from every channel.
bool test_capacity_global_view() {
  Assembly a;
  // Block (bank 1, block 1) belongs to channel 1; marking it bad rejects
  // channel-1 commands while channel 0 is unaffected.
  const auto block = *a.slice(1).topology().flatten_block({0, 0, 1, 1, 0});
  a.domain.bad_block_table().mark(block, ras::BadBlockReason::Factory);
  FlashCommand read{Token(1), Generation(0), CommandKind::Read, ChannelId(1),
                    {0, 0, 1, 1, 0}, {}};
  if (a.router.try_issue_media(read, Cycle(0)).code != AdmissionCode::Rejected) return false;
  if (a.router.try_issue_media(program(2, Generation(0), 0, 0, 0), Cycle(0)).code !=
      AdmissionCode::Accepted)
    return false;
  dispatch(a.queue, Generation(0), 0, 30);
  if (a.done0.size() != 1 || a.done0[0].status != MediaStatus::Success) return false;
  // Exhaust every block in the stack: both channels see CapacityUnusable.
  const config::HbfGeometry g = make_geometry();
  const MediaTopology topology(g);
  for (std::uint64_t bank = 0; bank < g.banks_per_die; ++bank)
    for (std::uint64_t b = 0; b < g.blocks_per_bank; ++b)
      a.domain.bad_block_table().mark(*topology.flatten_block({0, 0, bank, b, 0}),
                                      ras::BadBlockReason::Retired);
  if (!a.domain.capacity_unusable()) return false;
  FlashCommand read0{Token(3), Generation(0), CommandKind::Read, ChannelId(0),
                     {0, 0, 0, 0, 0}, {}};
  FlashCommand read1{Token(4), Generation(0), CommandKind::Read, ChannelId(1),
                     {0, 0, 1, 0, 0}, {}};
  return a.router.try_issue_media(read0, Cycle(31)).code == AdmissionCode::Rejected &&
         a.router.try_issue_media(read1, Cycle(31)).code == AdmissionCode::Rejected;
}

// CHAN-T11: identical assemblies with identical streams produce byte-identical
// completion timelines; interleaved cross-slice events change nothing.
bool test_determinism() {
  std::vector<MediaCompletion> runs[2];
  for (int run = 0; run < 2; ++run) {
    Assembly a;
    for (std::uint64_t i = 0; i < 4; ++i) {
      const std::uint64_t channel = i % 2;
      if (a.router.try_issue_media(
              program(10 + i, Generation(0), channel, channel == 0 ? 0 : 1, i / 2),
              Cycle(i * 3)).code != AdmissionCode::Accepted)
        return false;
    }
    dispatch(a.queue, Generation(0), 0, 120);
    for (const auto& c : a.done0) runs[run].push_back(c);
    for (const auto& c : a.done1) runs[run].push_back(c);
  }
  if (runs[0].size() != runs[1].size()) return false;
  for (std::size_t i = 0; i < runs[0].size(); ++i) {
    const auto& x = runs[0][i];
    const auto& y = runs[1][i];
    if (x.token != y.token || x.status != y.status || x.completed_at != y.completed_at ||
        x.data_valid != y.data_valid)
      return false;
  }
  return true;
}

// CHAN-T12: per slice, accepted = terminal + outstanding at every point.
bool test_conservation() {
  Assembly a;
  std::uint64_t accepted0 = 0, accepted1 = 0;
  // Distinct valid addresses per channel: (bank, block) in {(ch_bank, 0),
  // (ch_bank, 1), (other_owned_bank, 0)} with blocks_per_bank = 2.
  const std::uint64_t bank0[3] = {0, 0, 2}, block0[3] = {0, 1, 0};
  const std::uint64_t bank1[3] = {1, 1, 3}, block1[3] = {0, 1, 0};
  for (std::uint64_t i = 0; i < 3; ++i) {
    if (a.router.try_issue_media(program(20 + i, Generation(0), 0, bank0[i], block0[i]),
                                 Cycle(i)).code == AdmissionCode::Accepted)
      ++accepted0;
    if (a.router.try_issue_media(program(30 + i, Generation(0), 1, bank1[i], block1[i]),
                                 Cycle(i)).code == AdmissionCode::Accepted)
      ++accepted1;
  }
  if (accepted0 + accepted1 != 6) return false;
  dispatch(a.queue, Generation(0), 0, 10);
  if (a.done0.size() + a.slice(0).snapshot().inflight != accepted0) return false;
  if (a.done1.size() + a.slice(1).snapshot().inflight != accepted1) return false;
  dispatch(a.queue, Generation(0), 11, 120);
  return a.done0.size() == accepted0 && a.done1.size() == accepted1 &&
         a.slice(0).snapshot().inflight == 0 && a.slice(1).snapshot().inflight == 0;
}

int main() {
  if (!test_ownership_reject()) return EXIT_FAILURE;
  if (!test_path_isolation()) return EXIT_FAILURE;
  if (!test_reset_isolation()) return EXIT_FAILURE;
  if (!test_capacity_global_view()) return EXIT_FAILURE;
  if (!test_determinism()) return EXIT_FAILURE;
  if (!test_conservation()) return EXIT_FAILURE;
  std::cout << "EXP-CHAN-PARTITION-001 PASS: ownership reject, path isolation, "
               "reset isolation, capacity global view, determinism, conservation\n";
  return EXIT_SUCCESS;
}
