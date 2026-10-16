#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "openhbx/common/admission.h"
#include "openhbx/config/resolved_hbf_config.h"
#include "openhbx/hbf/address/block_sequence.h"
#include "openhbx/hbf/host/host_types.h"
#include "openhbx/media/media_snapshot.h"
#include "openhbx/media/types.h"
#include "openhbx/system/system_lifecycle.h"
#include "openhbx/system/system_stats.h"
#include "openhbx/system/observability.h"

namespace openhbx {

namespace config { class ProductAssembly; }
namespace config { class ComponentRegistry; }

namespace hbf::address {
class HbfGeometry;
class ChannelTopology;
class HbfAddressMapper;
class BlockSequence;
}
namespace hbf::controller { class BaseDieFlashController; }
namespace hbf::host { class BaseDieControllerPort; class HbfHostProtocol; }
namespace integration { class NandMediaPort; }
namespace interconnect { class TsvRepairManager; class InterconnectFabric; }
namespace media::nand { class DieDomain; class NandChannelSlice; class NandMediaRouter; }
namespace pal { class FlashPal; }

struct SystemCompletion {
  Token token;
  Generation generation;
  std::uint8_t command_status{0xF};
  bool data_valid{false};
  PayloadHandle payload;
  Cycle completed_at;
  hbf::controller::ControllerErrorInfo error_info{
      hbf::controller::ControllerErrorInfo::None};
};

using SystemCompletionSink = std::function<void(SystemCompletion)>;

struct SystemRequest {
  Token token;
  hbf::host::HostIngress ingress;
  SystemCompletionSink completion;
};

struct OpenHbxBuildResult {
  std::unique_ptr<class OpenHbxSystem> system;
  std::string error;
  explicit operator bool() const noexcept { return system != nullptr; }
};

struct AddressDebugSnapshot {
  hbf::address::HbfAddress mapped_address;
  media::PhysicalAddress media_address;
  hbf::address::BlockSequenceSnapshot sequence;
  bool page_valid{false};
  std::uint64_t block_erase_count{0};
};

// Result of the single flat byte address decode (CHAN-001). Ramulator's ABI
// hands the system flat addresses; this decode is the only place where
// channel/axi/local are derived. Downstream modules receive channel
// explicitly and never re-derive it from a raw address.
struct FlatAddressDecode {
  hbf::address::ChannelId channel;
  std::uint8_t axi_interface{0};
  std::uint64_t axi_local_bytes{0};
  std::uint64_t channel_local_bytes{0};
};

class OpenHbxSystem {
 public:
  static OpenHbxBuildResult compose(config::ResolvedHbfConfig config);
  static OpenHbxBuildResult compose_with_registry(
      config::ResolvedHbfConfig config, const config::ComponentRegistry& registry);
  ~OpenHbxSystem();

  AdmissionResult try_submit(SystemRequest request);
  void tick();
  bool reset();
  DrainResult drain(std::uint64_t cycle_budget);
  SystemSnapshot snapshot() const;
  std::optional<AddressDebugSnapshot> debug_address_snapshot(
      std::uint64_t flat_address_bytes) const;
  // The single authoritative flat->(channel, axi, local) decode (CHAN-001).
  std::optional<FlatAddressDecode> decode_flat_address(
      std::uint64_t flat_address_bytes) const;
  // Per-channel media statistics (CHAN-012): slices are indexed by channel
  // value; each slice counts independently and the sum is conserved. Exception:
  // MediaSnapshot::bad_blocks is the shared DieDomain's global BBT size and is
  // identical for every channel — do not sum it (see NandChannelSlice).
  std::optional<media::MediaSnapshot> media_snapshot(ChannelId channel) const;
  OpenHbxSystemStats stats() const;
  void enable_periodic_bandwidth_log(std::uint64_t interval_cycles);
  EventJournalSnapshot observed_events() const { return journal_.snapshot(); }
  void set_log_level(LogLevel level) noexcept { journal_.set_minimum_level(level); }
  void set_log_sink(EventJournal::Sink sink) { journal_.set_sink(std::move(sink)); }
  Cycle cycle() const noexcept;
  Generation generation() const noexcept;
  Token allocate_request_token() noexcept;
  std::uint8_t axi_interfaces() const noexcept {
    return static_cast<std::uint8_t>(config_.system_model().axi_interfaces);
  }
  const config::ResolvedHbfConfig& resolved_config() const noexcept { return config_; }
  EventPhase current_phase() const noexcept { return current_phase_; }

 private:
  explicit OpenHbxSystem(config::ResolvedHbfConfig config);
  static config::ConfigResult<std::unique_ptr<OpenHbxSystem>> build_unpublished(
      const config::ResolvedHbfConfig& config,
      std::unique_ptr<config::ProductAssembly> assembly);
  bool build_object_graph_from_factories(std::string& error);
  bool build_registered_component(const std::string& kind, std::string& error);
  void on_host_response(hbf::host::HostResponse response);
  void deliver_host_response(Token token, hbf::host::HostResponse response);
  void on_system_event(EventPayload payload);
  bool schedule_system_event(Token token, EventPhase phase, Generation event_generation);
  void execute_reset();
  void initialize_links();
  std::vector<IdleReason> module_idle() const;

  config::ResolvedHbfConfig config_;
  std::unique_ptr<config::ProductAssembly> component_assembly_;
  EventJournal journal_;
  EventQueue events_;
  CompletionRegistry completions_;
  SystemLifecycle lifecycle_;
  std::unique_ptr<hbf::address::HbfGeometry> geometry_;
  std::unique_ptr<hbf::address::ChannelTopology> topology_;
  std::unique_ptr<hbf::address::HbfAddressMapper> mapper_;
  std::unique_ptr<hbf::address::BlockSequence> sequences_;
  std::unique_ptr<interconnect::TsvRepairManager> repair_;
  std::unique_ptr<interconnect::InterconnectFabric> fabric_;
  std::unique_ptr<integration::NandMediaPort> media_port_;
  // Channel-partitioned media (CHAN-003/005): one shared DieDomain plus one
  // slice per Host Channel, indexed by channel value, behind the router.
  std::unique_ptr<media::nand::DieDomain> die_domain_;
  std::vector<std::unique_ptr<media::nand::NandChannelSlice>> media_slices_;
  std::unique_ptr<media::nand::NandMediaRouter> media_router_;
  std::unique_ptr<pal::FlashPal> pal_;
  std::unique_ptr<hbf::controller::BaseDieFlashController> controller_;
  std::unique_ptr<hbf::host::BaseDieControllerPort> controller_port_;
  std::unique_ptr<hbf::host::HbfHostProtocol> host_;
  std::map<std::uint64_t, Token> response_tokens_;
  std::map<std::uint64_t, hbf::host::HostResponse> response_values_;
  bool admitting_host_{false};
  std::optional<hbf::host::HostResponse> synchronous_response_;
  EventPhase current_phase_{EventPhase::FinalDelivery};
  bool tick_in_progress_{false};
  bool reset_pending_{false};
  std::optional<Generation> reset_target_generation_;
  std::uint64_t attempts_{0}, accepted_{0}, busy_{0}, rejected_{0};
  std::uint64_t callbacks_{0}, resets_{0}, ticks_{0};
  std::uint64_t callback_errors_{0};
  std::uint64_t read_accepted_bytes_{0}, write_accepted_bytes_{0};
  std::uint64_t read_completed_requests_{0}, read_completed_bytes_{0};
  std::uint64_t write_completed_requests_{0}, write_completed_bytes_{0};
  std::uint64_t failed_requests_{0}, latency_samples_{0}, latency_sum_cycles_{0};
  std::uint64_t latency_min_cycles_{0}, latency_max_cycles_{0};
  std::uint64_t first_completion_cycle_{0}, last_completion_cycle_{0};
  std::uint64_t bandwidth_log_interval_cycles_{0};
  std::uint64_t bandwidth_log_last_cycle_{0};
  std::uint64_t bandwidth_log_last_read_bytes_{0};
  std::uint64_t bandwidth_log_last_write_bytes_{0};
  std::uint64_t next_external_token_{1};
};

}  // namespace openhbx
