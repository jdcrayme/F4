// f4-world-viewer/src/campaign_session_view.cpp
//
// The "Campaign Session" window — the V-CAMP interactive surface.
//
// This is the UI half of the live campaign session. CAMP-HOST-3: the
// headless half is the f4-campaign-api CONTRACT — every read here is a
// QUERY (the per-advance snapshot in Impl), every act a typed COMMAND
// (focus/select_deagg/select_reagg/set_paused), and the engine session
// itself is only reachable through the render-plane seam. What it
// shows, top to bottom:
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
//       - Write Result JSON (the C1 ledger artifact, byte-stable, via
//         the books query) and Write Back (the contract's runtime-safe
//         save: ledger write-back + the WorldState JSON, next to the
//         world the session loaded).
//
// The window advances nothing itself — the campaign RUNNER's worker
// thread drains the session (V-THREAD on the contract; run()'s frame
// scope takes the runner's mutex around input + draw), so the canvas,
// the ATO window, and this window all see the same tick.

#include "viewer_state.hpp"
#include <f4/viewer/enum_text.hpp>
#include <f4/viewer/pipeline_io.hpp>

#include <f4/campaign/mission_type.hpp>   // display vocabulary (row bytes → names)
#include <f4/campaign/api/commands.hpp>   // the typed command surface

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

    // The session's ClassTable::load_auto is JSON-only (Tranche 0d) —
    // handing it a binary .ct path always failed at session create.
    // Use the path the campaign load resolved (canonical Data/ export
    // or the Data/Temp conversion); when unset, resolve on demand, and
    // only fall back to the committed Data/ export if that's all there
    // is. (The old fixture fallback pointed at a BINARY .ct fixture —
    // it could never have loaded.)
    std::filesystem::path class_table = impl_->class_table_json_path;
    if (class_table.empty() || !std::filesystem::exists(class_table)) {
        if (impl_->install && impl_->install->valid()) {
            try {
                class_table = ensure_class_table_json(*impl_->install);
            } catch (const std::exception&) {
                // fall through to the committed Data/ export below
            }
        }
    }
    if (class_table.empty() || !std::filesystem::exists(class_table)) {
#ifdef F4_SOURCE_DIR
        class_table = std::filesystem::path(F4_SOURCE_DIR) /
                      "Data/Classes/falcon4.ct.json";
#endif
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
    // G1/DOM-2: the ground war runs in viewer sessions — the supply
    // picture (objective stocks, battalion cut-off), the FLOT, and the
    // capture events are the mechanics the map is here to show. Without
    // this the ground layers draw nothing and the war is air-only.
    opts.ground_war = true;
    opts.ground_objective_supply = true;
    // The resupply cadence (team stock → objective stocks → battalion
    // draws). 6 h of campaign time per fire: at 240x that is one fire
    // every 90 wall-clock seconds — visible at QC speed, not a tick
    // storm. (0 = the engine's OFF default.)
    opts.ground_resupply_sec = 21600;
    // Stock-save bridge: the Steam install's stock campaigns (save0/1/2
    // + Instant) leave every squadron's home-airbase VU at 0 — the link
    // the game's own campaign engine establishes on first load, which
    // never ran on them. Unbased squadrons can carry no ATO (the ATM
    // bases flights on the squadron's airbase; routes gate on it), so
    // every session arms the nearest-friendly-airbase synthesis. Saves
    // that already carry bases (TestCamp, Auto Save) are untouched where
    // it matters: only wire-zero squadrons are ever assigned.
    opts.synthesize_airbases = true;
    // FID: the fidelity policy. Tiered is the DEFAULT (the original
    // game's own shape: flights are campaign aggregates until you zoom
    // into or select one) — full-fidelity-everything is the checkbox's
    // other side, and the pre-FID behavior exactly.
    opts.fidelity_policy = impl_->campaign_tiered
        ? f4::simulation::FidelityPolicy::Tiered
        : f4::simulation::FidelityPolicy::FullFidelity;
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
            r.session = f4::simulation::EngineSessionHost::create(
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
        // CAMP-HOST-2: arm the event stream (all kinds — the viewer is
        // the war-room client) BEFORE the runner starts, so no event is
        // fired into an unarmed bus. The filter gates what the session
        // buffers; drain_events() empties it per frame from
        // refresh_session_snapshot() (under the frame session lock).
        {
            f4::campaign::api::EventFilter all;
            all.all = true;
            impl_->session->set_event_filter(all);
        }
        // A new session starts PAUSED — the user starts the clock
        // deliberately (the tasking cycle is a 30-minute commitment at
        // 1x; an accidentally-live loop is the worse default).
        // V-SMOKE: --play opts out (headless smokes must verify the
        // campaign actually ADVANCES — the starved-worker regression
        // shipped invisible precisely because no smoke ever ran the
        // clock).
        impl_->session->set_paused(!impl_->session_auto_play);
        // CAMP-HOST-3: capture the save's EPOCH from the time query —
        // the session is paused at zero ticks here, so campaign_time_s
        // IS the absolute base (the missions table's TOT adds onto it).
        // Also reset the snapshot cache (a fresh war = fresh numbers).
        impl_->session_epoch_s =
            fetch_time(*impl_->session).campaign_time_s;
        impl_->session_snap = SessionSnapshot{};
        impl_->session_snap_serial = 0;
        impl_->session_snap_valid = false;
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
            std::make_unique<f4::viewer::CampaignClientRunner>(
                *impl_->session, impl_->session->options().sim_dt,
                kSessionSpeedTable[idx],
                /*paused=*/!impl_->session_auto_play);
        impl_->session_runner->start();
        // The session's controls live in this window — surface it when
        // the war actually comes up (the Start menu item already opened
        // it; this covers programmatic --session starts too).
        impl_->show_campaign_window = true;
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

// CAMP-HOST-3: the contract-plane snapshot refresh (see viewer_state.hpp)
// — one query round per advance (or after a command invalidates), never
// per draw. Runs under the frame session lock; the queries are
// session-safe there.
void ViewerApp::Impl::refresh_session_snapshot() {
    if (!session) return;

    // The event stream drains EVERY frame (not per advance) — a paused
    // session publishes nothing new, but the arm-point backlog and any
    // stragglers land the frame after they fire. Newest kept last; the
    // ring is capped (the feed reads the tail).
    for (auto& ev : session->drain_events()) {
        if (ev.kind == f4::campaign::api::CampaignEvent::Kind::ObjectiveCaptured) {
            capture_markers.emplace_back(
                ev.objective_captured.objective_id, GetTime());
        }
        session_events.push_back(std::move(ev));
        while (session_events.size() > 100) session_events.pop_front();
    }

    // The cut-off cache: recomputed once per advance while the supply
    // overlay is on (a battalion beyond the line-of-supply radius from
    // every own-held objective draws nothing). O(battalions × own
    // objectives) — a few hundred × a few hundred worst case, once per
    // advance, not per draw.
    const std::uint64_t serial =
        session_runner ? session_runner->step_serial() : 0;
    if (show_supply && serial != cutoff_stamp) {
        cutoff_stamp = serial;
        cutoff_battalions.clear();
        const auto* gw = session->engine().ground_war();
        if (gw) {
            // The line-of-supply radius (GroundWarConfig's default —
            // 10 grid ≈ 10 km, a division's logistics tail).
            constexpr int kSupplyRadiusGrid = 10;
            const auto& objs = gw->objectives();
            for (const auto& u : gw->units()) {
                if (u.destroyed) continue;
                bool supplied = false;
                for (const auto& o : objs) {
                    if (o.owner != u.owner) continue;
                    const float dx = float(o.x - u.x);
                    const float dy = float(o.y - u.y);
                    if (dx * dx + dy * dy <=
                        float(kSupplyRadiusGrid * kSupplyRadiusGrid)) {
                        supplied = true;
                        break;
                    }
                }
                if (!supplied) {
                    cutoff_battalions.emplace_back(
                        float(u.x) + float(u.fx) / 256.0f,
                        float(u.y) + float(u.fy) / 256.0f);
                }
            }
        }
    }

    if (session_snap_valid && serial == session_snap_serial) return;
    session_snap = fetch_snapshot(*session, show_threat_overlay);
    session_snap_serial = serial;
    session_snap_valid = true;
}

std::string ViewerApp::session_exit_summary() const {
    // V-SMOKE: one line a headless --session --play run can assert on.
    // Safe any time after run() stopped the runner (the worker is
    // joined; the session is frozen). Reading a live session is ALSO
    // safe for callers holding the runner's lock — but the intended
    // callers (run()'s exit, main() after run()) run after the join.
    if (!impl_->session) return {};
    // CAMP-HOST-3: the summary reads the QUERIES (the worker is joined;
    // the session answers them frozen — the contract plane, end to end).
    const auto st = fetch_stats(*impl_->session);
    const auto tm = fetch_time(*impl_->session);
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "[session] sim %.1fs  campaign %lld  cycles %d  "
                  "missions %d  live %d",
                  tm.sim_time_s,
                  static_cast<long long>(tm.campaign_time_s),
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
        // CAMP-HOST-3: the ledger rides the books query — one string
        // decode returns the EXACT ledger bytes (the identity hashes
        // the same bytes).
        const std::string json = fetch_books_ledger(*impl_->session);
        if (json.empty()) {
            std::fclose(f);
            impl_->status_msg = "books query returned no ledger";
            return;
        }
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

    // Left of the Inspector's top-right slot (Windows menu reopens it).
    ImGui::SetNextWindowPos(ImVec2(260, 30), ImGuiCond_FirstUseEver);
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

        // FID: the fidelity policy. Tiered (default) runs the war the
        // way the original game did — aggregates everywhere, full
        // fidelity only around the eye and at the airfield ops windows;
        // the high speed presets actually deliver. Full fidelity spawns
        // every flight's aircraft at start (the pre-FID behavior; the
        // CPU-limited readout below is its honest cost).
        ImGui::Checkbox("fidelity tiers (aggregates until observed)",
                        &impl_->campaign_tiered);

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
    const auto& st = impl_->session_snap.stats;

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
        : true;  // dead branch: a session always adopts with its runner

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
            f4::campaign::api::CommandIntent clear;
            clear.kind =
                f4::campaign::api::CommandIntent::Kind::ClearFocus;
            impl_->session->submit(clear);
            impl_->last_bubble_zoom = -1.0f;
            impl_->last_bubble_gx = -1.0e9f;
            impl_->last_bubble_gy = -1.0e9f;
        }
    }

    // The clock: absolute campaign time (the save's epoch + the
    // ladder's clock — ONE timeline with the sim). Contract plane: the
    // time query's campaign_time_s.
    {
        char tbuf[24];
        format_abs_campaign_time(impl_->session_snap.time.campaign_time_s,
                                 tbuf, sizeof(tbuf));
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
    // The tasking countdown: the ladder's first generated missions land
    // a FULL air_task_cycle_sec in (FreeFalcon's 30-minute ATM cadence
    // here) — without this line a fresh session's zero missions read
    // as "nothing happens" instead of "the first ATO wave is N away".
    {
        const int nt = std::max(0, st.next_tasking_sec);
        ImGui::Text("next tasking cycle in %d:%02d", nt / 60, nt % 60);
    }
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
    // FID: the tier machinery's one-line summary (the aggregate/live
    // split the flights table below renders row by row).
    if (impl_->campaign_tiered) {
        ImGui::Text(
            "flights %d (%d aggregate / %d live / %d home / %d lost)"
            "   deaggs %d   reaggs %d",
            st.agg_flights,
            st.agg_flights - st.agg_live - st.agg_arrived -
                st.agg_destroyed,
            st.agg_live, st.agg_arrived, st.agg_destroyed,
            st.tier_deaggs, st.tier_reaggs);
    }
    // G1/DOM-2: the ground war's books — the front shape, captures,
    // and the supply chain's counters (the map's FLOT + supply layers
    // render the same engine state spatially). Live via the
    // render-plane seam; refreshed per frame like the canvas.
    if (const auto* gw = impl_->session->engine().ground_war()) {
        const auto& gs = gw->stats();
        ImGui::Text(
            "ground: %d bn (%d mobile)   captures %d   front %d col",
            gs.battalions_alive, gs.battalions_mobile, gs.captures,
            gs.front_columns);
        ImGui::Text(
            "supply: regen %d   drawn %d   cut-offs %d   fires %d",
            gs.supply_regen_total, gs.supply_drawn_total,
            gs.cut_off_events, gs.resupply_fires);
    }

    ImGui::Separator();

    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuterH |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

    // --- FID: the flights table (select + deaggregate, the Falcon 4
    // campaign-view workflow) --------------------------------------------
    // Under the Tiered policy the save's flights are campaign
    // aggregates until something deaggregates them: the camera bubble
    // (V-3DLIVE — the 3D view deaggregates what you zoom into), an
    // airfield-ops window (takeoff/recovery), or the row buttons here
    // (the explicit "select an aircraft and de-aggregate it" act).
    // Clicking a row selects the flight entity and pans the map — the
    // same affordance the missions table's target cell uses.
    if (impl_->campaign_tiered) {
        // CAMP-HOST-3: the table reads the snapshot's FLIGHTS QUERY rows
        // (the FID tier view over the wire — FlightView is FlightTierView
        // wearing its contract hat). Team names are the viewer's own
        // world-JSON capture; the mission name maps the row's mission
        // byte (display vocabulary, not live state).
        const auto& tiers = impl_->session_snap.flights;
        const auto team_name = [this](std::uint8_t slot) {
            return impl_->team_name_for(slot);
        };
        const auto mmss = [](std::int32_t sec, char* buf, size_t cap) {
            if (sec < 0) {
                std::snprintf(buf, cap, "-");
                return;
            }
            std::snprintf(buf, cap, "%d:%02d", sec / 60, sec % 60);
        };

        ImGui::TextUnformatted(
            "Flights (click to select; D deaggregates, R folds):");
        if (ImGui::BeginTable("session_flights", 9, table_flags,
                              ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("VU", ImGuiTableColumnFlags_WidthFixed,
                                    52.0f, 0);
            ImGui::TableSetupColumn("team", ImGuiTableColumnFlags_WidthFixed,
                                    56.0f, 1);
            ImGui::TableSetupColumn("mission",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    96.0f, 2);
            ImGui::TableSetupColumn("grid",
                                    ImGuiTableColumnFlags_WidthStretch,
                                    90.0f, 3);
            ImGui::TableSetupColumn("alt", ImGuiTableColumnFlags_WidthFixed,
                                    56.0f, 4);
            ImGui::TableSetupColumn("fuel burnt",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    64.0f, 5);
            ImGui::TableSetupColumn("tier", ImGuiTableColumnFlags_WidthFixed,
                                    52.0f, 6);
            ImGui::TableSetupColumn("window",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    60.0f, 7);
            ImGui::TableSetupColumn("ops", ImGuiTableColumnFlags_WidthFixed,
                                    96.0f, 8);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(tiers.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd;
                     ++i) {
                    const auto& t = tiers[static_cast<std::size_t>(i)];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    {
                        // Click the VU: select the flight entity + pan.
                        char vbuf[16];
                        std::snprintf(vbuf, sizeof(vbuf), "%u", t.vu);
                        const auto& vu_map = impl_->unit_id_map();
                        const bool selected =
                            impl_->sel_kind == Impl::SelectionKind::Unit &&
                            impl_->session != nullptr &&
                            vu_map.find(t.vu) != vu_map.end() &&
                            vu_map.at(t.vu) == impl_->sel_entity;
                        if (ImGui::Selectable(vbuf, selected)) {
                            const auto it = vu_map.find(t.vu);
                            if (it != vu_map.end() && it->second.valid()) {
                                impl_->sel_kind = Impl::SelectionKind::Unit;
                                impl_->sel_entity = it->second;
                                impl_->cam_x = static_cast<float>(t.x_grid);
                                impl_->cam_y = static_cast<float>(t.y_grid);
                                impl_->cam_zoom =
                                    std::max(impl_->cam_zoom, 4.0f);
                            }
                        }
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        team_name(static_cast<std::uint8_t>(t.team)));
                    ImGui::TableNextColumn();
                    {
                        const std::string mname(f4::campaign::mission_type_name(
                            static_cast<std::uint8_t>(t.mission)));
                        ImGui::TextUnformatted(mname.c_str());
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f,%.0f", t.x_grid, t.y_grid);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", static_cast<double>(t.altitude_ft));
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", t.fuel_burnt);
                    ImGui::TableNextColumn();
                    if (t.destroyed) {
                        ImGui::TextDisabled("LOST");
                    } else if (t.arrived) {
                        ImGui::TextDisabled("HOME");
                    } else if (t.live) {
                        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                                           "LIVE");
                    } else {
                        ImGui::TextDisabled("AGG");
                    }
                    ImGui::TableNextColumn();
                    {
                        // The next ops window: takeoff countdown when the
                        // flight still holds at its base, else the
                        // recovery countdown. "-" = no schedule.
                        char wbuf[16];
                        const std::int32_t w =
                            t.to_depart >= 0
                                ? t.to_depart
                                : t.to_mission_over;
                        mmss(w, wbuf, sizeof(wbuf));
                        ImGui::TextUnformatted(wbuf);
                    }
                    ImGui::TableNextColumn();
                    {
                        // The explicit acts: deaggregate (ground spawn
                        // pre-takeoff or at home, air spawn otherwise)
                        // and fold back. Both are synchronous under the
                        // frame lock — the paused-session rule.
                        ImGui::PushID(static_cast<int>(t.vu));
                        if (!t.live && !t.destroyed) {
                            if (ImGui::Button("D")) {
                                // CAMP-HOST-3: the explicit act is the
                                // select_deagg COMMAND (FID-4's force
                                // wearing its wire hat). Applied
                                // immediately; a typed refusal (unknown
                                // vu) surfaces instead of a silent no-op.
                                f4::campaign::api::CommandIntent cmd;
                                cmd.kind = f4::campaign::api::CommandIntent::
                                    Kind::SelectDeagg;
                                cmd.flight = t.vu;
                                const auto ack = impl_->session->submit(cmd);
                                if (ack.status != f4::campaign::api::
                                                     CommandAck::Status::
                                                         Applied) {
                                    impl_->status_msg =
                                        "deagg refused: " + ack.detail;
                                }
                                // force-deagg mutates tier state without
                                // advancing — refresh on the next frame.
                                impl_->invalidate_session_snapshot();
                            }
                            ImGui::SameLine();
                        }
                        if (t.live) {
                            if (ImGui::Button("R")) {
                                f4::campaign::api::CommandIntent cmd;
                                cmd.kind = f4::campaign::api::CommandIntent::
                                    Kind::SelectReagg;
                                cmd.flight = t.vu;
                                const auto ack = impl_->session->submit(cmd);
                                if (ack.status != f4::campaign::api::
                                                     CommandAck::Status::
                                                         Applied) {
                                    impl_->status_msg =
                                        "reagg refused: " + ack.detail;
                                }
                                impl_->invalidate_session_snapshot();
                            }
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
    }

    // --- Generated missions table ---------------------------------------
    const auto& intents = impl_->session_snap.tasking;
    ImGui::TextUnformatted("Generated missions (ATM packages):");
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
                    // TOT in ABSOLUTE campaign time (the save's epoch —
                    // captured from the time query at adopt — plus the
                    // intent's relative TOT; the engine's own formula).
                    char tbuf[24];
                    format_abs_campaign_time(
                        impl_->session_epoch_s + in.time_on_target,
                        tbuf, sizeof(tbuf));
                    ImGui::TextUnformatted(tbuf);
                }
                ImGui::TableNextColumn();
                {
                    // Click the target: select the objective + pan.
                    if (in.target_objective_id != 0) {
                        const auto& obj_map = impl_->objective_id_map();
                        const auto it = obj_map.find(in.target_objective_id);
                        if (it != obj_map.end() && it->second.valid()) {
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
                ImGui::Text("%d", in.route_waypoints);
                ImGui::TableNextColumn();
                ImGui::Text("%d", in.aircraft_count);
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();

    // --- Event feed (CAMP-HOST-2) ----------------------------------------
    //
    // The war's narrative as it happens: missions filed, objectives
    // captured/damaged/repaired, kills, reinforcements, weather. The
    // stream is armed at adopt (all kinds) and drained every frame
    // under the frame session lock (refresh_session_snapshot); this
    // section renders the newest tail, newest first. Objective ids
    // resolve through the render-plane bridge (the missions table's
    // own pattern).
    if (ImGui::CollapsingHeader("Events",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto objective_name = [this](std::uint32_t id) {
            const auto& obj_map = impl_->objective_id_map();
            const auto it = obj_map.find(id);
            if (it == obj_map.end() || !it->second.valid()) {
                return std::string{};
            }
            auto oh = impl_->session_handle(it->second);
            auto* ot = oh.get<f4::entities::ObjectiveTypeComponent>();
            return ot ? ot->class_name : std::string{};
        };
        const auto event_time =
            [](const f4::campaign::api::CampaignEvent& ev) -> std::int64_t {
            using K = f4::campaign::api::CampaignEvent::Kind;
            switch (ev.kind) {
                case K::MissionFiled:          return ev.mission_filed.t;
                case K::Kill:                  return ev.kill.t;
                case K::ObjectiveDamage:       return ev.objective_damage.t;
                case K::ObjectiveCaptured:     return ev.objective_captured.t;
                case K::ReinforcementDelivered:
                                               return ev.reinforcement_delivered.t;
                case K::WeatherChanged:        return ev.weather_changed.t;
                case K::RoeChanged:            return ev.roe_changed.t;
                case K::TaskingCycle:          return ev.tasking_cycle.t;
                case K::ActionFiled:           return ev.action_filed.t;
                case K::Verdict:               return ev.verdict.t;
                case K::ObjectiveRepaired:     return ev.objective_repaired.t;
                case K::PilotAssigned:         return ev.pilot_assigned.t;
                case K::PilotLost:             return ev.pilot_lost.t;
                case K::PilotRecovered:        return ev.pilot_recovered.t;
                case K::SlotDenied:            return ev.slot_denied.t;
            }
            return 0;
        };
        const auto format_event_label =
            [&](const f4::campaign::api::CampaignEvent& ev, char* buf,
                std::size_t cap) -> bool {
            using K = f4::campaign::api::CampaignEvent::Kind;
            switch (ev.kind) {
                case K::MissionFiled: {
                    const std::string nm = objective_name(
                        ev.mission_filed.target_objective_id);
                    std::snprintf(buf, cap, "mission: %s team %u -> %s",
                                  ev.mission_filed.mission_name.c_str(),
                                  ev.mission_filed.team,
                                  nm.empty() ? "?" : nm.c_str());
                    return true;
                }
                case K::Kill:
                    std::snprintf(buf, cap,
                                  "air kill: team %u downed team %u (%s)",
                                  ev.kill.killer_team, ev.kill.victim_team,
                                  ev.kill.weapon.c_str());
                    return true;
                case K::ObjectiveDamage: {
                    const std::string nm =
                        objective_name(ev.objective_damage.objective_id);
                    std::snprintf(buf, cap, "objective damaged: %s (%u features)",
                                  nm.empty() ? "?" : nm.c_str(),
                                  ev.objective_damage.features_damaged);
                    return true;
                }
                case K::ObjectiveCaptured: {
                    const std::string nm = objective_name(
                        ev.objective_captured.objective_id);
                    std::snprintf(buf, cap, "CAPTURED: %s -> team %u",
                                  nm.empty() ? "?" : nm.c_str(),
                                  ev.objective_captured.new_owner);
                    return true;
                }
                case K::ObjectiveRepaired: {
                    const std::string nm = objective_name(
                        ev.objective_repaired.objective_id);
                    std::snprintf(buf, cap, "repaired %u features at %s",
                                  ev.objective_repaired.features_repaired,
                                  nm.empty() ? "?" : nm.c_str());
                    return true;
                }
                case K::ReinforcementDelivered:
                    std::snprintf(buf, cap,
                                  "reinforcements: %d aircraft (%d squadrons)",
                                  ev.reinforcement_delivered.aircraft,
                                  ev.reinforcement_delivered.squadrons_touched);
                    return true;
                case K::WeatherChanged:
                    std::snprintf(buf, cap, "weather: %s",
                                  ev.weather_changed.condition.c_str());
                    return true;
                case K::TaskingCycle:
                    std::snprintf(buf, cap, "tasking cycle: %d intents",
                                  ev.tasking_cycle.intents);
                    return true;
                case K::ActionFiled:
                    std::snprintf(buf, cap, "action filed: %s (team %u, %d%% damage)",
                                  ev.action_filed.mission_name.c_str(),
                                  ev.action_filed.team,
                                  ev.action_filed.damage_pct);
                    return true;
                case K::Verdict:
                    std::snprintf(buf, cap, "verdict: %s (leader team %d, swing %d)",
                                  ev.verdict.band.c_str(), ev.verdict.leader,
                                  ev.verdict.swing);
                    return true;
                default:
                    return false;  // pilot/roe/slot lines: shown when the
                                   // war books grow a face for them
            }
        };

        if (impl_->session_events.empty()) {
            ImGui::TextDisabled("(no events yet)");
        } else {
            constexpr std::size_t kMaxRows = 14;
            const std::size_t n = impl_->session_events.size();
            const std::size_t first = n > kMaxRows ? n - kMaxRows : 0;
            char line[192];
            for (std::size_t i = n; i-- > first;) {
                if (format_event_label(impl_->session_events[i], line,
                                       sizeof(line))) {
                    char tbuf[24];
                    format_abs_campaign_time(
                        impl_->session_epoch_s +
                            event_time(impl_->session_events[i]),
                        tbuf, sizeof(tbuf));
                    ImGui::Text("%s  %s", tbuf, line);
                }
            }
        }
    }

    ImGui::Separator();

    // --- Artifacts + stop ------------------------------------------------
    if (ImGui::Button("Write Result JSON")) {
        write_result_json();
    }
    ImGui::SameLine();
    if (ImGui::Button("Write Back")) {
        // CAMP-HOST-3: the contract's runtime-safe save — the ledger
        // write-back (pools, squadron counters, objective fstatus) plus
        // the WorldState JSON, written next to the world the session
        // loaded. The detail line carries the same counts the old
        // in-memory write-back reported.
        const auto res = impl_->session->save(
            impl_->last_world_json_path.string());
        impl_->status_msg = res.ok
            ? "write-back + world JSON: " + res.detail
            : "save failed: " + res.detail;
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
