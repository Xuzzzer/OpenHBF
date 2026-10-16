#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "openhbx/common/admission.h"
#include "openhbx/hbf/address/block_sequence.h"
#include "openhbx/hbf/controller/bank_cache_manager.h"
#include "openhbx/hbf/controller/control_plane.h"
#include "openhbx/hbf/controller/dlu_accumulator.h"
#include "openhbx/hbf/controller/ecc_pipeline.h"
#include "openhbx/hbf/controller/flash_scheduler.h"
#include "openhbx/pal/flash_pal.h"

namespace openhbx::hbf::controller {
struct ControllerConfig {
  // OCP per-channel subsystem alignment: pending/credit budgets below bound
  // ONE Host Channel's controller slice (mirrors CHAN-010's per-slice media
  // semantics). queue_depth stays per-Bank (a Bank has exactly one owner
  // channel, so bank queues are channel-exclusive by construction).
  std::size_t max_pending_dlu{0};
  std::uint64_t accumulation_timeout_cycles{0};
  std::size_t queue_depth{0};
  std::size_t cache_buffers_per_bank{0};
  std::size_t ecc_credits{0};
  std::size_t scratchpad_bytes{0};
  std::uint64_t backend_wait_timeout_cycles{0};
};
struct ControllerSnapshot {
  std::uint64_t accepted{0}, terminal{0};
  std::size_t outstanding{0}, pending_programs{0};
};

class BaseDieFlashController {
 public:
  BaseDieFlashController(ControllerConfig config,
      std::vector<address::PhysicalBank> banks, address::BlockSequence& sequences,
      pal::FlashPal& pal, ControllerCompletionSink completion);
  AdmissionResult submit(ControllerRequest request, Cycle now);
  AdmissionResult submit_admin(Token token, Generation generation,
                               AdminOpcode opcode, bool vendor_enabled, Cycle now);
  void on_pal_completion(pal::PalCompletion completion);
  void pump(Cycle now);
  void reset(Generation next_generation, Cycle now);
  ControllerSnapshot snapshot() const;
  DluAccumulator& accumulator() noexcept { return accumulator_; }
  BankCacheManager& cache() noexcept { return cache_; }
  ControlPlane& control_plane() noexcept { return control_; }

 private:
  struct BackendRecord {
    enum class Stage { Read, Erase, Program };
    ControllerRequest request;
    Stage stage{Stage::Read};
    PayloadHandle program_payload;
    std::vector<Token> host_tokens;
    std::optional<address::ProgramReservation> reservation;
    std::optional<CacheHandle> cache;
    Token work_token;
    Cycle deadline;
  };

  AdmissionResult submit_write(ControllerRequest request, Cycle now);
  AdmissionResult submit_read(ControllerRequest request, Cycle now);
  bool enqueue(BackendRecord record, WorkKind kind);
  void drive_scheduler(Cycle now);
  void terminal(Token token, ControllerStatus status, bool data_valid,
                PayloadHandle payload, Cycle now,
                ControllerErrorInfo error_info = ControllerErrorInfo::None);
  void release_ecc_credit(address::ChannelId channel);
  static ControllerStatus map_sequence(address::SequenceError error);

  ControllerConfig config_;
  address::BlockSequence& sequences_;
  pal::FlashPal& pal_;
  ControllerCompletionSink completion_;
  DluAccumulator accumulator_;
  FlashScheduler scheduler_;
  BankCacheManager cache_;
  EccPipeline ecc_;
  ControlPlane control_;
  Generation generation_{0};
  std::uint64_t next_pal_token_{1};
  std::uint64_t next_work_token_{1};
  std::map<std::uint64_t, BackendRecord> backend_;
  std::map<std::uint64_t, BackendRecord> ready_;
  std::set<std::uint64_t> seen_host_tokens_;
  // ECC decode credits are charged per owning channel (keyed by ChannelId
  // value); one channel's read burst cannot exhaust another channel's ECC
  // budget (OCP per-channel subsystem alignment).
  std::map<std::uint64_t, std::size_t> ecc_inflight_by_channel_;
  std::uint64_t accepted_{0}, terminal_{0};
};
}  // namespace openhbx::hbf::controller
