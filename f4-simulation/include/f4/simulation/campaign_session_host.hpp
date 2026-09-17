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

#include <memory>
#include <string>

namespace f4::simulation {

class EngineSessionHost final
    : public f4::campaign::api::ICampaignSession {
public:
    /// Build the engine session (CampaignSession::create's contract:
    /// nullptr + `error` on any failure; throws nothing).
    [[nodiscard]] static std::unique_ptr<EngineSessionHost>
    create(const CampaignSessionOptions& opts, std::string* error = nullptr);

    ~EngineSessionHost() override = default;

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

    // --- direct engine access (tests; the viewer's later HOST-3 move) --

    [[nodiscard]] CampaignSession& engine() noexcept { return *session_; }
    [[nodiscard]] const CampaignSessionOptions& options() const noexcept {
        return opts_;
    }

private:
    EngineSessionHost() = default;

    std::unique_ptr<CampaignSession> session_;
    CampaignSessionOptions opts_{};
    /// The host's pacing presentation (echoed by the `time` query; never
    /// touches the fixed dt — the plan §2.2 rule).
    double time_scale_{1.0};
};

} // namespace f4::simulation
