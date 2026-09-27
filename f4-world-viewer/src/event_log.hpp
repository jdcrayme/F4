// f4-world-viewer/src/event_log.hpp
//
// The Event Log — the campaign's running log, the original game's
// scrolling theater log (the textual face of the yellow capture rings).
// The campaign event stream (CAMP-HOST-2) drains into the store every
// frame under the frame session lock; the window in
// campaign_session_view.cpp renders it, filtered.
//
// Rows are FROZEN AT ARRIVAL — the label resolves objective ids through
// the live session and stores the finished text, so the log reads back
// after the session is gone. This header is deliberately ImGui-free:
// the store + formatters are pure and unit-tested (test_event_log.cpp).

#pragma once

#include <f4/campaign/api/events.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace f4::viewer {

// One log row — self-contained display text (the window needs nothing
// but this). `team` is the owning team slot for the row's color dot,
// 0xFF = teamless family.
struct EventLogRow {
    static constexpr std::size_t kLabelCap = 176;

    double arrived_wall{0.0};      ///< GetTime() at arrival (debug/order)
    std::int64_t abs_t{0};         ///< save epoch + the envelope's `t`
    std::uint8_t team{0xFF};
    char label[kLabelCap] = "";
};

// The log store — newest LAST (a running log fills downward). UI-thread
// only: filled by the frame drain, read by the window's draw. Survives
// a stopped session so the log reads after the war ends; a fresh adopt
// clears it.
using EventLogStore = std::deque<EventLogRow>;

// Format + append + cap the ring (oldest rows fall off the front).
// `objective_name` resolves an objective id to a display name (""
// = unknown — the label carries the raw id instead).
void event_log_append(EventLogStore& log,
                      const f4::campaign::api::CampaignEvent& ev,
                      double wall_now, std::int64_t epoch_s,
                      const std::function<std::string(std::uint32_t)>&
                          objective_name,
                      std::size_t cap = 2000);

// The event's campaign-clock second, per family (the envelope's `t`).
std::int64_t campaign_event_time(
    const f4::campaign::api::CampaignEvent& ev) noexcept;

// The owning team slot for row coloring; 0xFF = teamless family.
std::uint8_t campaign_event_team(
    const f4::campaign::api::CampaignEvent& ev) noexcept;

// The event's row text. EVERY v1 family has a face here — the Campaign
// Session window's compact feed dropped the pilot/roe/slot lines; the
// log never does.
bool format_campaign_event_label(
    const f4::campaign::api::CampaignEvent& ev,
    const std::function<std::string(std::uint32_t)>& objective_name,
    char* buf, std::size_t cap);

// Case-insensitive ASCII substring filter; an empty needle matches all.
bool event_log_matches(const char* filter, const char* label) noexcept;

// Absolute campaign time as D# HH:MM:SS (the shared formatter's int32
// form is fine for the display range).
void format_event_log_time(std::int64_t t, char* buf,
                           std::size_t buf_size) noexcept;

} // namespace f4::viewer
