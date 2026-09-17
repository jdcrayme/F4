// f4-simulation/include/f4/simulation/campaign_session_host.hpp
//
// CAMP-HOST-1 — the engine-side adapter of the host contract
// (Docs/CAMP_HOST_PLAN.md §6): f4::campaign::api::ICampaignSession
// implemented over f4::simulation::CampaignSession. NO new campaign
// logic, NO new sim logic — the same translation discipline the
// contract demands: engine state in, pure data out, refusals as data.
//
//     ICampaignSession (f4-campaign-api, pure contract)
//            ▲
//            │ implements
//     EngineSessionHost            ← THIS class (f4-simulation)
//            │ owns
//     CampaignSession              ← the V-CAMP engine object
//
// Commands in v1:
//   - focus / clear_focus / select_deagg / select_reagg — the FID
//     machinery wearing its contract hat (plan §4): set_view_bubble,
//     force_(de|re)aggregate_flight. Applied immediately (presentation-
//     adjacent doctrine, not war state); the ack says so.
//   - roe_set / flight_retask / flight_abort / objective_priority —
//     REFUSED with Refusal::NotImplemented and the tranche ID in the
//     detail (CAMP-CMD-1/2 land behind the same wire; no protocol bump).
//
// step() semantics (plan §5): advance(ticks * sim_dt, override = ticks)
// — the engine's own accumulator drains whole ticks and carries any
// sub-tick rounding residue forward, the override never lets a request
// overshoot, and dilation (the cap hit) comes back as data. A paused
// session no-ops the drain exactly like the viewer's pause.

#pragma once

#include <f4/campaign/api/session.hpp>

#include <f4/simulation/campaign_session.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace f4::simulation {

class EngineSessionHost final
    : public f4::campaign::api::ICampaignSession {
public:
    /// Build the engine session (CampaignSession::create's contract:
    /// nullptr + `error` on any failure; throws nothing).
    [[nodiscard]] static std::unique_ptr<EngineSessionHost>
    create(const CampaignSessionOptions& opts, std::string* error = nullptr);

    ~EngineSessionHost() override;

    EngineSessionHost(const EngineSessionHost&) = delete;
    EngineSessionHost& operator=(const EngineSessionHost&) = delete;

    // --- ICampaignSession (f4::campaign::api) ---------------------------

    [[nodiscard]] f4::campaign::api::IdentityFingerprint identity()
        const override;
    f4::campaign::api::StepResult step(std::uint32_t ticks) override;
    void set_time_scale(double scale) override;
    void set_paused(bool on) override;
    f4::campaign::api::SaveResult save(std::string_view path) override;
    [[nodiscard]] f4::campaign::api::QueryResult query(
        const f4::campaign::api::QuerySpec& spec) override;
    f4::campaign::api::CommandAck submit(
        const f4::campaign::api::CommandIntent& intent) override;
    void set_event_filter(
        const f4::campaign::api::EventFilter& filter) override;
    [[nodiscard]] std::vector<f4::campaign::api::CampaignEvent>
    drain_events() override;

    // --- HOST-2: extra engine-rate sinks (the journal; UNFILTERED —
    // the journal is the complete record, the wire is the filtered one)

    /// Install a sink called for every published event, in engine
    /// occurrence order. Returns the handle remove_event_sink() takes.
    [[nodiscard]] std::size_t add_event_sink(
        std::function<void(const f4::campaign::api::CampaignEvent&)> sink);
    void remove_event_sink(std::size_t handle);

    // --- direct engine access (CAMP-HOST-3's two planes) ---------------
    //
    // The contract plane (ICampaignSession, above) is the ONLY surface a
    // client needs: campaignd uses exactly it. The reference renderer
    // (f4-world-viewer) additionally draws the LIVE entity graph — the
    // per-vehicle transforms, models, and selection rings the FID focus
    // bubble materializes — which is a RENDER-plane concern (plan §2.2:
    // pacing/animation are host business), not campaign state. engine()
    // exists for the contract tests and for that renderer's quarantined
    // render-plane helpers; everything else a host does goes through the
    // four surfaces.
    [[nodiscard]] CampaignSession& engine() noexcept { return *session_; }
    [[nodiscard]] const CampaignSessionOptions& options() const noexcept {
        return opts_;
    }

private:
    EngineSessionHost() = default;

    /// Install the wire buffer's bus subscription (once, on first arm).
    void arm_buffer_();

    std::unique_ptr<CampaignSession> session_;
    CampaignSessionOptions opts_{};
    /// The host's pacing presentation (echoed by the `time` query; never
    /// touches the fixed dt — the plan §2.2 rule).
    double time_scale_{1.0};

    // --- HOST-2 event plumbing ------------------------------------------
    struct EventSink {
        std::size_t handle{0};
        std::size_t subscription{0};
        std::function<void(const f4::campaign::api::CampaignEvent&)> fn;
    };
    std::vector<EventSink> sinks_;
    std::size_t next_sink_handle_{1};
    /// The wire buffer (armed by set_event_filter; push-time filtered).
    bool buffer_armed_{false};
    std::size_t buffer_subscription_{static_cast<std::size_t>(-1)};
    f4::campaign::api::EventFilter filter_{};
    std::vector<f4::campaign::api::CampaignEvent> buffer_;
};

} // namespace f4::simulation
