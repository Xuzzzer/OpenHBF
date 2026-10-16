#include "openhbx/system/event_queue.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace openhbx {
namespace {
const char* schedule_result(ScheduleCode code) {
  switch (code) {
    case ScheduleCode::Accepted: return "accepted";
    case ScheduleCode::PastCycle: return "past_cycle";
    case ScheduleCode::ClosedPhase: return "closed_phase";
    case ScheduleCode::UnknownHandler: return "unknown_handler";
    case ScheduleCode::SequenceExhausted: return "sequence_exhausted";
  }
  return "unknown";
}

Token payload_token(const EventPayload& payload) {
  const auto* token = std::get_if<Token>(&payload);
  return token == nullptr ? Token{} : *token;
}
}  // namespace

bool EventQueue::Later::operator()(const Event& lhs, const Event& rhs) const {
  return std::tie(lhs.view.due, lhs.view.phase, lhs.view.sequence) >
         std::tie(rhs.view.due, rhs.view.phase, rhs.view.sequence);
}

bool EventQueue::register_handler(HandlerId id, Handler handler) {
  if (id.value() == 0 || !handler) return false;
  return handlers_.emplace(id.value(), std::move(handler)).second;
}

ScheduleCode EventQueue::schedule(EventSpec& spec, Cycle now, EventPhase current_phase) {
  ScheduleCode code = ScheduleCode::Accepted;
  if (spec.due < now) code = ScheduleCode::PastCycle;
  else if (spec.due == now && spec.phase <= current_phase) code = ScheduleCode::ClosedPhase;
  else if (handlers_.find(spec.handler.value()) == handlers_.end()) code = ScheduleCode::UnknownHandler;
  else if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) code = ScheduleCode::SequenceExhausted;
  if (code != ScheduleCode::Accepted) {
    if (observer_) observer_->observe({LogLevel::Warn, now, current_phase, 0,
        false, spec.generation, "event_queue", "schedule", "rejected",
        payload_token(spec.payload), schedule_result(code), spec.handler.value()});
    return code;
  }
  Event event{{spec.due, spec.phase, next_sequence_++, spec.handler, spec.generation},
              std::move(spec.payload)};
  const EventView observed_view = event.view;
  const Token observed_token = payload_token(event.payload);
  events_.push(std::move(event));
  ++scheduled_;
  if (observer_) observer_->observe({LogLevel::Trace, observed_view.due,
      observed_view.phase, observed_view.sequence, true, observed_view.generation,
      "event_queue", "schedule", "queued", observed_token,
      "accepted", observed_view.handler.value()});
  return ScheduleCode::Accepted;
}

std::size_t EventQueue::dispatch_due(Cycle now, EventPhase phase, Generation generation) {
  std::size_t count = 0;
  while (!events_.empty()) {
    const auto& view = events_.top().view;
    if (view.due != now || view.phase != phase) break;
    Event event = events_.top();
    events_.pop();
    if (event.view.generation != generation) {
      ++stale_;
      if (observer_) observer_->observe({LogLevel::Warn, now, phase,
          event.view.sequence, true, event.view.generation, "event_queue",
          "dispatch", "generation_check", payload_token(event.payload),
          "stale", event.view.handler.value()});
      continue;
    }
    const EventView observed_view = event.view;
    const Token observed_token = payload_token(event.payload);
    handlers_.at(event.view.handler.value())(std::move(event.payload));
    ++dispatched_;
    ++count;
    if (observer_) observer_->observe({LogLevel::Trace, now, phase,
        observed_view.sequence, true, observed_view.generation, "event_queue",
        "dispatch", "handler", observed_token, "delivered",
        observed_view.handler.value()});
  }
  return count;
}

void EventQueue::discard_generation(Generation current) {
  std::vector<Event> keep;
  while (!events_.empty()) {
    Event event = events_.top();
    events_.pop();
    if (event.view.generation == current) keep.push_back(std::move(event));
    else ++stale_;
  }
  for (auto& event : keep) events_.push(std::move(event));
}

void EventQueue::discard_generation(HandlerId handler, Generation current) {
  std::vector<Event> keep;
  while (!events_.empty()) {
    Event event = events_.top();
    events_.pop();
    if (event.view.handler != handler || event.view.generation == current)
      keep.push_back(std::move(event));
    else
      ++stale_;
  }
  for (auto& event : keep) events_.push(std::move(event));
}

std::size_t EventQueue::rebase_generation(Generation old_generation,
                                          Generation new_generation,
                                          HandlerId handler,
                                          EventPhase phase) {
  std::vector<Event> keep;
  std::size_t rebased = 0;
  while (!events_.empty()) {
    Event event = events_.top();
    events_.pop();
    if (event.view.generation == old_generation &&
        event.view.handler == handler && event.view.phase == phase) {
      event.view.generation = new_generation;
      ++rebased;
    }
    keep.push_back(std::move(event));
  }
  for (auto& event : keep) events_.push(std::move(event));
  return rebased;
}

EventQueueSnapshot EventQueue::snapshot() const {
  auto copy = events_;
  std::vector<EventView> views;
  while (!copy.empty()) { views.push_back(copy.top().view); copy.pop(); }
  return {events_.size(), scheduled_, dispatched_, stale_, std::move(views)};
}

}  // namespace openhbx
