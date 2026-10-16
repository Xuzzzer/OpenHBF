#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>

#include "openhbx/common/admission.h"
#include "openhbx/interconnect/fabric.h"
#include "openhbx/media/types.h"
#include "openhbx/pal/media_port.h"
#include "openhbx/system/event_queue.h"

namespace openhbx::pal {

struct FlashPhysicalRequest {
  Token token;
  Generation generation;
  media::CommandKind kind{media::CommandKind::Read};
  // Owning Host Channel, carried explicitly from 05's HbfAddress (CHAN-002);
  // forwarded verbatim into FlashCommand.channel. `endpoint` below remains
  // the fabric routing key only.
  ChannelId channel{};
  media::PhysicalAddress address;
  std::uint64_t endpoint{0};
  PayloadHandle payload;
};

enum class PalStatus {
  Success,
  ReadErasedPage,
  RawCorrectable,
  RawUncorrectable,
  RetrySuggested,
  RefreshNotice,
  ProgramFail,
  EraseFail,
  CapacityUnusable,
  DieTemporarilyBlocked,
  BadBlock,
  InvalidAddress,
  InvalidState,
  InvalidPayload,
  MediaIntegrityError,
  MediaRejected,
  PathUnavailable,
  Aborted,
  InternalError,
  // Compatibility names only. Production MediaStatus mapping uses the
  // lossless typed values above.
  Corrected,
  Uncorrectable
};

struct PalCompletion {
  Token token;
  PalStatus status{PalStatus::InternalError};
  bool data_valid{false};
  PayloadHandle payload;
  Cycle completed_at;
};

using PalCompletionSink = std::function<void(PalCompletion)>;

struct FlashPalConfig {
  // OCP per-channel subsystem alignment: in-flight budget of ONE Host
  // Channel's PAL slice, not a cube-global pool (mirrors media_max_inflight's
  // per-slice semantics, CHAN-010). Cube-wide in-flight = max_inflight x
  // host_channels.
  std::size_t max_inflight{0};
  std::uint64_t media_retry_budget{0};
  std::uint64_t return_retry_budget{0};
  std::uint64_t retry_delay_cycles{1};
  std::uint64_t command_bits{512};
  std::uint64_t page_bytes{4096};
};

struct FlashPalSnapshot {
  std::size_t inflight{0};
  std::uint64_t accepted{0};
  std::uint64_t terminal{0};
  std::uint64_t media_retries{0};
  std::uint64_t return_retries{0};
  std::uint64_t stale_events{0};
  std::uint64_t integrity_errors{0};
};

class FlashPal {
 public:
  FlashPal(FlashPalConfig config, EventQueue& events, HandlerId handler,
           interconnect::InterconnectFabric& fabric, IFlashMediaPort& media,
           PalCompletionSink completion);
  AdmissionResult try_issue(FlashPhysicalRequest request, Cycle now);
  void on_media_completion(media::MediaCompletion completion);
  void reset(Generation next_generation, Cycle now);
  FlashPalSnapshot snapshot() const;

 private:
  enum class Stage { ForwardInFlight, MediaPending, MediaInFlight, ReturnPending,
                     ReturnInFlight };
  struct Context {
    FlashPhysicalRequest request;
    Stage stage{Stage::ForwardInFlight};
    interconnect::TransferId transfer_id{0};
    interconnect::Route frozen_route;
    Cycle due;
    std::uint64_t media_retries{0};
    std::uint64_t return_retries{0};
    PalStatus terminal_status{PalStatus::InternalError};
    bool data_valid{false};
    PayloadHandle result_payload;
  };

  void on_event(EventPayload payload);
  bool schedule(Token token, Cycle due, Cycle now);
  void issue_media(Context& context, Cycle now);
  void begin_return(Context& context, Cycle now);
  void terminal(std::uint64_t token, Cycle now);
  static PalStatus map_status(media::MediaStatus status);

  FlashPalConfig config_;
  EventQueue& events_;
  HandlerId handler_;
  interconnect::InterconnectFabric& fabric_;
  IFlashMediaPort& media_;
  PalCompletionSink completion_;
  Generation generation_{0};
  std::map<std::uint64_t, Context> contexts_;
  // Per-channel in-flight accounting (keyed by ChannelId value). Channels
  // never share their PAL transfer budget.
  std::map<std::uint64_t, std::size_t> inflight_by_channel_;
  std::set<std::uint64_t> seen_tokens_;
  std::uint64_t next_transfer_id_{1};
  std::uint64_t accepted_{0}, terminal_{0}, media_retries_{0};
  std::uint64_t return_retries_{0};
  std::uint64_t stale_events_{0}, integrity_errors_{0};
};

}  // namespace openhbx::pal
