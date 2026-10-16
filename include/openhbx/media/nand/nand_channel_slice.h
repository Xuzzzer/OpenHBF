#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "openhbx/common/admission.h"
#include "openhbx/config/resolved_hbf_config.h"
#include "openhbx/media/media_snapshot.h"
#include "openhbx/media/nand/die_domain.h"
#include "openhbx/media/nand/topology.h"
#include "openhbx/ras/reliability_model.h"
#include "openhbx/system/event_queue.h"

namespace openhbx::media::nand {

enum class PayloadMode { Sparse, TimingOnly };
struct StageTiming {
  // Fast component-test defaults. Product timings come from ResolvedSystemModel.
  std::uint64_t read_command{1}, read_sense{10};
  std::uint64_t program_data_in{2}, program_array{20}, program_verify{3};
  std::uint64_t erase_setup{1}, erase_array{40}, erase_verify{4};
};

// Configuration of one channel's media slice. `owned_banks` holds the flat
// bank ids owned by this channel; 02 expands them from
// hbf::address::ChannelTopology::banks_by_channel at assembly time, so 04
// remains the single ownership source and this module never includes 04
// headers (ADR-CHAN-004).
struct NandChannelSliceConfig {
  config::HbfGeometry geometry;
  ChannelId channel{};
  std::set<std::uint64_t> owned_banks;
  PayloadMode payload_mode{PayloadMode::Sparse};
  StageTiming timing;
  ras::ReliabilityProfile reliability;
  std::size_t max_inflight{64};  // CHAN-010: per-slice limit
};

// The media engine of exactly one Host Channel's bank slice (CHAN-003).
// Owns stage FSM contexts, Page/payload records, erase counts, the per-bank
// BankArray EAT and the slice-private ChannelMediaPath(channel, core_die) EAT
// — the latter replaces the monolithic device's global die bus (CHAN-004).
// Die-level shared state (blocked dies, BBT, capacity view) is queried from
// the shared DieDomain (CHAN-005). Cross-channel access is rejected
// structurally: a command whose channel differs from this slice's, or whose
// bank is not in owned_banks, never reaches media state (CHAN-006).
class NandChannelSlice {
 public:
  NandChannelSlice(NandChannelSliceConfig config, DieDomain& die_domain,
                   EventQueue& events, HandlerId handler,
                   CompletionSink completion);
  AdmissionResult try_issue(FlashCommand command, Cycle now);
  // Aborts this slice's in-flight commands and discards only this slice's
  // queued events (scoped discard, CHAN-007); other slices are unaffected.
  void reset(Generation next_generation, Cycle now);
  // Per-channel counters (CHAN-012). `inflight`, `queued_events`,
  // `committed_pages` and `stale_events` are slice-private and therefore
  // summable across channels. `bad_blocks` is NOT: it is the shared
  // DieDomain's global BBT size (CHAN-009) and reads identically from every
  // slice, so summing it would multiply the stack-wide count by the channel
  // count. Read it from any single slice, or from DieDomain directly.
  MediaSnapshot snapshot() const;
  const MediaTopology& topology() const noexcept { return topology_; }
  ChannelId channel() const noexcept { return config_.channel; }
  bool page_is_valid(const PhysicalAddress& address) const;
  std::uint64_t block_erase_count(const PhysicalAddress& address) const;
  ras::ReliabilityModel& reliability() noexcept { return reliability_; }

 private:
  enum class Stage : std::uint8_t { ReadCommand, ReadSense, ProgramData, ProgramArray,
                                    ProgramVerify, EraseSetup, EraseArray, EraseVerify };
  struct Context { FlashCommand command; Stage stage; Cycle due; std::uint64_t sequence{0}; PayloadHandle read_snapshot; };
  struct PageRecord { PayloadHandle payload; std::uint64_t signature{0}; };
  void on_event(EventPayload payload);
  bool schedule(Context& context, Cycle now, Stage next);
  std::uint64_t duration(Stage stage) const;
  // Stages that occupy the ChannelMediaPath(channel, core_die) resource:
  // the command/data-in transfer legs toward the core die domain.
  bool uses_channel_path(Stage stage) const;
  void terminal(Context context, MediaStatus status, Cycle now, bool commit);
  MediaStatus validate(const FlashCommand& command) const;

  NandChannelSliceConfig config_;
  DieDomain& die_domain_;
  MediaTopology topology_;
  EventQueue& events_;
  HandlerId handler_;
  CompletionSink completion_;
  Generation generation_{0};
  std::map<std::uint64_t, Context> contexts_;
  std::map<std::uint64_t, PageRecord> pages_;
  std::map<std::uint64_t, std::uint64_t> erase_counts_;
  std::map<std::uint64_t, Cycle> bank_eat_;              // BankArray, owned banks only
  std::map<std::uint64_t, Cycle> channel_media_path_eat_;  // keyed by core_die
  ras::ReliabilityModel reliability_;
  std::uint64_t committed_pages_{0}, stale_events_{0};
};

}  // namespace openhbx::media::nand
