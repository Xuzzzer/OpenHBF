#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "openhbx/common/strong_types.h"

namespace openhbx {

enum class EventPhase : std::uint8_t {
  Reset, MediaCommit, ControllerCompletion, Interconnect, CreditReturn,
  ControllerSchedule, HostSchedule, FinalDelivery
};

enum class LogLevel : std::uint8_t { Error, Warn, Info, Debug, Trace };

struct ObservedEvent {
  LogLevel level{LogLevel::Info};
  Cycle cycle;
  EventPhase phase{EventPhase::Reset};
  std::uint64_t sequence{0};
  bool has_sequence{false};
  Generation generation;
  std::string module;
  std::string operation;
  std::string stage;
  Token token;
  std::string result;
  std::uint64_t resource_id{0};
};

struct EventJournalSnapshot {
  std::size_t capacity{0};
  std::uint64_t observed{0};
  std::uint64_t filtered{0};
  std::uint64_t dropped{0};
  std::uint64_t sink_errors{0};
  std::vector<ObservedEvent> events;
};

class EventObserver {
 public:
  virtual ~EventObserver() = default;
  virtual void observe(const ObservedEvent& event) noexcept = 0;
};

class EventJournal final : public EventObserver {
 public:
  using Sink = std::function<void(const ObservedEvent&)>;

  explicit EventJournal(std::size_t capacity = 256,
                        LogLevel minimum_level = LogLevel::Warn);
  void observe(const ObservedEvent& event) noexcept override;
  void set_minimum_level(LogLevel level) noexcept { minimum_level_ = level; }
  void set_sink(Sink sink) { sink_ = std::move(sink); }
  EventJournalSnapshot snapshot() const;
  void clear() noexcept;

 private:
  std::size_t capacity_;
  LogLevel minimum_level_;
  Sink sink_;
  std::deque<ObservedEvent> events_;
  std::uint64_t observed_{0};
  std::uint64_t filtered_{0};
  std::uint64_t dropped_{0};
  std::uint64_t sink_errors_{0};
};

const char* to_string(LogLevel level) noexcept;
const char* to_string(EventPhase phase) noexcept;
std::string render_event_text(const ObservedEvent& event);
std::string render_event_jsonl(const ObservedEvent& event);
// Ramulator-style single-line console rendering: "[module] [level] cycle=... ".
std::string render_event_console(const ObservedEvent& event);
// Default printing sink: writes render_event_console lines to std::clog.
// OpenHbxSystem installs it on construction; set_log_sink({}) disables output.
void console_log(const ObservedEvent& event);

}  // namespace openhbx
