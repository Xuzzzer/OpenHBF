#include "openhbx/system/observability.h"

#include <iostream>
#include <sstream>

namespace openhbx {
namespace {
bool enabled(LogLevel event, LogLevel minimum) {
  return static_cast<std::uint8_t>(event) <= static_cast<std::uint8_t>(minimum);
}

std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}
}  // namespace

EventJournal::EventJournal(std::size_t capacity, LogLevel minimum_level)
    : capacity_(capacity), minimum_level_(minimum_level) {}

void EventJournal::observe(const ObservedEvent& event) noexcept {
  ++observed_;
  if (!enabled(event.level, minimum_level_)) {
    ++filtered_;
    return;
  }
  if (events_.size() == capacity_) {
    ++dropped_;
    return;
  }
  try {
    events_.push_back(event);
    if (sink_) sink_(events_.back());
  } catch (...) {
    ++sink_errors_;
  }
}

EventJournalSnapshot EventJournal::snapshot() const {
  return {capacity_, observed_, filtered_, dropped_, sink_errors_,
          {events_.begin(), events_.end()}};
}

void EventJournal::clear() noexcept {
  events_.clear();
  observed_ = filtered_ = dropped_ = sink_errors_ = 0;
}

const char* to_string(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Error: return "error";
    case LogLevel::Warn: return "warn";
    case LogLevel::Info: return "info";
    case LogLevel::Debug: return "debug";
    case LogLevel::Trace: return "trace";
  }
  return "unknown";
}

const char* to_string(EventPhase phase) noexcept {
  switch (phase) {
    case EventPhase::Reset: return "reset";
    case EventPhase::MediaCommit: return "media_commit";
    case EventPhase::ControllerCompletion: return "controller_completion";
    case EventPhase::Interconnect: return "interconnect";
    case EventPhase::CreditReturn: return "credit_return";
    case EventPhase::ControllerSchedule: return "controller_schedule";
    case EventPhase::HostSchedule: return "host_schedule";
    case EventPhase::FinalDelivery: return "final_delivery";
  }
  return "unknown";
}

std::string render_event_text(const ObservedEvent& event) {
  std::ostringstream out;
  out << "level=" << to_string(event.level) << " cycle=" << event.cycle.value()
      << " phase=" << to_string(event.phase);
  if (event.has_sequence) out << " sequence=" << event.sequence;
  out << " generation=" << event.generation.value()
      << " module=" << event.module << " operation=" << event.operation
      << " stage=" << event.stage;
  if (event.token.value() != 0) out << " token=" << event.token.value();
  out << " result=" << event.result;
  if (event.resource_id != 0) out << " resource_id=" << event.resource_id;
  return out.str();
}

std::string render_event_jsonl(const ObservedEvent& event) {
  std::ostringstream out;
  out << "{\"level\":\"" << to_string(event.level)
      << "\",\"cycle\":" << event.cycle.value()
      << ",\"phase\":\"" << to_string(event.phase) << "\"";
  if (event.has_sequence) out << ",\"sequence\":" << event.sequence;
  out << ",\"generation\":" << event.generation.value()
      << ",\"module\":\"" << json_escape(event.module)
      << "\",\"operation\":\"" << json_escape(event.operation)
      << "\",\"stage\":\"" << json_escape(event.stage) << "\"";
  if (event.token.value() != 0) out << ",\"token\":" << event.token.value();
  out << ",\"result\":\"" << json_escape(event.result) << "\"";
  if (event.resource_id != 0) out << ",\"resource_id\":" << event.resource_id;
  out << '}';
  return out.str();
}

std::string render_event_console(const ObservedEvent& event) {
  std::ostringstream out;
  out << '[' << event.module << "] [" << to_string(event.level) << "] cycle="
      << event.cycle.value() << " phase=" << to_string(event.phase)
      << " op=" << event.operation << " stage=" << event.stage
      << " result=" << event.result;
  if (event.token.value() != 0) out << " token=" << event.token.value();
  if (event.resource_id != 0) out << " resource_id=" << event.resource_id;
  if (event.has_sequence) out << " seq=" << event.sequence;
  return out.str();
}

void console_log(const ObservedEvent& event) {
  std::clog << render_event_console(event) << '\n';
}

}  // namespace openhbx
