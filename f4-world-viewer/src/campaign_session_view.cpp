// f4-world-viewer/src/campaign_session_view.cpp
//
// The "Campaign Session" window — the V-CAMP interactive surface.
//
// This is the UI half of the live campaign session (the headless half
// is f4-simulation's CampaignSession): time controls, the war-status
// block, and the generated-missions table. What it shows, top to
// bottom:
//
//   * When NO session runs: the start row (saved-flight spawn filter:
//     team combo + max-flights) + Start Session + the last error, if
//     any. Start needs a loaded world JSON (the same file the static
//     layers render) and the campaign fixtures (class table, F-16
//     config, mission profiles) — resolved from the install when one
//     is configured, else the build-tree fixtures, the campaign_qc
//     defaults.
//   * When a session runs:
//       - play/pause + the speed presets (1x/10x/60x/240x — they
//         scale WALL-CLOCK time; the sim tick stays fixed at its
//         tuned 1/60 s, the scenario player's "Fix Your Timestep"
//         contract). The campaign clock shows D# HH:MM:SS (the save's
//         epoch + the ladder's clock — one timeline).
//       - the war-status block: cycles fired, missions generated,
//         routes built/failed, aircraft drawn (C2's one pool), combat
//         losses, reinforcement fires/deliveries, live aircraft +
//         airborne, sim time. The numbers refresh once per advance(),
//         never per draw.
//       - the generated-missions table: one row per ladder intent —
//         mission, team, TOT (relative + absolute), target (click to
//         select + pan), route waypoints, aircraft count. Rows with
//         routes are SYNTHETIC (generation-to-spawn); clicking selects
//         the flight's live entity when it materialized, else the
//         target objective.
//       - Write Result JSON (the C1 ledger artifact, byte-stable) and
//         Write Back (apply_to the session's WorldState — in-memory).
//
// The window advances nothing itself — the campaign RUNNER's worker
// thread drains the session (V-THREAD; run()'s frame scope takes the
// runner's mutex around input + draw), so the canvas, the ATO window,
// and this window all see the same tick.

#include "viewer_state.hpp"
#include <f4/viewer/enum_text.hpp>

#include <f4/campaign/mission_type.hpp>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

namespace f4::viewer {

namespace {

// Absolute campaign time as D# HH:MM:SS (the shared formatter's int32
// form is fine for the display range).
void format_abs_campaign_time(std::int64_t t, char* buf,
                              std::size_t buf_size) noexcept {
    if (t < INT32_MIN || t > INT32_MAX) {
        std::snprintf(buf, buf_size, "%lld",
                      static_cast<long long>(t));
        return;
    }
    format_campaign_time(static_cast<std::int32_t>(t), buf, buf_size);
}

} // namespace

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

void ViewerApp::start_campaign_session() {
    if (!impl_->world_loaded || impl_->last_world_json_path.empty()) {
        std::snprintf(impl_->campaign_error,
                      sizeof(impl_->campaign_error),
                      "no world loaded — open a campaign first");
        return;
    }
    // One start at a time: while a create() runs on the worker the
    // Start button is disabled; a stray call here (menu accelerator)
    // is ignored rather than queued.
    if (impl_->session_starting) return;

    // The campaign fixtures, campaign_qc's own resolution ladder:
    // install data when configured, else the build-tree fixtures.
    // (F4_SOURCE_DIR / F4_BINARY_DIR / F4_MISSION_PROFILES_JSON come
    // from the viewer's compile definitions — the same defaults the QC
    // tool bakes in.)
    f4::simulation::CampaignSessionOptions opts;
    opts.world_json = std::filesystem::absolute(
        impl_->last_world_json_path);

    std::filesystem::path class_table;
#ifdef F4_SOURCE_DIR
    class_table = std::filesystem::path(F4_SOURCE_DIR) /
                  "f4-world-convert/tests/fixtures/FALCON4.ct";
#endif
    if (impl_->install && impl_->install->valid()) {
        // The install's own class table (real theater data) when the
        // user configured one — same preference the campaign load flow
        // applies for the world JSON.
        const auto& ct = impl_->install->class_table();
        if (!ct.empty() && std::filesystem::exists(ct)) {
            class_table = ct;
        }
    }
    opts.class_table = class_table;

#ifdef F4_BINARY_DIR
    opts.aircraft_config = std::filesystem::path(F4_BINARY_DIR) /
                           "generated_fixtures/f16.json";
#endif
#ifdef F4_MISSION_PROFILES_JSON
    opts.mission_profiles = std::filesystem::path(F4_MISSION_PROFILES_JSON);
#endif

    // The fixture pre-check: the session's runtime inputs are BUILD
    // artifacts — building this app's target generates them (the V-CAMP
    // add_dependencies block in f4-world-viewer/CMakeLists.txt). One
    // missing means a stale, partial, or relocated build tree; say how
    // to fix it, not just what is missing (a bare "aircraft config not
    // found" reads like a manual preparation step exists — it doesn't;
    // the rebuild IS the step).
    {
        const std::filesystem::path missing_fixtures[2] = {
            opts.aircraft_config,
            opts.mission_profiles,
        };
        const char* missing_names[2] = {
            "F-16 aircraft config",
            "mission-profile table",
        };
        for (int i = 0; i < 2; ++i) {
            if (!missing_fixtures[i].empty() &&
                std::filesystem::exists(missing_fixtures[i])) {
                continue;
            }
#ifdef F4_BINARY_DIR
            const std::string hint = "rebuild this app's target: cmake "
                                     "--build " +
                                     std::string(F4_BINARY_DIR) +
                                     " --target f4-world-viewer";
#else
            const std::string hint = "rebuild this app's target";
#endif
            std::snprintf(impl_->campaign_error,
                          sizeof(impl_->campaign_error),
                          "%s missing: %s (%s)", missing_names[i],
                          missing_fixtures[i].string().c_str(),
                          hint.c_str());
            impl_->status_msg = "Campaign session failed to start";
            return;
        }
    }

    opts.team = impl_->campaign_start_team;
    opts.mission = -1;              // the whole tasking picture
    opts.max_flights = impl_->campaign_start_max_flights;
    opts.tasking_cycle_sec = 1800;  // FreeFalcon's own ATM cadence
    opts.reinforce_period_sec = 43200;  // the QC's armed 12 h
    // C4: the ATM pipeline (FindBestAir replaces the C3 fallback
    // bridge this line used to arm).

    // ASYNC START: create() is pure headless (no GL/raylib/ImGui) but
    // SLOW over a real install world — the world-JSON parse, the world
    // population, hundreds of flights, thousands of squadron parked
    // aircraft. The old synchronous call froze the window for the
    // whole build (the user reported "froze for a long time"); the
    // worker keeps the UI alive and honest ("Starting session…") and
    // adopt_session_start() lands the result on the main thread.
    impl_->session_starting = true;
    impl_->campaign_error[0] = '\0';
    impl_->status_msg = "Starting campaign session — building the war "
                        "(large installs take a while)…";
    // packaged_task: the future comes from the task (BEFORE the thread
    // launches — no get_future race), the thread moves the task in.
    std::packaged_task<Impl::SessionStartResult()> task(
        [opts = std::move(opts)]() mutable -> Impl::SessionStartResult {
            Impl::SessionStartResult r;
            r.session = f4::simulation::CampaignSession::create(
                opts, &r.error);
            return r;
        });
    impl_->session_start_future = task.get_future();
    impl_->session_start_thread = std::thread(std::move(task));
}

bool ViewerApp::adopt_session_start() {
    if (!impl_->session_starting) return false;
    if (!impl_->session_start_future.valid()) return false;
    if (impl_->session_start_future.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
        return false;  // still building — the window shows the spinner text
    }

    // create() finished: join the worker, take the result, adopt.
    if (impl_->session_start_thread.joinable()) {
        impl_->session_start_thread.join();
    }
    auto r = impl_->session_start_future.get();
    impl_->session_starting = false;

    if (r.session) {
        // V-THREAD: stop any PREVIOUS runner BEFORE the old session is
        // destroyed by the assignment below (the worker borrows it). The
        // Restart flow (stop → start) normally stops it one frame earlier
        // via process_session_stop(); this is the belt-and-braces order
        // guarantee for ANY adopt-over-a-live-session path. We are NOT
        // holding the frame lock here (adopt runs before the scope), so
        // stop()'s join is deadlock-free.
        if (impl_->session_runner) {
            impl_->session_runner->stop();
            impl_->session_runner.reset();
        }
        impl_->session = std::move(r.session);
        // A new session starts PAUSED — the user starts the clock
        // deliberately (the tasking cycle is a 30-minute commitment at
        // 1x; an accidentally-live loop is the worse default).
        // V-SMOKE: --play opts out (headless smokes must verify the
        // campaign actually ADVANCES — the starved-worker regression
        // shipped invisible precisely because no smoke ever ran the
        // clock).
        impl_->session->set_paused(!impl_->session_auto_play);
        impl_->sel_kind = Impl::SelectionKind::None;
        impl_->sel_entity = f4::entities::EntityId{};
        // V-THREAD: launch the campaign runner — the worker thread that
        // owns advance() from now on (the frame read scope in run()
        // locks the runner's mutex; the old inline per-frame advance is
        // gone). Starts with the current speed preset, paused unless
        // --play; the worker idles (no debt accrues) until Play.
        const int idx = std::clamp(impl_->campaign_speed_index, 0,
                                   kSessionSpeedCount - 1);
        impl_->session_runner =
            std::make_unique<f4::simulation::CampaignSessionRunner>(
                *impl_->session, kSessionSpeedTable[idx],
                /*paused=*/!impl_->session_auto_play);
        impl_->session_runner->start();
        // V-3DLIVE: reset the camera-bubble tracking (a fresh session
        // re-points the bubble on the next camera move).
        impl_->last_bubble_zoom = -1.0f;
        impl_->last_bubble_gx = -1.0e9f;
        impl_->last_bubble_gy = -1.0e9f;
        impl_->status_msg = impl_->session_auto_play
            ? "Campaign session started (running)"
            : "Campaign session started (paused)";
        return true;
    }
    std::snprintf(impl_->campaign_error, sizeof(impl_->campaign_error),
                  "%s", r.error.c_str());
    impl_->status_msg = "Campaign session failed to start";
    return false;
}

bool ViewerApp::request_campaign_session() {
    if (impl_->session_starting) return false;
    start_campaign_session();
    return impl_->session_starting;
}

bool ViewerApp::campaign_session_starting() const noexcept {
    return impl_->session_starting;
}

bool ViewerApp::campaign_session_live() const noexcept {
    return impl_->session != nullptr;
}

void ViewerApp::set_session_auto_play(bool enabled) noexcept {
    impl_->session_auto_play = enabled;
}

void ViewerApp::request_exit() noexcept {
    // V-SMOKE: thread-safe (the --screenshot timeout thread calls this).
    // run()'s loop unwinds through the FULL epilogue — runner stop +
    // join, the session exit summary, the GL-context-safe texture
    // unloads, CloseWindow — instead of std::exit() mid-frame, which
    // skipped all of it (and, with the runner now alive, would have
    // killed the process with a joinable worker thread attached).
    impl_->exit_requested.store(true);
}

std::string ViewerApp::session_exit_summary() const {
    // V-SMOKE: one line a headless --session --play run can assert on.
    // Safe any time after run() stopped the runner (the worker is
    // joined; the session is frozen). Reading a live session is ALSO
    // safe for callers holding the runner's lock — but the intended
    // callers (run()'s exit, main() after run()) run after the join.
    if (!impl_->session) return {};
    const auto& st = impl_->session->stats();
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "[session] sim %.1fs  campaign %lld  cycles %d  "
                  "missions %d  live %d",
                  impl_->session->sim().sim_time_s(),
                  static_cast<long long>(impl_->session->campaign_time()),
                  st.cycles, st.intents, st.live_aircraft);
    return buf;
}

void ViewerApp::stop_campaign_session() {
    if (impl_->session_starting) {
        // A start is still building: wait for it (the create is finite;
        // blocking here is the honest, simple contract) and discard.
        if (impl_->session_start_thread.joinable()) {
            impl_->session_start_thread.join();
        }
        if (impl_->session_start_future.valid()) {
            impl_->session_start_future.get();  // discard (dtor frees)
        }
        impl_->session_starting = false;
        impl_->status_msg = "Campaign session start cancelled";
        return;
    }
    if (!impl_->session) return;
    // V-THREAD DEFERRED STOP: this runs from an ImGui button — INSIDE
    // run()'s frame session-lock scope. runner->stop() joins a worker
    // that may be waiting on that very lock = self-deadlock. So the
    // button only sets the flag; run() performs the actual
    // stop-join-reset one step later, right AFTER the frame scope
    // releases the lock (at most one frame of latency).
    impl_->session_stop_requested = true;
    impl_->session_stop_target = impl_->session.get();
    impl_->campaign_time_dilated = false;
}

void ViewerApp::set_session_paused(bool paused) {
    if (!impl_->session) return;
    // The frame session-lock scope is held by every caller (window
    // button, Space shortcut, Campaign menu), so the runner's
    // ATOMIC-ONLY setter is the safe call (the locking set_paused()
    // would re-lock the mutex we already hold = self-deadlock) and
    // mirroring the session's own flag directly is consistent.
    if (impl_->session_runner) {
        impl_->session_runner->set_paused_flag(paused);
    }
    impl_->session->set_paused(paused);
}

void ViewerApp::write_result_json() {
    if (!impl_->session) return;
    // campaign_result.json next to the world JSON (the QC
    // artifact's own location convention).
    const auto out = impl_->last_world_json_path.parent_path() /
                     "campaign_result.json";
    FILE* f = std::fopen(out.string().c_str(), "wb");
    if (f) {
        const std::string json = impl_->session->ledger_json();
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        impl_->status_msg = "Wrote " + out.string();
    } else {
        impl_->status_msg = "Cannot write " + out.string();
    }
}

void ViewerApp::process_session_stop() {
    // run() calls this every frame, OUTSIDE the frame session lock —
    // the safe place to join the worker and drop the session.
    if (!impl_->session_stop_requested) return;
    impl_->session_stop_requested = false;

    // A stop targets the session that was live when it was requested.
    // If adopt_session_start landed a DIFFERENT session first (the menu's
    // Reset = stop + start, racing a fast create), the request is stale —
    // the adopt already stopped the old runner, and the fresh session
    // must survive.
    if (impl_->session.get() != impl_->session_stop_target) {
        impl_->session_stop_target = nullptr;
        return;
    }
    impl_->session_stop_target = nullptr;

    if (impl_->session_runner) {
        impl_->session_runner->stop();
        impl_->session_runner.reset();
    }
    impl_->session.reset();
    if (impl_->sel_kind == Impl::SelectionKind::LiveAircraft) {
        impl_->sel_kind = Impl::SelectionKind::None;
        impl_->sel_entity = f4::entities::EntityId{};
    }
    impl_->status_msg = "Campaign session stopped";
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

void ViewerApp::draw_campaign_session_view() {
    if (!impl_->show_campaign_window) return;
    if (!impl_->world_loaded) return;  // nothing to run a session over

    ImGui::SetNextWindowPos(ImVec2(620, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Campaign Session", &impl_->show_campaign_window,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // --- No session: the start row --------------------------------------
    if (!impl_->session) {
        if (impl_->session_starting) {
            // create() is running on the worker thread: keep the window
            // alive + honest (the pre-async build froze the whole app
            // here — "not responding" — for the entire session build).
            ImGui::TextDisabled(
                "Starting session — building the war\n"
                "(world load, flight spawn, airbase wiring;\n"
                "large installs take tens of seconds)…");
            ImGui::Separator();
            if (ImGui::Button("Cancel Start", ImVec2(160, 0))) {
                stop_campaign_session();
            }
            if (impl_->campaign_error[0] != '\0') {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.9f, 0.4f, 0.4f, 1));
                ImGui::TextWrapped("%s", impl_->campaign_error);
                ImGui::PopStyleColor();
            }
            ImGui::End();
            return;
        }
        ImGui::TextUnformatted("Live campaign loop over this world:");
        ImGui::BulletText("C2 tasking draws the one pool, C3 routes bend "
                          "around threats, generated flights fly the sim "
                          "alongside the save's own.");
        ImGui::Separator();

        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("team filter",
                impl_->campaign_start_team < 0
                    ? "All teams"
                    : impl_->team_name_for_slot(
                          static_cast<std::uint8_t>(
                              impl_->campaign_start_team)))) {
            if (ImGui::Selectable("All teams",
                                  impl_->campaign_start_team < 0)) {
                impl_->campaign_start_team = -1;
            }
            for (std::size_t i = 0; i < impl_->teams().size(); ++i) {
                auto h = impl_->handle(impl_->teams()[i]);
                auto* cid = h.get<f4::entities::CampaignIdentityComponent>();
                const char* nm = (cid && !cid->callsign.empty())
                    ? cid->callsign.c_str() : "(empty)";
                if (ImGui::Selectable(
                        nm, impl_->campaign_start_team ==
                                static_cast<int>(i))) {
                    impl_->campaign_start_team = static_cast<int>(i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("saved flights cap",
                        &impl_->campaign_start_max_flights);
        impl_->campaign_start_max_flights =
            std::clamp(impl_->campaign_start_max_flights, 1, 449);

        // Speed preset (same control as the running view) — applied
        // when the session's runner is constructed, so the clock rate
        // is chosen BEFORE the war starts.
        for (int i = 0; i < kSessionSpeedCount; ++i) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::RadioButton(kSessionSpeedNames[i],
                                   impl_->campaign_speed_index == i)) {
                impl_->campaign_speed_index = i;
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Start Session", ImVec2(160, 0))) {
            start_campaign_session();
        }
        if (impl_->campaign_error[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.4f, 0.4f, 1));
            ImGui::TextWrapped("%s", impl_->campaign_error);
            ImGui::PopStyleColor();
        }
        ImGui::End();
        return;
    }

    // --- Session running: time controls ---------------------------------
    const auto& st = impl_->session->stats();

    // V-THREAD: Play/pause + speed presets talk to the RUNNER now (its
    // worker thread owns advance()); the presets scale wall-clock time,
    // the tick dt stays the session's fixed 1/60 s. This window draws
    // inside run()'s frame session-lock scope, so the pause flip uses
    // the runner's ATOMIC-ONLY setter (set_paused() would re-lock the
    // mutex we already hold = self-deadlock) and mirrors the session's
    // own flag directly — consistent, because the worker can't be
    // mid-advance while we hold the lock. Speed is atomic — lock-free.
    const bool session_paused = impl_->session_runner
        ? impl_->session_runner->paused()
        : impl_->session->paused();

    if (ImGui::Button(session_paused ? "Play (Space)"
                                     : "Pause (Space)",
                      ImVec2(110, 0))) {
        set_session_paused(!session_paused);
    }
    ImGui::SameLine();
    for (int i = 0; i < kSessionSpeedCount; ++i) {
        if (i > 0) ImGui::SameLine();
        // With a runner live, the ACTIVE radio mirrors the runner's
        // actual speed (the single source) — not just the last clicked
        // index — so the UI can't lie if the two ever diverge.
        const bool active = impl_->session_runner
            ? impl_->session_runner->speed() ==
                  static_cast<double>(kSessionSpeedTable[i])
            : impl_->campaign_speed_index == i;
        if (ImGui::RadioButton(kSessionSpeedNames[i], active)) {
            impl_->campaign_speed_index = i;
            // V-THREAD: the runner's speed is an atomic — lock-free,
            // no deadlock risk under the frame scope.
            if (impl_->session_runner) {
                impl_->session_runner->set_speed(kSessionSpeedTable[i]);
            }
        }
    }

    // V-3DLIVE: the camera bubble — when on (default), zooming in past
    // ~4 px/grid drives the deaggregation bubble from the camera: the
    // ground units you're looking at spawn their individual vehicles
    // and personnel (works while paused). Off = FreeFalcon's ownship
    // bubble (the first parked aircraft — tiny).
    ImGui::Checkbox("camera bubble (deagg what you zoom into)",
                    &impl_->campaign_view_bubble);
    if (!impl_->campaign_view_bubble) {
        // Turning it off mid-run: return to the ownship bubble NOW
        // (we hold the frame lock — the worker can't be mid-advance).
        if (impl_->session && impl_->last_bubble_zoom >= 0.0f) {
            impl_->session->clear_view_bubble();
            impl_->last_bubble_zoom = -1.0f;
            impl_->last_bubble_gx = -1.0e9f;
            impl_->last_bubble_gy = -1.0e9f;
        }
    }

    // The clock: absolute campaign time (the save's epoch + the
    // ladder's clock — ONE timeline with the sim).
    {
        char tbuf[24];
        format_abs_campaign_time(impl_->session->campaign_time(), tbuf,
                                 sizeof(tbuf));
        ImGui::Text("campaign time: %s", tbuf);
    }
    // Speed: requested vs MEASURED. When the CPU can't sustain the
    // preset (Debug build, huge world) the excess time is dropped and
    // every unsustainable preset moves the clock at the same rate —
    // this readout is what makes the speed control's effect visible.
    {
        const int idx = std::clamp(impl_->campaign_speed_index, 0,
                                   kSessionSpeedCount - 1);
        const double requested = impl_->session_runner
            ? impl_->session_runner->speed()
            : static_cast<double>(kSessionSpeedTable[idx]);
        const double effective = impl_->session_runner
            ? impl_->session_runner->effective_speed()
            : 0.0;
        char sbuf[96];
        if (session_paused) {
            std::snprintf(sbuf, sizeof(sbuf), "speed: %gx (paused)",
                          requested);
        } else if (effective < requested * 0.9) {
            std::snprintf(sbuf, sizeof(sbuf),
                          "speed: %gx (effective %.1fx — CPU-limited)",
                          requested, effective);
        } else {
            std::snprintf(sbuf, sizeof(sbuf), "speed: %gx", requested);
        }
        ImGui::TextUnformatted(sbuf);
        if (impl_->campaign_time_dilated) {
            ImGui::SameLine();
            ImGui::TextDisabled("(time-dilated)");
        }
    }

    ImGui::Separator();

    // --- War status ------------------------------------------------------
    ImGui::Text("cycles %d   missions %d   routes %d (failed %d, wps %d)",
                st.cycles, st.intents, st.routes_built, st.routes_failed,
                st.route_waypoints);
    ImGui::Text("drawn %d   losses %d   reinforced %d (fires %d)",
                st.drawn_aircraft, st.air_losses, st.reinforced,
                st.reinforce_fires);
    // C4: the ATM pipeline's own line (the session's default tasking).
    if (st.packages > 0 || st.recovered > 0) {
        ImGui::Text("packages %d (escorts %d)   recovered %d",
                    st.packages, st.escorts, st.recovered);
    }
    ImGui::Text("live aircraft %d (%d airborne)   synthetic %d   sim %.0fs",
                st.live_aircraft, st.airborne, st.synthetic_spawned,
                st.sim_time_s);

    ImGui::Separator();

    // --- Generated missions table ---------------------------------------
    const auto& intents = impl_->session->intents();
    ImGui::TextUnformatted("Generated missions (ATM packages):");
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuterH |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("session_missions", 7, table_flags,
                          ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("mission", ImGuiTableColumnFlags_WidthFixed,
                                130.0f, 0);
        ImGui::TableSetupColumn("role", ImGuiTableColumnFlags_WidthFixed,
                                54.0f, 6);
        ImGui::TableSetupColumn("team", ImGuiTableColumnFlags_WidthFixed,
                                56.0f, 1);
        ImGui::TableSetupColumn("TOT", ImGuiTableColumnFlags_WidthFixed,
                                100.0f, 2);
        ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthStretch,
                                130.0f, 3);
        ImGui::TableSetupColumn("wps", ImGuiTableColumnFlags_WidthFixed,
                                32.0f, 4);
        ImGui::TableSetupColumn("ac", ImGuiTableColumnFlags_WidthFixed,
                                32.0f, 5);
        ImGui::TableHeadersRow();

        // Newest last; clipper for the long runs.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(intents.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd;
                 ++i) {
                const auto& in = intents[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(in.mission_name.c_str());
                ImGui::TableNextColumn();
                // C4: the flight's package role (main / +SEAD escort /
                // fighter escort) — the pairing the ATM composed.
                switch (in.flight_role) {
                    case 1:
                        ImGui::TextDisabled("+sead");
                        break;
                    case 2:
                        ImGui::TextDisabled("+esc");
                        break;
                    default:
                        ImGui::TextDisabled("main");
                        break;
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    in.team_name.empty() ? "(?)"
                                         : in.team_name.c_str());
                ImGui::TableNextColumn();
                {
                    // TOT in ABSOLUTE campaign time (epoch + the
                    // intent's relative TOT).
                    char tbuf[24];
                    format_abs_campaign_time(
                        impl_->session->world_state()
                                .campaign.current_time +
                            in.time_on_target,
                        tbuf, sizeof(tbuf));
                    ImGui::TextUnformatted(tbuf);
                }
                ImGui::TableNextColumn();
                {
                    // Click the target: select the objective + pan.
                    if (in.target_objective_id != 0) {
                        const auto it = impl_->session->objective_id_map()
                            .find(in.target_objective_id);
                        if (it != impl_->session->objective_id_map().end() &&
                            it->second.valid()) {
                            auto th = impl_->session_handle(it->second);
                            auto* ot = th.get<
                                f4::entities::ObjectiveTypeComponent>();
                            const std::string name =
                                ot ? ot->class_name : std::string{};
                            if (ImGui::Selectable(
                                    name.empty() ? "(objective)"
                                                 : name.c_str(),
                                    false, ImGuiSelectableFlags_None)) {
                                impl_->sel_kind =
                                    Impl::SelectionKind::Objective;
                                impl_->sel_entity = it->second;
                                if (auto* ttr = th.get<
                                        f4::entities::TransformComponent>()) {
                                    impl_->cam_x = Impl::grid_x(ttr);
                                    impl_->cam_y = Impl::grid_y(ttr);
                                    impl_->cam_zoom =
                                        std::max(impl_->cam_zoom, 4.0f);
                                }
                            }
                        } else {
                            ImGui::TextDisabled("#%u",
                                in.target_objective_id);
                        }
                    } else {
                        ImGui::TextDisabled("-");
                    }
                }
                ImGui::TableNextColumn();
                ImGui::Text("%zu", in.route.size());
                ImGui::TableNextColumn();
                ImGui::Text("%d", in.aircraft_count);
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();

    // --- Artifacts + stop ------------------------------------------------
    if (ImGui::Button("Write Result JSON")) {
        write_result_json();
    }
    ImGui::SameLine();
    if (ImGui::Button("Write Back")) {
        // apply_to the session's WorldState — pools, squadron
        // counters, objective fstatus (in-memory; the .cam re-encoder
        // is a future tranche).
        const auto wb = impl_->session->apply_writeback();
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "write-back: %d team pools, %d squadrons, "
                      "%d objectives (unmatched: %zu sq / %zu obj)",
                      wb.team_pools_written, wb.squadrons_written,
                      wb.objectives_written,
                      wb.unmatched_squadrons.size(),
                      wb.unmatched_objectives.size());
        impl_->status_msg = buf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop Session")) {
        stop_campaign_session();
        ImGui::End();
        return;
    }

    ImGui::End();
}

} // namespace f4::viewer
