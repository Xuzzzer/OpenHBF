#include "openhbx/system/open_hbx_system.h"

#include <algorithm>
#include <array>
#include <limits>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "openhbx/common/checked_math.h"
#include "openhbx/config/component_registry.h"
#include "openhbx/config/hbf_product_composer.h"
#include "openhbx/hbf/address/block_sequence.h"
#include "openhbx/hbf/address/channel_topology.h"
#include "openhbx/hbf/address/hbf_address_mapper.h"
#include "openhbx/hbf/address/hbf_geometry.h"
#include "openhbx/hbf/controller/base_die_flash_controller.h"
#include "openhbx/hbf/host/controller_port.h"
#include "openhbx/hbf/host/host_protocol.h"
#include "openhbx/integration/nand_media_port.h"
#include "openhbx/interconnect/fabric.h"
#include "openhbx/interconnect/tsv_repair.h"
#include "openhbx/media/nand/die_domain.h"
#include "openhbx/media/nand/nand_channel_slice.h"
#include "openhbx/media/nand/nand_media_router.h"
#include "openhbx/pal/flash_pal.h"
#include "openhbx/system/handler_ids.h"

namespace openhbx {
namespace config {
ConfigResult<std::unique_ptr<OpenHbxSystem>> compose_open_hbx_system(
    const ResolvedHbfConfig& config, const ComponentRegistry& registry,
    SystemAssemblyBuilder builder) {
  if (!builder)
    return ConfigResult<std::unique_ptr<OpenHbxSystem>>::failure({{
        ConfigErrorCode::BuildFailed, "system", "system assembly builder is required", 0}});
  auto selection = compose_hbf_product(config, registry);
  if (!selection)
    return ConfigResult<std::unique_ptr<OpenHbxSystem>>::failure(selection.issues());
  auto system = builder(config, selection.take_value());
  if (!system) return system;
  if (!system.value())
    return ConfigResult<std::unique_ptr<OpenHbxSystem>>::failure({{
        ConfigErrorCode::BuildFailed, "system", "system builder returned null", 0}});
  return system;
}
}  // namespace config

namespace {
// HandlerId values come from the single allocation header (CHAN-008).


std::uint64_t add_counter(std::uint64_t current, std::uint64_t increment,
                          const char* message) {
  const auto updated = checked_add(current, increment);
  if (!updated) throw std::overflow_error(message);
  return *updated;
}

std::vector<std::string> component_dependencies(const std::string& kind) {
  if (kind == "address" || kind == "media") return {};
  if (kind == "interconnect") return {"address", "media"};
  if (kind == "controller") return {"address", "interconnect"};
  if (kind == "host") return {"address", "controller"};
  if (kind == "ras") return {"media"};
  return {"__unknown_kind__"};
}

std::vector<hbf::address::PhysicalBank> all_banks(
    const hbf::address::HbfGeometry& geometry) {
  std::vector<hbf::address::PhysicalBank> banks;
  for (std::uint64_t core = 0; core < geometry.core_dies(); ++core)
    for (std::uint64_t die = 0; die < geometry.dies_per_core(); ++die)
      for (std::uint64_t bank = 0; bank < geometry.banks_per_die(); ++bank)
        banks.push_back({hbf::address::CoreDieIndex(core),
                         hbf::address::DieIndex(die),
                         hbf::address::BankIndex(bank)});
  return banks;
}

interconnect::FabricProfile make_fabric_profile(
    std::uint8_t channels, std::uint8_t axi_interfaces,
    const config::ResolvedSystemModel& model) {
  interconnect::FabricProfile profile;
  profile.technology = interconnect::FabricTechnology::Tsv;
  profile.source = model.source;
  profile.queue_depth = static_cast<std::size_t>(model.pal_max_inflight);
  profile.channel_count = channels;
  profile.active_lanes = model.fabric_active_lanes;
  profile.spare_lanes = model.fabric_spare_lanes;
  profile.bits_per_lane_per_cycle = model.fabric_bits_per_lane_cycle;
  profile.efficiency_ppm = model.fabric_efficiency_ppm;
  profile.arbitration_cycles = model.fabric_arbitration_cycles;
  profile.propagation_cycles = model.fabric_propagation_cycles;
  for (std::uint64_t channel = 0; channel < channels; ++channel) {
    std::vector<std::uint64_t> channel_lanes;
    channel_lanes.reserve(static_cast<std::size_t>(profile.active_lanes));
    for (std::uint64_t lane = 0; lane < profile.active_lanes; ++lane)
      channel_lanes.push_back(channel * profile.active_lanes + lane);
    for (std::uint64_t axi = 0; axi < axi_interfaces; ++axi) {
      const std::uint64_t endpoint = channel * axi_interfaces + axi;
      // AXI interfaces are virtual endpoints. They retain independent queues
      // and ordering identities but share their physical Channel data path.
      profile.routes.push_back(
          {endpoint + 1, endpoint, {channel + 1}, channel_lanes});
    }
  }
  return profile;
}
}  // namespace

OpenHbxBuildResult OpenHbxSystem::compose(config::ResolvedHbfConfig config) {
  config::ComponentRegistry registry;
  const auto& selected = config.components();
  const std::array<std::pair<const char*, const std::string*>, 6> components{{
      {"media", &selected.media}, {"interconnect", &selected.interconnect},
      {"address", &selected.address}, {"controller", &selected.controller},
      {"host", &selected.host}, {"ras", &selected.ras}}};
  for (const auto& component : components) {
    const std::string kind = component.first;
    const std::string impl = *component.second;
    auto issues = registry.add(kind, impl,
        [kind, impl](const config::ResolvedHbfConfig&) {
          return config::ConfigResult<std::unique_ptr<config::ComponentInstance>>::success(
              std::make_unique<config::SystemComponentFactory>(
                  kind, impl, component_dependencies(kind),
                  [kind](OpenHbxSystem& system, std::string& error) {
                    return system.build_registered_component(kind, error);
                  }));
        });
    if (!issues.empty()) return {nullptr, issues.front().message};
  }
  registry.seal();
  return compose_with_registry(std::move(config), registry);
}

OpenHbxBuildResult OpenHbxSystem::compose_with_registry(
    config::ResolvedHbfConfig config, const config::ComponentRegistry& registry) {
  auto composed = config::compose_open_hbx_system(
      config, registry, [](const config::ResolvedHbfConfig& resolved,
                           std::unique_ptr<config::ProductAssembly> assembly) {
        return build_unpublished(resolved, std::move(assembly));
      });
  if (!composed) {
    return {nullptr, composed.issues().empty() ? "system composition failed"
                                               : composed.issues().front().message};
  }
  return {composed.take_value(), {}};
}

config::ConfigResult<std::unique_ptr<OpenHbxSystem>>
OpenHbxSystem::build_unpublished(
    const config::ResolvedHbfConfig& config,
    std::unique_ptr<config::ProductAssembly> assembly) {
  try {
    if (!assembly || assembly->size() != 6)
      return config::ConfigResult<std::unique_ptr<OpenHbxSystem>>::failure({{
          config::ConfigErrorCode::BuildFailed, "system",
          "complete component factory assembly is required", 0}});
    auto result = std::unique_ptr<OpenHbxSystem>(new OpenHbxSystem(config));
    result->component_assembly_ = std::move(assembly);
    std::string error;
    if (!result->build_object_graph_from_factories(error))
      return config::ConfigResult<std::unique_ptr<OpenHbxSystem>>::failure({{
          config::ConfigErrorCode::BuildFailed, "system", std::move(error), 0}});
    return config::ConfigResult<std::unique_ptr<OpenHbxSystem>>::success(std::move(result));
  } catch (const std::exception& error) {
    return config::ConfigResult<std::unique_ptr<OpenHbxSystem>>::failure({{
        config::ConfigErrorCode::BuildFailed, "system", error.what(), 0}});
  }
}

OpenHbxSystem::OpenHbxSystem(config::ResolvedHbfConfig config)
    : config_(std::move(config)),
      journal_(256, LogLevel::Warn),
      events_(&journal_),
      completions_(static_cast<std::size_t>(config_.system_model().completion_capacity)),
      lifecycle_(events_, completions_) {
  // Ramulator-style default: print Warn+ events to std::clog. Embedders may
  // replace the sink via set_log_sink or silence it with set_log_sink({}).
  journal_.set_sink(&console_log);
}

OpenHbxSystem::~OpenHbxSystem() = default;

bool OpenHbxSystem::build_object_graph_from_factories(std::string& error) {
  const auto& model = config_.system_model();
  if (!events_.register_handler(kSystemHandlerId, [this](EventPayload payload) {
        on_system_event(std::move(payload));
      })) {
    error = "duplicate System event handler";
    return false;
  }
  if (config_.profile() != "OCP_HBF_0_7") {
    error = "OpenHbxSystem supports only OCP_HBF_0_7";
    return false;
  }
  const auto& components = config_.components();
  if (components.host != "OcpHbfTransactionLevel" ||
      components.address != "HbfR1R5" ||
      components.controller != "BaseDieFlash" ||
      components.interconnect != "HbfTsvBaseline" ||
      components.media != "NandFlash" || components.ras != "NandRawRas") {
    error = "resolved component selection is not the TASK1 production object graph";
    return false;
  }
  if (model.completion_capacity == 0 || model.axi_interfaces == 0 ||
      model.axi_interfaces > 4 ||
      (model.axi_interfaces != 1 && model.axi_interfaces != 2 &&
       model.axi_interfaces != 4)) {
    error = "invalid OpenHBX model resources";
    return false;
  }
  if (model.source.empty() || model.media_max_inflight == 0 ||
      model.pal_max_inflight == 0 || model.media_read_command_cycles == 0 ||
      model.media_read_sense_cycles == 0 ||
      model.media_program_data_cycles == 0 ||
      model.media_program_array_cycles == 0 ||
      model.media_program_verify_cycles == 0 ||
      model.media_erase_setup_cycles == 0 ||
      model.media_erase_array_cycles == 0 ||
      model.media_erase_verify_cycles == 0) {
    error = "runtime model values require an explicit source and nonzero resources";
    return false;
  }
  if (config_.geometry().page_bytes != 4096) {
    error = "TASK1 freezes the HBF DLU and NAND page size at 4096 B";
    return false;
  }
  if (!component_assembly_) {
    error = "component factory assembly is missing";
    return false;
  }
  auto factories = component_assembly_->take_components();
  std::set<std::string> built;
  const auto& selected = config_.components();
  for (auto& instance : factories) {
    auto* factory = dynamic_cast<config::SystemComponentFactory*>(instance.get());
    if (factory == nullptr) {
      error = "component assembly contains an invalid typed factory";
      return false;
    }
    const std::string* expected_impl = nullptr;
    if (factory->kind() == "address") expected_impl = &selected.address;
    else if (factory->kind() == "media") expected_impl = &selected.media;
    else if (factory->kind() == "interconnect") expected_impl = &selected.interconnect;
    else if (factory->kind() == "controller") expected_impl = &selected.controller;
    else if (factory->kind() == "host") expected_impl = &selected.host;
    else if (factory->kind() == "ras") expected_impl = &selected.ras;
    if (expected_impl == nullptr || factory->impl() != *expected_impl) {
      error = "component factory does not match resolved implementation: " +
              factory->kind();
      return false;
    }
    if (!built.insert(factory->kind()).second) {
      error = "component assembly contains duplicate kind: " + factory->kind();
      return false;
    }
    for (const auto& dependency : factory->dependencies()) {
      if (built.count(dependency) == 0) {
        error = "component dependency is not satisfied: " + factory->kind() +
                " requires " + dependency;
        return false;
      }
    }
    if (!factory->build(*this, error)) return false;
  }
  if (built.size() != 6) {
    error = "component assembly did not build all production owners";
    return false;
  }
  if (!geometry_ || !topology_ || !mapper_ || !sequences_ || !repair_ ||
      !fabric_ || !media_port_ || !die_domain_ || media_slices_.empty() ||
      !media_router_ || !pal_ || !controller_ ||
      !controller_port_ || !host_) {
    error = "component factories did not publish the complete production object graph";
    return false;
  }
  for (const auto& slice : media_slices_)
    slice->reset(lifecycle_.generation(), lifecycle_.cycle());
  host_->reset(lifecycle_.generation(), lifecycle_.cycle());
  initialize_links();
  journal_.observe({LogLevel::Info, lifecycle_.cycle(), EventPhase::Reset, 0,
      false, lifecycle_.generation(), "open_hbx_system", "compose",
      "object_graph", Token{},
      "ok profile=" + config_.profile() +
          " host_channels=" + std::to_string(geometry_->channels()) +
          " capacity_bytes=" +
          std::to_string(config_.geometry().capacity_bytes) +
          " config_hash=" + config_.canonical_hash(), 0});
  return true;
}

bool OpenHbxSystem::build_registered_component(const std::string& kind,
                                                std::string& error) {
  const auto& model = config_.system_model();
  if (kind == "address") {
  auto geometry = hbf::address::HbfGeometry::from_config(config_, &error);
  if (!geometry) return false;
  geometry_ = std::make_unique<hbf::address::HbfGeometry>(std::move(*geometry));
  hbf::address::ChannelOwnershipProfile ownership;
  ownership.source = config_.ownership().synthetic
      ? hbf::address::OwnershipSource::SyntheticFixture
      : hbf::address::OwnershipSource::ProductProfile;
  ownership.banks_by_channel.resize(config_.ownership().banks_by_channel.size());
  for (std::size_t channel = 0; channel < config_.ownership().banks_by_channel.size(); ++channel)
    for (const auto& bank : config_.ownership().banks_by_channel[channel])
      ownership.banks_by_channel[channel].push_back({
          hbf::address::CoreDieIndex(bank.core_die), hbf::address::DieIndex(bank.die),
          hbf::address::BankIndex(bank.bank)});
  auto topology = hbf::address::ChannelTopology::create(
      *geometry_, std::move(ownership), &error);
  if (!topology) return false;
  topology_ = std::make_unique<hbf::address::ChannelTopology>(std::move(*topology));
  mapper_ = std::make_unique<hbf::address::HbfAddressMapper>(*geometry_, *topology_);
  sequences_ = std::make_unique<hbf::address::BlockSequence>(*geometry_);
    return true;
  }
  if (kind == "media") {
  media_port_ = std::make_unique<integration::NandMediaPort>();
  // CHAN-003/004/005: channel-partitioned media. One shared DieDomain owns
  // die-level state; each Host Channel gets its own NandChannelSlice whose
  // owned banks are expanded from the 04 ChannelTopology ownership profile;
  // the router is the 06-facing media port.
  const auto& geometry_config = config_.geometry();
  if (geometry_config.host_channels == 0 ||
      geometry_config.host_channels > kMaxHostChannels) {
    error = "host channel count exceeds the OCP HBF 16-channel maximum";
    return false;
  }
  const std::uint64_t total_blocks = geometry_config.core_dies *
      geometry_config.dies_per_core * geometry_config.banks_per_die *
      geometry_config.blocks_per_bank;
  die_domain_ = std::make_unique<media::nand::DieDomain>(total_blocks);
  const media::nand::MediaTopology media_topology(geometry_config);
  media_slices_.clear();
  std::vector<media::nand::NandChannelSlice*> slices_by_channel;
  for (std::uint64_t channel = 0; channel < geometry_config.host_channels;
       ++channel) {
    media::nand::NandChannelSliceConfig slice_config;
    slice_config.geometry = geometry_config;
    slice_config.channel = ChannelId(channel);
    if (channel < config_.ownership().banks_by_channel.size()) {
      for (const auto& bank : config_.ownership().banks_by_channel[channel]) {
        const media::PhysicalAddress physical{bank.core_die, bank.die,
                                              bank.bank, 0, 0};
        slice_config.owned_banks.insert(media_topology.bank_id(physical));
      }
    }
    slice_config.payload_mode = media::nand::PayloadMode::Sparse;
    slice_config.max_inflight = static_cast<std::size_t>(model.media_max_inflight);
    slice_config.timing = {
        model.media_read_command_cycles, model.media_read_sense_cycles,
        model.media_program_data_cycles, model.media_program_array_cycles,
        model.media_program_verify_cycles, model.media_erase_setup_cycles,
        model.media_erase_array_cycles, model.media_erase_verify_cycles};
    slice_config.reliability = {model.reliability_seed,
                                std::numeric_limits<std::uint64_t>::max()};
    auto slice = std::make_unique<media::nand::NandChannelSlice>(
        std::move(slice_config), *die_domain_, events_,
        media_slice_handler(ChannelId(channel)),
        [this](media::MediaCompletion completion) {
          if (pal_) pal_->on_media_completion(std::move(completion));
        });
    slices_by_channel.push_back(slice.get());
    media_slices_.push_back(std::move(slice));
  }
  media_router_ =
      std::make_unique<media::nand::NandMediaRouter>(std::move(slices_by_channel));
  media_port_->connect(*media_router_);
    return true;
  }
  if (kind == "interconnect") {
  const auto endpoint_count = checked_mul(geometry_->channels(),
                                          model.axi_interfaces);
  if (!endpoint_count || *endpoint_count > std::numeric_limits<std::uint8_t>::max()) {
    error = "Host endpoint count is not representable";
    return false;
  }
  const auto fabric_profile = make_fabric_profile(
      static_cast<std::uint8_t>(geometry_->channels()),
      static_cast<std::uint8_t>(model.axi_interfaces), model);
  if (!(error = fabric_profile.validate()).empty()) return false;
  repair_ = std::make_unique<interconnect::TsvRepairManager>(
      fabric_profile.channel_count, fabric_profile.active_lanes,
      fabric_profile.spare_lanes);
  fabric_ = std::make_unique<interconnect::InterconnectFabric>(fabric_profile, *repair_);
  pal::FlashPalConfig pal_config;
  pal_config.max_inflight = static_cast<std::size_t>(model.pal_max_inflight);
  pal_config.media_retry_budget = model.pal_media_retry_budget;
  pal_config.return_retry_budget = model.pal_return_retry_budget;
  pal_config.retry_delay_cycles = model.pal_retry_delay_cycles;
  pal_config.command_bits = 512;
  pal_config.page_bytes = config_.geometry().page_bytes;
  pal_ = std::make_unique<pal::FlashPal>(
      pal_config, events_, kPalHandlerId, *fabric_, *media_port_,
      [this](pal::PalCompletion completion) {
        if (controller_) controller_->on_pal_completion(std::move(completion));
      });
    return true;
  }
  if (kind == "controller") {
  hbf::controller::ControllerConfig controller_config;
  controller_config.max_pending_dlu = static_cast<std::size_t>(model.controller_pending_dlu);
  controller_config.accumulation_timeout_cycles =
      model.controller_accumulation_timeout;
  controller_config.queue_depth = static_cast<std::size_t>(model.controller_queue_depth);
  controller_config.cache_buffers_per_bank =
      static_cast<std::size_t>(model.controller_cache_buffers_per_bank);
  controller_config.ecc_credits = static_cast<std::size_t>(model.controller_ecc_credits);
  controller_config.scratchpad_bytes = 0;
  controller_config.backend_wait_timeout_cycles =
      model.controller_backend_timeout;
  controller_ = std::make_unique<hbf::controller::BaseDieFlashController>(
      controller_config, all_banks(*geometry_), *sequences_, *pal_,
      [this](hbf::controller::ControllerCompletion completion) {
        if (host_) host_->on_controller_completion(std::move(completion));
      });
  controller_port_ = std::make_unique<hbf::host::BaseDieControllerPort>(*controller_);
    return true;
  }
  if (kind == "host") {
  hbf::host::HostProfile host_profile;
  host_profile.channels = static_cast<std::uint8_t>(geometry_->channels());
  host_profile.axi_interfaces = static_cast<std::uint8_t>(model.axi_interfaces);
  host_profile.queue_depth_per_interface =
      static_cast<std::size_t>(model.host_queue_depth_per_interface);
  host_profile.channel_capacity_bytes = geometry_->local_capacity_bytes();
  host_ = std::make_unique<hbf::host::HbfHostProtocol>(
      host_profile, *mapper_, *controller_port_,
      [this](hbf::host::HostResponse response) {
        on_host_response(std::move(response));
      });
    return true;
  }
  if (kind == "ras") return die_domain_ != nullptr && !media_slices_.empty();
  error = "unknown production component factory kind: " + kind;
  return false;
}

void OpenHbxSystem::initialize_links() {
  const auto& model = config_.system_model();
  const auto generation_value = lifecycle_.generation();
  for (std::uint64_t channel = 0; channel < geometry_->channels(); ++channel) {
    const hbf::address::ChannelId id(channel);
    for (const auto state : {hbf::host::LinkState::InitializationPhase1,
                             hbf::host::LinkState::InitializationPhase2,
                             hbf::host::LinkState::InitializationPhase3,
                             hbf::host::LinkState::InitializationPhase4,
                             hbf::host::LinkState::Ready}) {
      if (host_->set_channel_state(id, state, generation_value) !=
          hbf::host::LinkTransitionResult::Accepted)
        throw std::logic_error("HBF link initialization failed");
    }
    for (std::uint8_t axi = 0; axi < model.axi_interfaces; ++axi) {
      const auto enabled = host_->registers().access({id, axi, 0x000C, true, 1});
      if (enabled.status != hbf::host::RegisterStatus::Success)
        throw std::logic_error("BUCC enable failed");
    }
  }
  journal_.observe({LogLevel::Debug, cycle(), EventPhase::Reset, 0, false,
      generation(), "open_hbx_system", "link_init", "ready", Token{},
      "channels=" + std::to_string(geometry_->channels()) +
          " axi_interfaces=" + std::to_string(model.axi_interfaces), 0});
}

AdmissionResult OpenHbxSystem::try_submit(SystemRequest request) {
  ++attempts_;
  if (lifecycle_.mode() != SystemMode::Running || reset_pending_ || !request.completion ||
      request.token.value() == 0 || request.ingress.generation != generation()) {
    ++rejected_;
    journal_.observe({LogLevel::Warn, cycle(), EventPhase::HostSchedule, 0,
        false, generation(), "open_hbx_system", "submit", "admission",
        request.token, "rejected:invalid_lifecycle", 0});
    return AdmissionResult::rejected(RejectionReason::InvalidLifecycle);
  }
  if (!completions_.can_register(request.token)) {
    const auto completion_snapshot = completions_.snapshot();
    if (completion_snapshot.outstanding >= config_.system_model().completion_capacity) {
      ++busy_;
      journal_.observe({LogLevel::Debug, cycle(), EventPhase::HostSchedule, 0,
          false, generation(), "open_hbx_system", "submit", "admission",
          request.token, "busy:completion_capacity", 0});
      return AdmissionResult::busy();
    }
    ++rejected_;
    journal_.observe({LogLevel::Warn, cycle(), EventPhase::HostSchedule, 0,
        false, generation(), "open_hbx_system", "submit", "admission",
        request.token, "rejected:duplicate_token", 0});
    return AdmissionResult::rejected(RejectionReason::InvalidArgument,
                                     "system token is not unique");
  }
  std::optional<std::uint64_t> updated_accepted_bytes;
  const bool flash_read = request.ingress.packet_type == hbf::host::PacketType::FlashIo &&
                          request.ingress.operation == hbf::host::HostOperation::Read;
  const bool flash_write = request.ingress.packet_type == hbf::host::PacketType::FlashIo &&
                           request.ingress.operation == hbf::host::HostOperation::Write;
  if (flash_read)
    updated_accepted_bytes = checked_add(read_accepted_bytes_,
                                         std::uint64_t{request.ingress.size_bytes});
  else if (flash_write)
    updated_accepted_bytes = checked_add(write_accepted_bytes_,
                                         std::uint64_t{request.ingress.size_bytes});
  if ((flash_read || flash_write) && !updated_accepted_bytes)
    throw std::overflow_error("Host accepted byte counter overflow");
  synchronous_response_.reset();
  admitting_host_ = true;
  hbf::host::HostAdmission admission;
  try {
    admission = host_->submit_tracked(request.ingress, cycle());
  } catch (...) {
    admitting_host_ = false;
    throw;
  }
  admitting_host_ = false;
  if (admission.admission.code != AdmissionCode::Accepted) {
    if (admission.admission.code == AdmissionCode::Busy) ++busy_;
    else ++rejected_;
    journal_.observe({admission.admission.code == AdmissionCode::Busy
                          ? LogLevel::Debug : LogLevel::Warn,
        cycle(), EventPhase::HostSchedule, 0, false, generation(),
        "open_hbx_system", "submit", "admission", request.token,
        admission.admission.code == AdmissionCode::Busy
            ? "busy:host" : "rejected:host", 0});
    synchronous_response_.reset();
    return admission.admission;
  }

  const Token system_token = request.token;
  const auto operation = request.ingress.operation;
  const auto packet_type = request.ingress.packet_type;
  const std::uint64_t request_bytes = request.ingress.size_bytes;
  const Cycle accepted_at = cycle();
  const auto registered = completions_.register_request(
      system_token, generation(),
      [this, callback = std::move(request.completion), operation, packet_type, request_bytes,
       accepted_at](Completion completion) mutable {
        SystemCompletion result;
        result.token = completion.token;
        result.generation = completion.generation;
        result.data_valid = completion.data_valid;
        result.payload = std::move(completion.payload);
        result.completed_at = cycle();
        const auto found = response_values_.find(completion.token.value());
        if (found != response_values_.end()) {
          result.command_status = found->second.command_status;
          result.completed_at = found->second.completed_at;
          result.error_info = found->second.error_info;
          response_values_.erase(found);
        } else if (completion.code == TerminalCode::AbortedByReset) {
          result.command_status = 0xE;
        }
        const bool valid_flash_read =
            packet_type == hbf::host::PacketType::FlashIo &&
            operation == hbf::host::HostOperation::Read && result.data_valid &&
            result.payload.size() == request_bytes &&
            (result.command_status == 0 || result.command_status == 0x5);
        const bool successful_flash_write =
            packet_type == hbf::host::PacketType::FlashIo &&
            operation == hbf::host::HostOperation::Write &&
            result.command_status == 0;
        const bool flash_io = packet_type == hbf::host::PacketType::FlashIo;
        if (flash_io && !valid_flash_read && !successful_flash_write) {
          ++failed_requests_;
        } else if (valid_flash_read) {
          ++read_completed_requests_;
          read_completed_bytes_ = add_counter(
              read_completed_bytes_, request_bytes,
              "System completed read byte counter overflow");
        } else if (successful_flash_write) {
          ++write_completed_requests_;
          write_completed_bytes_ = add_counter(
              write_completed_bytes_, request_bytes,
              "System completed write byte counter overflow");
        }
        if (flash_io) {
          const std::uint64_t completed_cycle = result.completed_at.value();
          const std::uint64_t latency = completed_cycle >= accepted_at.value()
                                            ? completed_cycle - accepted_at.value()
                                            : 0;
          if (latency_samples_ == 0) {
            latency_min_cycles_ = latency;
            first_completion_cycle_ = completed_cycle;
            last_completion_cycle_ = completed_cycle;
          } else if (latency < latency_min_cycles_) {
            latency_min_cycles_ = latency;
          }
          if (latency > latency_max_cycles_) latency_max_cycles_ = latency;
          if (completed_cycle < first_completion_cycle_)
            first_completion_cycle_ = completed_cycle;
          if (completed_cycle > last_completion_cycle_)
            last_completion_cycle_ = completed_cycle;
          ++latency_samples_;
          latency_sum_cycles_ = add_counter(
              latency_sum_cycles_, latency,
              "System latency cycle counter overflow");
        }
        ++callbacks_;
        try {
          callback(std::move(result));
        } catch (...) {
          ++callback_errors_;
          journal_.observe({LogLevel::Error, cycle(), EventPhase::FinalDelivery,
              0, false, completion.generation, "open_hbx_system", "callback",
              "final_delivery", completion.token, "exception", 0});
        }
      });
  if (registered.code != AdmissionCode::Accepted)
    throw std::logic_error("completion capacity changed during single-threaded admission");
  completions_.mark_issued(system_token, generation());
  if (flash_read) read_accepted_bytes_ = *updated_accepted_bytes;
  if (flash_write) write_accepted_bytes_ = *updated_accepted_bytes;
  ++accepted_;
  if (synchronous_response_) {
    if (synchronous_response_->token != admission.token)
      throw std::logic_error("synchronous Host response token mismatch");
    auto response = std::move(*synchronous_response_);
    synchronous_response_.reset();
    deliver_host_response(system_token, std::move(response));
  } else {
    response_tokens_.emplace(admission.token.value(), system_token);
  }
  return AdmissionResult::accepted();
}

void OpenHbxSystem::on_host_response(hbf::host::HostResponse response) {
  const auto found = response_tokens_.find(response.token.value());
  if (found == response_tokens_.end()) {
    if (!admitting_host_)
      throw std::logic_error("Host produced a response without system correlation");
    if (synchronous_response_)
      throw std::logic_error("multiple synchronous Host responses for one admission");
    synchronous_response_ = std::move(response);
    return;
  }
  const Token system_token = found->second;
  response_tokens_.erase(found);
  deliver_host_response(system_token, std::move(response));
}

void OpenHbxSystem::deliver_host_response(Token token,
                                          hbf::host::HostResponse response) {
  const auto code = response.command_status == 0 ? TerminalCode::Success
                                                  : TerminalCode::Failed;
  const auto finish = completions_.stage_terminal(
      {token, response.generation, code, response.data_valid,
       response.data_valid ? response.payload : PayloadHandle{}});
  if (finish == FinishCode::DuplicateToken && reset_target_generation_ &&
      response.generation != *reset_target_generation_)
    return;
  if (finish != FinishCode::Delivered)
    throw std::logic_error("Host response violated system completion contract");
  journal_.observe({LogLevel::Debug, cycle(), EventPhase::FinalDelivery, 0,
      false, response.generation, "open_hbx_system", "respond", "staged",
      token, response.command_status == 0 ? "success" : "failed",
      response.command_status});
  response_values_[token.value()] = response;
  const Generation event_generation = reset_target_generation_.value_or(generation());
  if (!schedule_system_event(token, EventPhase::FinalDelivery, event_generation))
    throw std::logic_error("could not schedule Host FinalDelivery");
}

bool OpenHbxSystem::schedule_system_event(Token token, EventPhase phase,
                                          Generation event_generation) {
  Cycle due = cycle();
  EventPhase cursor = tick_in_progress_ ? current_phase_ : EventPhase::Reset;
  if (phase == EventPhase::Reset || (tick_in_progress_ && current_phase_ >= phase)) {
    const auto next = checked_add(due.value(), 1);
    if (!next) return false;
    due = Cycle(*next);
    cursor = EventPhase::Reset;
  }
  EventSpec event{due, phase, kSystemHandlerId, event_generation, EventPayload(token)};
  return events_.schedule(event, cycle(), cursor) == ScheduleCode::Accepted;
}

void OpenHbxSystem::on_system_event(EventPayload payload) {
  if (current_phase_ == EventPhase::Reset) {
    execute_reset();
    return;
  }
  if (current_phase_ != EventPhase::FinalDelivery)
    throw std::logic_error("System handler dispatched in an invalid phase");
  const auto* token = std::get_if<Token>(&payload);
  const auto finish = token ? completions_.deliver_once(*token) : FinishCode::UnknownToken;
  if (finish == FinishCode::DuplicateToken) return;
  if (finish != FinishCode::Delivered)
    throw std::logic_error("FinalDelivery violated completion contract for token " +
                           std::to_string(token ? token->value() : 0) +
                           " finish=" + std::to_string(static_cast<int>(finish)));
}

void OpenHbxSystem::tick() {
  const Cycle now = cycle();
  tick_in_progress_ = true;
  current_phase_ = EventPhase::Reset;
  events_.dispatch_due(now, current_phase_, generation());
  current_phase_ = EventPhase::MediaCommit;
  events_.dispatch_due(now, current_phase_, generation());
  current_phase_ = EventPhase::ControllerCompletion;
  events_.dispatch_due(now, current_phase_, generation());
  current_phase_ = EventPhase::Interconnect;
  events_.dispatch_due(now, current_phase_, generation());
  current_phase_ = EventPhase::CreditReturn;
  events_.dispatch_due(now, current_phase_, generation());
  current_phase_ = EventPhase::ControllerSchedule;
  controller_->pump(now);
  events_.dispatch_due(now, current_phase_, generation());
  current_phase_ = EventPhase::HostSchedule;
  host_->pump(now);
  events_.dispatch_due(now, current_phase_, generation());
  current_phase_ = EventPhase::FinalDelivery;
  events_.dispatch_due(now, current_phase_, generation());
  tick_in_progress_ = false;
  lifecycle_.advance_cycle();
  ++ticks_;
  const std::uint64_t sample_cycle = cycle().value();
  if (bandwidth_log_interval_cycles_ != 0 &&
      sample_cycle - bandwidth_log_last_cycle_ >= bandwidth_log_interval_cycles_) {
    const std::uint64_t interval_cycles = sample_cycle - bandwidth_log_last_cycle_;
    const std::uint64_t interval_read_bytes =
        read_completed_bytes_ - bandwidth_log_last_read_bytes_;
    const std::uint64_t interval_write_bytes =
        write_completed_bytes_ - bandwidth_log_last_write_bytes_;
    const double time_scale = 1000.0 / config_.system_model().tck_picoseconds;
    const double read_gigabytes_per_second =
        static_cast<double>(interval_read_bytes) / interval_cycles * time_scale;
    const double write_gigabytes_per_second =
        static_cast<double>(interval_write_bytes) / interval_cycles * time_scale;
    const auto old_flags = std::clog.flags();
    const auto old_precision = std::clog.precision();
    std::clog << std::fixed << std::setprecision(3)
              << "[OpenHBX][bandwidth] cycles=" << bandwidth_log_last_cycle_
              << '-' << sample_cycle
              << " read_GBps=" << read_gigabytes_per_second
              << " write_GBps=" << write_gigabytes_per_second
              << '\n';
    std::clog.flags(old_flags);
    std::clog.precision(old_precision);
    bandwidth_log_last_cycle_ = sample_cycle;
    bandwidth_log_last_read_bytes_ = read_completed_bytes_;
    bandwidth_log_last_write_bytes_ = write_completed_bytes_;
  }
}

void OpenHbxSystem::enable_periodic_bandwidth_log(
    std::uint64_t interval_cycles) {
  bandwidth_log_interval_cycles_ = interval_cycles;
  bandwidth_log_last_cycle_ = cycle().value();
  bandwidth_log_last_read_bytes_ = read_completed_bytes_;
  bandwidth_log_last_write_bytes_ = write_completed_bytes_;
}

bool OpenHbxSystem::reset() {
  if (lifecycle_.mode() != SystemMode::Running || reset_pending_) {
    journal_.observe({LogLevel::Warn, cycle(), EventPhase::Reset, 0, false,
        generation(), "open_hbx_system", "reset", "request", Token{},
        "rejected:not_running_or_pending", 0});
    return false;
  }
  if (!checked_add(generation().value(), 1)) return false;
  reset_pending_ = true;
  if (!schedule_system_event(Token(1), EventPhase::Reset, generation())) {
    reset_pending_ = false;
    return false;
  }
  return true;
}

void OpenHbxSystem::execute_reset() {
  if (!reset_pending_) throw std::logic_error("unexpected reset event");
  const Generation old = generation();
  const auto next_value = checked_add(old.value(), 1);
  if (!next_value) throw std::overflow_error("reset generation overflow");
  const Generation next(*next_value);
  reset_target_generation_ = next;
  // Preserve already-staged terminal delivery before downstream reset paths
  // discard old-generation events.
  events_.rebase_generation(old, next, kSystemHandlerId, EventPhase::FinalDelivery);
  host_->reset(next, cycle());
  // Per-slice reset with scoped discard (CHAN-007); DieDomain state
  // (BBT, blocked dies) intentionally survives, matching prior behavior.
  for (const auto& slice : media_slices_) slice->reset(next, cycle());
  const auto fallback = completions_.stage_abort_generation(old);
  if (!lifecycle_.reset_without_completion_abort())
    throw std::logic_error("lifecycle reset failed");
  for (Token token : fallback)
    if (!schedule_system_event(token, EventPhase::FinalDelivery, next))
      throw std::logic_error("could not schedule reset FinalDelivery");
  response_tokens_.clear();
  synchronous_response_.reset();
  reset_target_generation_.reset();
  reset_pending_ = false;
  initialize_links();
  ++resets_;
  journal_.observe({LogLevel::Info, cycle(), EventPhase::Reset, 0, false, next,
      "open_hbx_system", "reset", "complete", Token{},
      "generation=" + std::to_string(next.value()), 0});
}

std::vector<IdleReason> OpenHbxSystem::module_idle() const {
  std::vector<IdleReason> result;
  if (host_->snapshot().outstanding != 0) result.push_back({"host", "requests outstanding"});
  if (controller_->snapshot().outstanding != 0) result.push_back({"controller", "work outstanding"});
  if (pal_->snapshot().inflight != 0) result.push_back({"pal", "requests in flight"});
  for (const auto& slice : media_slices_) {
    if (slice->snapshot().inflight != 0) {
      result.push_back({"media", "commands in flight"});
      break;
    }
  }
  if (fabric_->snapshot().active_reservations != 0)
    result.push_back({"fabric", "transfers reserved"});
  if (!response_tokens_.empty()) result.push_back({"system", "Host response correlation pending"});
  return result;
}

DrainResult OpenHbxSystem::drain(std::uint64_t cycle_budget) {
  return lifecycle_.drain(cycle_budget, [this] { tick(); },
                          [this] { return module_idle(); });
}

SystemSnapshot OpenHbxSystem::snapshot() const {
  return lifecycle_.snapshot(module_idle());
}

OpenHbxSystemStats OpenHbxSystem::stats() const {
  return {attempts_, accepted_, busy_, rejected_, callbacks_, callback_errors_,
          resets_, ticks_, completions_.snapshot().outstanding,
          read_accepted_bytes_, write_accepted_bytes_,
          read_completed_requests_, read_completed_bytes_,
          write_completed_requests_, write_completed_bytes_, failed_requests_,
          latency_samples_, latency_sum_cycles_, latency_min_cycles_,
          latency_max_cycles_, first_completion_cycle_, last_completion_cycle_};
}

std::optional<FlatAddressDecode> OpenHbxSystem::decode_flat_address(
    std::uint64_t flat_address_bytes) const {
  if (!geometry_ ||
      flat_address_bytes >= config_.geometry().capacity_bytes)
    return std::nullopt;
  const auto channel_capacity = geometry_->local_capacity_bytes();
  const auto axi_count = static_cast<std::uint64_t>(axi_interfaces());
  if (channel_capacity == 0 || axi_count == 0) return std::nullopt;
  const auto axi_capacity = channel_capacity / axi_count;
  if (axi_capacity == 0) return std::nullopt;
  const hbf::address::ChannelId channel(flat_address_bytes / channel_capacity);
  const auto channel_local = flat_address_bytes % channel_capacity;
  return FlatAddressDecode{channel,
                           static_cast<std::uint8_t>(channel_local / axi_capacity),
                           channel_local % axi_capacity, channel_local};
}

std::optional<media::MediaSnapshot> OpenHbxSystem::media_snapshot(
    ChannelId channel) const {
  if (channel.value() >= media_slices_.size()) return std::nullopt;
  return media_slices_[channel.value()]->snapshot();
}

std::optional<AddressDebugSnapshot> OpenHbxSystem::debug_address_snapshot(
    std::uint64_t flat_address_bytes) const {
  if (!mapper_ || !sequences_ || !topology_ || media_slices_.empty())
    return std::nullopt;
  const auto decoded = decode_flat_address(flat_address_bytes);
  if (!decoded) return std::nullopt;
  const auto mapped = mapper_->map(
      decoded->channel,
      hbf::address::LocalByteAddress(decoded->channel_local_bytes));
  if (!mapped) return std::nullopt;
  const auto& address = mapped.mapped->address;
  const hbf::address::BlockKey block{address.channel, address.owned_bank,
                                     address.block};
  const media::PhysicalAddress physical{
      address.physical_bank.core_die.value(), address.physical_bank.die.value(),
      address.physical_bank.bank.value(), address.block.value(), address.page.value()};
  // Route to the channel slice owning this physical bank (CHAN-003).
  const auto owner = topology_->owner_of(address.physical_bank);
  if (!owner || owner->value() >= media_slices_.size()) return std::nullopt;
  const auto& slice = media_slices_[owner->value()];
  return AddressDebugSnapshot{address, physical, sequences_->snapshot(block),
                              slice->page_is_valid(physical),
                              slice->block_erase_count(physical)};
}

Cycle OpenHbxSystem::cycle() const noexcept { return lifecycle_.cycle(); }
Generation OpenHbxSystem::generation() const noexcept { return lifecycle_.generation(); }
Token OpenHbxSystem::allocate_request_token() noexcept {
  if (next_external_token_ == 0) return Token{};
  const Token result(next_external_token_);
  if (next_external_token_ == std::numeric_limits<std::uint64_t>::max())
    next_external_token_ = 0;
  else
    ++next_external_token_;
  return result;
}

}  // namespace openhbx
