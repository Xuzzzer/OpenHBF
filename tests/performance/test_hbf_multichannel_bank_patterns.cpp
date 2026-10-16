// EXP-PERF-HBF-MULTICH-BANK-PATTERNS
//
// All 16 Host Channels driven concurrently over the real production path
// (OpenHbxSystem + RequestBridge). Channels never share a Bank: synthetic
// ownership gives channel c the physical banks with `bank % 16 == c`, so each
// channel's traffic lands on its own disjoint BankArray set (the model has no
// Plane level - HLD_07 3.1 implements Core Die -> Die -> Bank -> Block -> Page
// only, so Bank is the sub-Die parallelism unit).
//
// Phase A sweeps reads-per-bank with every channel using all of its own banks,
// showing how aggregate bandwidth approaches the raw link ceiling as sense
// latency (tR) pipelines behind data transfer. N=1 reproduces the operating
// point of EXP-PERF-HBF-MAX-READ as a cross-check.
//
// Phase B runs 16 *different* per-channel patterns simultaneously (bank-count
// sweep plus traversal-order variants) and reports per-channel bandwidth, which
// isolates bank parallelism from the shared per-channel link.
//
// Reads only: the fixture overrides tPROG to a non-physical 20 cycles for setup
// speed, so mixing writes into a bandwidth number would not be defensible.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "openhbx/config/hbf_config_schema.h"
#include "openhbx/config/resolved_hbf_config.h"
#include "openhbx/integration/request_bridge.h"
#include "openhbx/system/open_hbx_system.h"

using namespace openhbx;

namespace {
constexpr std::uint64_t kFixtureProgramArrayCycles = 20;

void print_phase(const char* name, const std::string& detail) {
  std::cout << "[OpenHBX] " << std::left << std::setw(9) << name << detail << '\n'
            << std::flush;
}

config::ResolvedHbfConfig resolved_config() {
  std::ifstream input(OPENHBX_PRODUCT_CONFIG_PATH);
  assert(input);
  std::ostringstream yaml;
  yaml << input.rdbuf();
  auto parsed = config::parse_hbf_yaml(yaml.str());
  assert(parsed);
  auto raw = parsed.take_value();
  // Page programming is fixture setup and sits outside every read window.
  raw.scalars["model.media_program_array_cycles"] =
      std::to_string(kFixtureProgramArrayCycles);
  auto resolved = config::resolve_hbf_config(raw);
  assert(resolved);
  return resolved.take_value();
}

integration::BridgeRequest request(
    std::uint64_t address, integration::BridgeRequestType type,
    std::uint64_t page_bytes,
    std::function<void(const SystemCompletion&)> completion) {
  integration::BridgeRequest value;
  value.address = static_cast<std::int64_t>(address);
  value.size_bytes = static_cast<std::int32_t>(
      type == integration::BridgeRequestType::Write ? 64 : page_bytes);
  value.source_id = static_cast<std::int32_t>(address / 64 + 1);
  value.ingress_id = 0;
  value.type = type;
  if (type == integration::BridgeRequestType::Write)
    value.payload = PayloadHandle::from_bytes(std::vector<std::uint8_t>(64, 0xa5));
  value.completion = std::move(completion);
  return value;
}

void submit_retry(OpenHbxSystem& system, integration::RequestBridge& bridge,
                  integration::BridgeRequest request) {
  for (;;) {
    const auto result = bridge.try_submit(request);
    if (result.code == AdmissionCode::Accepted) return;
    assert(result.code == AdmissionCode::Busy);
    system.tick();
  }
}

void tick_until(OpenHbxSystem& system, const std::function<bool()>& done,
                std::uint64_t budget = 100000000) {
  for (std::uint64_t cycle = 0; cycle < budget && !done(); ++cycle) system.tick();
  // Checked in both Debug and Release: a silently exhausted budget would turn
  // an incomplete drain into a fabricated bandwidth number.
  if (!done()) {
    std::cerr << "drain budget exhausted after " << budget << " cycles\n";
    std::abort();
  }
}

// Geometry-derived addressing. In channel-local page units (l1):
//   bank          = l1 % owned_banks
//   page_in_block = (l1 / owned_banks) % pages_per_block
//   block         = l1 / (owned_banks * pages_per_block)
// so `l1 = bank + owned_banks * slot` walks the `slot`-th page of one bank.
// Because the AXI interface is derived from the address
// (axi = channel_local / (channel_capacity / axi_interfaces)), consecutive
// slots also spread across the channel's AXI interfaces and their per-interface
// host queues.
struct Addressing {
  std::uint64_t channel_capacity{0};
  std::uint64_t owned_banks{0};
  std::uint64_t pages_per_bank{0};
  std::uint64_t page_bytes{0};

  std::uint64_t address(std::uint64_t channel, std::uint64_t bank,
                        std::uint64_t slot) const {
    return channel * channel_capacity + (bank + owned_banks * slot) * page_bytes;
  }
};

Addressing addressing_of(const config::ResolvedHbfConfig& config) {
  Addressing value;
  value.channel_capacity =
      config.geometry().capacity_bytes / config.geometry().host_channels;
  value.owned_banks = config.ownership().banks_by_channel.front().size();
  value.pages_per_bank =
      config.geometry().blocks_per_bank * config.geometry().pages_per_block;
  value.page_bytes = config.geometry().page_bytes;
  return value;
}

void reset_between_phases(OpenHbxSystem& system) {
  // Must not sit inside assert(): with -DNDEBUG the call itself would vanish,
  // leaving the generation unchanged and the wait below spinning to its budget.
  const bool accepted = system.reset();
  if (!accepted) {
    std::cerr << "system reset was refused\n";
    std::abort();
  }
  const auto generation = system.generation().value() + 1;
  tick_until(system, [&] { return system.generation().value() == generation; });
}

double gigabytes_per_second(std::uint64_t bytes, std::uint64_t cycles,
                            std::uint64_t tck_picoseconds) {
  if (cycles == 0) return 0.0;
  return static_cast<double>(bytes) / cycles * 1000.0 / tck_picoseconds;
}

// One read request targeting (channel, bank, slot).
struct Target {
  std::uint64_t channel{0};
  std::uint64_t bank{0};
  std::uint64_t slot{0};
};

// Runs `targets` for every channel, injected round-robin across channels so all
// 16 channels stay loaded instead of draining one at a time. Returns the window
// in cycles; fills per-channel byte and window statistics.
struct RunResult {
  std::uint64_t cycles{0};
  std::uint64_t bytes{0};
  std::uint64_t reads{0};
  std::uint64_t errors{0};
  std::vector<std::uint64_t> bytes_by_channel;
  std::vector<std::uint64_t> first_submit_by_channel;
  std::vector<std::uint64_t> last_completion_by_channel;
};

RunResult run_reads(OpenHbxSystem& system, integration::RequestBridge& bridge,
                    const Addressing& addressing, std::uint64_t channels,
                    const std::vector<std::vector<Target>>& per_channel) {
  RunResult result;
  result.bytes_by_channel.assign(channels, 0);
  result.first_submit_by_channel.assign(channels, 0);
  result.last_completion_by_channel.assign(channels, 0);
  std::vector<bool> submitted_any(channels, false);

  std::uint64_t expected = 0;
  for (const auto& targets : per_channel) expected += targets.size();

  std::uint64_t completed = 0;
  const std::uint64_t start = system.cycle().value();

  // Non-blocking round-robin injection. A channel whose queues are full is
  // skipped for this round instead of stalling the injector: with a blocking
  // injector one back-pressured channel starves every other channel, which
  // collapses all per-channel windows onto one global window and destroys the
  // per-pattern comparison this phase exists to make.
  std::vector<std::size_t> cursor(channels, 0);
  // A back-pressured channel keeps its prepared request across retry rounds so
  // a long stall does not rebuild the request (and its completion closure)
  // once per simulated cycle.
  std::vector<std::optional<integration::BridgeRequest>> staged(channels);
  for (;;) {
    bool outstanding_targets = false;
    bool progressed = false;
    for (std::uint64_t channel = 0; channel < channels; ++channel) {
      const auto& targets = per_channel[channel];
      if (cursor[channel] >= targets.size()) continue;
      outstanding_targets = true;
      if (!staged[channel]) {
        const auto& target = targets[cursor[channel]];
        staged[channel] = request(
            addressing.address(target.channel, target.bank, target.slot),
            integration::BridgeRequestType::Read, addressing.page_bytes,
            [&, channel](const SystemCompletion& completion) {
              if (completion.command_status != 0 || !completion.data_valid ||
                  completion.payload.size() != addressing.page_bytes)
                ++result.errors;
              result.bytes_by_channel[channel] += completion.payload.size();
              result.last_completion_by_channel[channel] =
                  std::max(result.last_completion_by_channel[channel],
                           completion.completed_at.value());
              ++completed;
            });
      }
      const auto admission = bridge.try_submit(*staged[channel]);
      if (admission.code == AdmissionCode::Accepted) {
        if (!submitted_any[channel]) {
          result.first_submit_by_channel[channel] = system.cycle().value();
          submitted_any[channel] = true;
        }
        staged[channel].reset();
        ++cursor[channel];
        progressed = true;
      } else {
        assert(admission.code == AdmissionCode::Busy);
      }
    }
    if (!outstanding_targets) break;
    if (!progressed) system.tick();
  }
  tick_until(system, [&] { return completed == expected; });
  result.cycles = system.cycle().value() - start;
  result.reads = completed;
  for (const auto value : result.bytes_by_channel) result.bytes += value;
  return result;
}

std::vector<std::uint64_t> shuffled_banks(std::uint64_t count, std::uint64_t seed) {
  std::vector<std::uint64_t> banks(count);
  std::iota(banks.begin(), banks.end(), 0);
  std::mt19937_64 engine(seed);
  std::shuffle(banks.begin(), banks.end(), engine);
  return banks;
}
}  // namespace

int main() {
  const auto config = resolved_config();
  const auto& model = config.system_model();
  std::cout << "\n"
            << "OpenHBX HBF Multi-Channel Bank-Pattern Bandwidth\n"
            << "===============================================\n";
  print_phase("CONFIG", "loaded configs/products/ocp_hbf_0_7.yaml");
  auto built = OpenHbxSystem::compose(config);
  if (!built) {
    std::cerr << "system compose failed: " << built.error << "\n";
    return EXIT_FAILURE;
  }
  auto& system = *built.system;
  integration::RequestBridge bridge(system);

  const std::uint64_t channels = config.geometry().host_channels;
  const auto addressing = addressing_of(config);
  const std::uint64_t page_bytes = addressing.page_bytes;
  const std::uint64_t owned_banks = addressing.owned_banks;
  const std::uint64_t pages_per_bank = addressing.pages_per_bank;
  const std::uint64_t sectors_per_page = page_bytes / 64;
  assert(config.ownership().banks_by_channel.size() == channels);

  print_phase("TOPOLOGY", std::to_string(channels) + " channels x " +
      std::to_string(owned_banks) + " owned banks x " +
      std::to_string(pages_per_bank) + " pages/bank, " +
      std::to_string(page_bytes) + " B pages");

  // Setup: program every page that any phase reads (all pages of all owned
  // banks). Writes are 64 B non-posted, matching the OCP host model.
  const std::uint64_t setup_pages = channels * owned_banks * pages_per_bank;
  print_phase("SETUP", "programming " + std::to_string(setup_pages) +
      " pages (" + std::to_string(setup_pages * sectors_per_page) +
      " x 64 B writes)");
  std::uint64_t writes = 0;
  // Counted rather than asserted so the check survives -DNDEBUG (Release).
  std::uint64_t setup_errors = 0;
  for (std::uint64_t slot = 0; slot < pages_per_bank; ++slot) {
    for (std::uint64_t channel = 0; channel < channels; ++channel) {
      for (std::uint64_t bank = 0; bank < owned_banks; ++bank) {
        const auto base = addressing.address(channel, bank, slot);
        for (std::uint64_t sector = 0; sector < sectors_per_page; ++sector) {
          submit_retry(system, bridge,
              request(base + sector * 64, integration::BridgeRequestType::Write,
                  page_bytes,
                  [&](const SystemCompletion& completion) {
                    if (completion.command_status != 0 || completion.data_valid)
                      ++setup_errors;
                    ++writes;
                  }));
        }
      }
    }
  }
  tick_until(system, [&] { return writes == setup_pages * sectors_per_page; });
  if (setup_errors != 0) {
    std::cerr << "setup write errors: " << setup_errors << "\n";
    return EXIT_FAILURE;
  }

  const double per_channel_raw =
      static_cast<double>(model.fabric_active_lanes) *
      model.fabric_bits_per_lane_cycle * model.fabric_efficiency_ppm /
      1000000.0 / 8.0 * 1000.0 / model.tck_picoseconds;
  const double raw_ceiling = per_channel_raw * channels;
  const std::uint64_t read_service_cycles =
      model.media_read_command_cycles + model.media_read_sense_cycles;

  // ---------------------------------------------------------------- Phase A
  print_phase("PHASE A", "bank-parallel saturation sweep, all channels on all"
      " own banks");
  std::cout << "\nphase_a_saturation_sweep:\n"
            << "  every channel reads all " << owned_banks
            << " of its own banks; N = reads issued per bank\n"
            << "  (slots cycle through the bank's " << pages_per_bank
            << " pages, which also spreads across the channel's AXI interfaces)\n\n"
            << "  " << std::left << std::setw(5) << "N"
            << std::setw(12) << "reads"
            << std::setw(14) << "MiB"
            << std::setw(12) << "cycles"
            << std::setw(14) << "GB/s"
            << std::setw(12) << "TB/s"
            << "link_util\n" << std::flush;
  struct SweepRow { std::uint64_t n, reads, bytes, cycles; double gbps; };
  std::vector<SweepRow> sweep;
  for (const std::uint64_t reads_per_bank : {1ULL, 2ULL, 4ULL, 8ULL, 16ULL}) {
    reset_between_phases(system);
    std::vector<std::vector<Target>> per_channel(channels);
    for (std::uint64_t channel = 0; channel < channels; ++channel) {
      per_channel[channel].reserve(owned_banks * reads_per_bank);
      for (std::uint64_t repeat = 0; repeat < reads_per_bank; ++repeat)
        for (std::uint64_t bank = 0; bank < owned_banks; ++bank)
          per_channel[channel].push_back(
              {channel, bank, repeat % pages_per_bank});
    }
    const auto run = run_reads(system, bridge, addressing, channels, per_channel);
    if (run.errors != 0) {
      std::cerr << "phase A read errors: " << run.errors << "\n";
      return EXIT_FAILURE;
    }
    const double gbps =
        gigabytes_per_second(run.bytes, run.cycles, model.tck_picoseconds);
    sweep.push_back({reads_per_bank, run.reads, run.bytes, run.cycles, gbps});
    std::cout << std::fixed << std::setprecision(3)
              << "  " << std::left << std::setw(5) << reads_per_bank
              << std::setw(12) << run.reads
              << std::setw(14) << static_cast<double>(run.bytes) / (1024.0 * 1024.0)
              << std::setw(12) << run.cycles
              << std::setw(14) << gbps
              << std::setw(12) << gbps / 1000.0
              << std::setprecision(1) << gbps / raw_ceiling * 100.0 << " %\n"
              << std::flush;
  }

  // ---------------------------------------------------------------- Phase B
  print_phase("PHASE B", "16 different per-channel patterns running"
      " concurrently");
  struct Pattern { const char* name; std::vector<std::uint64_t> banks; bool page_major; std::uint64_t slots; };
  std::vector<Pattern> patterns;
  {
    std::vector<std::uint64_t> all(owned_banks);
    std::iota(all.begin(), all.end(), 0);
    std::vector<std::uint64_t> reversed(all.rbegin(), all.rend());
    std::vector<std::uint64_t> stride7;
    for (std::uint64_t i = 0; i < owned_banks; ++i)
      stride7.push_back((i * 7) % owned_banks);  // 7 is coprime with 256
    const auto permuted = shuffled_banks(owned_banks, 0x5eed1234ULL);
    std::vector<std::uint64_t> even, odd, first_half, second_half;
    for (std::uint64_t i = 0; i < owned_banks; ++i)
      (i % 2 == 0 ? even : odd).push_back(i);
    for (std::uint64_t i = 0; i < owned_banks / 2; ++i) first_half.push_back(i);
    for (std::uint64_t i = owned_banks / 2; i < owned_banks; ++i)
      second_half.push_back(i);
    auto head = [&](std::uint64_t count) {
      return std::vector<std::uint64_t>(all.begin(), all.begin() + count);
    };
    patterns = {
        {"seq-all-banks", all, false, pages_per_bank},
        {"reverse-all-banks", reversed, false, pages_per_bank},
        {"stride7-permutation", stride7, false, pages_per_bank},
        {"seeded-random-perm", permuted, false, pages_per_bank},
        {"even-banks-half", even, false, pages_per_bank},
        {"odd-banks-half", odd, false, pages_per_bank},
        {"low-half-contiguous", first_half, false, pages_per_bank},
        {"high-half-contiguous", second_half, false, pages_per_bank},
        {"64-banks", head(64), false, pages_per_bank},
        {"32-banks", head(32), false, pages_per_bank},
        {"16-banks", head(16), false, pages_per_bank},
        {"8-banks", head(8), false, pages_per_bank},
        {"4-banks", head(4), false, pages_per_bank},
        {"2-banks", head(2), false, pages_per_bank},
        {"single-bank-serial", head(1), false, pages_per_bank},
        {"page-major-all-banks", all, true, pages_per_bank},
    };
    assert(patterns.size() >= channels);
  }

  reset_between_phases(system);
  std::vector<std::vector<Target>> per_channel(channels);
  for (std::uint64_t channel = 0; channel < channels; ++channel) {
    const auto& pattern = patterns[channel];
    auto& targets = per_channel[channel];
    targets.reserve(pattern.banks.size() * pattern.slots);
    if (pattern.page_major) {
      for (std::uint64_t slot = 0; slot < pattern.slots; ++slot)
        for (const auto bank : pattern.banks) targets.push_back({channel, bank, slot});
    } else {
      for (const auto bank : pattern.banks)
        for (std::uint64_t slot = 0; slot < pattern.slots; ++slot)
          targets.push_back({channel, bank, slot});
    }
  }
  const auto phase_b = run_reads(system, bridge, addressing, channels, per_channel);
  if (phase_b.errors != 0) {
    std::cerr << "phase B read errors: " << phase_b.errors << "\n";
    return EXIT_FAILURE;
  }

  std::cout << "\nphase_b_per_channel_patterns:\n"
            << "  " << std::left << std::setw(4) << "ch"
            << std::setw(23) << "pattern"
            << std::setw(8) << "banks"
            << std::setw(8) << "reads"
            << std::setw(10) << "MiB"
            << std::setw(12) << "window"
            << std::setw(12) << "GB/s"
            << std::setw(14) << "bank_ceiling"
            << "achieved\n";
  for (std::uint64_t channel = 0; channel < channels; ++channel) {
    const auto& pattern = patterns[channel];
    const std::uint64_t window =
        phase_b.last_completion_by_channel[channel] -
        phase_b.first_submit_by_channel[channel];
    const double gbps = gigabytes_per_second(
        phase_b.bytes_by_channel[channel], window, model.tck_picoseconds);
    // Ceiling this pattern could reach if its banks ran fully in parallel:
    // min(bank parallelism, channel link). Achieved/ceiling well below 100%
    // means the issue path, not the banks, was the limiter.
    const double bank_parallel = static_cast<double>(pattern.banks.size()) *
        page_bytes / read_service_cycles * 1000.0 / model.tck_picoseconds;
    const double ceiling = std::min(bank_parallel, per_channel_raw);
    std::cout << std::fixed << std::setprecision(3)
              << "  " << std::left << std::setw(4) << channel
              << std::setw(23) << pattern.name
              << std::setw(8) << pattern.banks.size()
              << std::setw(8) << per_channel[channel].size()
              << std::setw(10)
              << static_cast<double>(phase_b.bytes_by_channel[channel]) / (1024.0 * 1024.0)
              << std::setw(12) << window
              << std::setw(12) << gbps
              << std::setw(14) << ceiling
              << std::setprecision(1) << gbps / ceiling * 100.0 << " %\n";
  }
  const double phase_b_gbps = gigabytes_per_second(
      phase_b.bytes, phase_b.cycles, model.tck_picoseconds);
  // Channels carry deliberately unequal loads here, so the short-pattern
  // channels idle while the 256-bank channels finish. Dividing total bytes by
  // the global window therefore understates what the machine sustained; the
  // sum of each channel's own rate is the concurrent-throughput figure.
  double phase_b_sum_of_channels = 0.0;
  for (std::uint64_t channel = 0; channel < channels; ++channel) {
    const std::uint64_t window = phase_b.last_completion_by_channel[channel] -
                                 phase_b.first_submit_by_channel[channel];
    phase_b_sum_of_channels += gigabytes_per_second(
        phase_b.bytes_by_channel[channel], window, model.tck_picoseconds);
  }

  // Per-channel media counters come from the channel-partitioned slices
  // (CHAN-012). bad_blocks is a shared global view and is deliberately not
  // summed here.
  std::uint64_t slice_committed = 0, slice_inflight = 0;
  for (std::uint64_t channel = 0; channel < channels; ++channel) {
    const auto snapshot = system.media_snapshot(hbf::address::ChannelId(channel));
    assert(snapshot);
    slice_committed += snapshot->committed_pages;
    slice_inflight += snapshot->inflight;
  }

  const auto best = *std::max_element(sweep.begin(), sweep.end(),
      [](const SweepRow& a, const SweepRow& b) { return a.gbps < b.gbps; });

  std::cout << std::fixed << std::setprecision(3)
            << "\nsummary:\n"
            << "  id:                         EXP-PERF-HBF-MULTICH-BANK-PATTERNS\n"
            << "  evidence:                   E4-model-resource candidate\n"
            << "  config_hash:                " << config.canonical_hash() << "\n"
            << "  tck:                        " << model.tck_picoseconds << " ps\n"
            << "  read_service_cycles:        " << read_service_cycles
            << " (tR + command)\n"
            << "  per_channel_raw_ceiling:    " << per_channel_raw << " GB/s\n"
            << "  aggregate_raw_ceiling:      " << raw_ceiling << " GB/s ("
            << raw_ceiling / 1000.0 << " TB/s)\n"
            << "  phase_a_best:               " << best.gbps << " GB/s ("
            << best.gbps / 1000.0 << " TB/s) at N=" << best.n << ", "
            << best.reads << " reads, " << best.cycles << " cycles\n"
            << "  phase_a_best_link_util:     " << best.gbps / raw_ceiling * 100.0 << " %\n"
            << "  phase_b_aggregate:          " << phase_b_gbps << " GB/s ("
            << phase_b_gbps / 1000.0 << " TB/s) over " << phase_b.reads
            << " reads, " << phase_b.cycles << " cycles\n"
            << "  phase_b_sum_of_channels:    " << phase_b_sum_of_channels
            << " GB/s (" << phase_b_sum_of_channels / 1000.0
            << " TB/s), each channel over its own window\n"
            << "  slice_committed_pages_sum:  " << slice_committed << " (setup wrote "
            << setup_pages << " pages)\n"
            << "  slice_inflight_sum:         " << slice_inflight << " (expect 0)\n";
  const auto stats = system.stats();
  if (slice_inflight != 0 || stats.failed_requests != 0 || stats.outstanding != 0) {
    std::cerr << "conservation check failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "  failed_requests:            " << stats.failed_requests << "\n"
            << "  outstanding_after_drain:    " << stats.outstanding << "\n"
            << "\nresult: PASS\n"
            << "note: synthetic transaction-level model; tR/link values are"
            << " synthetic, not vendor guarantees\n";
  return EXIT_SUCCESS;
}
