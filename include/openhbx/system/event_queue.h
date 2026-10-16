#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <queue>
#include <string>
#include <variant>
#include <vector>

#include "openhbx/common/payload_handle.h"
#include "openhbx/common/strong_types.h"
#include "openhbx/system/observability.h"

namespace openhbx {

enum class ScheduleCode { Accepted, PastCycle, ClosedPhase, UnknownHandler, SequenceExhausted };
using EventPayload = std::variant<std::monostate, std::uint64_t, Token, PayloadHandle>;

struct EventSpec { Cycle due; EventPhase phase; HandlerId handler; Generation generation; EventPayload payload; };
struct EventView { Cycle due; EventPhase phase; std::uint64_t sequence; HandlerId handler; Generation generation; };
struct EventQueueSnapshot { std::size_t queued; std::uint64_t scheduled; std::uint64_t dispatched; std::uint64_t stale; std::vector<EventView> events; };

class EventQueue {
 public:
  using Handler = std::function<void(EventPayload)>;
  explicit EventQueue(EventObserver* observer = nullptr) : observer_(observer) {}
  void set_observer(EventObserver* observer) noexcept { observer_ = observer; }
  bool register_handler(HandlerId id, Handler handler);
  ScheduleCode schedule(EventSpec& spec, Cycle now, EventPhase current_phase);
  std::size_t dispatch_due(Cycle now, EventPhase phase, Generation generation);
  EventQueueSnapshot snapshot() const;
  bool empty() const noexcept { return events_.empty(); }
  void discard_generation(Generation current);
  // Scoped variant (CHAN-007): discards only events owned by `handler` whose
  // generation differs from `current`; every other handler's events are
  // untouched. Used by per-channel media slice reset so one slice cannot
  // purge another slice's pending stage events.
  void discard_generation(HandlerId handler, Generation current);
  std::size_t rebase_generation(Generation old_generation,
                                Generation new_generation,
                                HandlerId handler, EventPhase phase);
 private:
  struct Event { EventView view; EventPayload payload; };
  struct Later { bool operator()(const Event& lhs, const Event& rhs) const; };
  std::priority_queue<Event, std::vector<Event>, Later> events_;
  std::map<std::uint64_t, Handler> handlers_;
  std::uint64_t next_sequence_{0}, scheduled_{0}, dispatched_{0}, stale_{0};
  EventObserver* observer_{nullptr};
};

}  // namespace openhbx
