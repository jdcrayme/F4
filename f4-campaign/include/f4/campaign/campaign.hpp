// f4-campaign/include/f4/campaign/campaign.hpp
//
// Campaign — the headless dynamic-campaign engine (Architecture Proposal
// M4.7, first slice per NEXT_PHASE_PLAN.md §B.2).
//
// FreeFalcon's campaign is a master CampaignClass tracking campaign time
// and firing each domain's tasking cycle when its timestamp comes due;
// the Air Tasking Manager then decides which missions should exist and
// builds packages for them. F4's Campaign reproduces that shape on the
// engine-agnostic side:
//
//   * It consumes the f4-world IDataSource interfaces (ICampaignSource,
//     ITeamSource, IUnitCoreSource) — the same boundary discipline
//     everything else uses. It NEVER sees f4-world-convert, EntityWorld
//     components, or a flight model (the NullFlightModel discipline:
//     the campaign tick moves nothing; units stay put this slice).
//   * `tick(delta)` advances the clock and fires the air tasking cycle
//     whenever it comes due. Per belligerent team, the cycle walks the
//     mission-profile table in wire-byte order and generates a mission
//     for every profile whose cadence and availability gates pass:
//
//       - availability (role): the profile's `aro` must match one of the
//         team's squadrons (specialty byte bound through aro_name()).
//       - availability (capability): a profile with caps this slice
//         cannot verify (VEH_STEALTH, VTOL, ...) does not generate —
//         same effect as FreeFalcon's caps check, conservative default.
//       - availability (aircraft): the team's aircraft pool (squadron
//         roster when nonzero, else the campaign source's per-team
//         te_number_aircraft) must cover the profile's `str` (default
//         aircraft count); each generated mission deducts from the
//         pool. A profile whose count can't be met at all (0 aircraft)
//         generates nothing.
//       - target: AMIS_TAR_NONE-carrying profiles (alert, training)
//         generate only when the profile carries no external-target
//         requirement — in this slice all profiles generate against
//         abstract targets; route/target resolution arrives with the
//         M4.3–M4.5 tranche.
//     Each generated mission publishes one MissionIntent on the message
//     bus — the only coupling to the outside world (B.3's sim-side
//     spawner subscribes to exactly this message).
//
//   * Time on target: the M4.7-skeleton rule is the midpoint of the
//     profile's planning-advance window (min_time + max_time)/2 minutes
//     past the cycle time — deterministic, profile-driven, and replaced
//     by real slot scheduling when the ATM tranche lands.
//
//   * Everything is a pure function of (source interfaces, profile
//     table, config, tick history): NO RNG anywhere in the campaign
//     layer (determinism stays absolute — see NEXT_PHASE_PLAN.md §3).
//     The same campaign state ticked the same way produces byte-stable
//     summaries — asserted by the golden test.
//
// Dependencies: f4-world (IDataSource), f4-messaging (bus), f4-json
// (summary writer). C++20.

#pragma once

#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/mission_type.hpp>
#include <f4/campaign/result_ledger.hpp>

#include <f4/messaging/bus.hpp>
#include <f4/world/data_source.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace f4::campaign {

/// Campaign clock in seconds, relative to campaign start (0 = start).
/// FreeFalcon's CampaignTime is likewise a second count; the world's
/// absolute epoch (campaign.current_time) is intentionally NOT folded in —
/// the tick advances a relative clock and the sources carry the epoch.
using CampaignTime = std::int64_t;

/// One generated mission — the campaign's contract with the outside
/// world. Published on the MessageBus as f4::campaign::MissionIntent;
/// B.3's sim-side spawner subscribes and materializes flights through
/// the Milestone-A campaign spawn path.
struct MissionIntent {
    /// Campaign time the intent was issued (seconds, relative).
    CampaignTime issued_time{0};
    /// Campaign time the mission launches (seconds, relative).
    CampaignTime time_on_target{0};
    /// Owning team slot (from the unit data's owner vocabulary).
    std::uint8_t team{0};
    /// Owning team name (empty when the slot is unnamed).
    std::string team_name;
    /// Mission wire byte + canonical name (profile-bound).
    std::uint8_t mission_byte{0};
    std::string mission_name;
    /// Package composition: aircraft count and the source squadron.
    int aircraft_count{0};
    std::uint32_t squadron_id{0};    ///< home squadron VU_ID.num
    std::string squadron_name;       ///< display name from the unit data
    /// Synthetic package identity (deterministic counter).
    std::uint32_t package_id{0};
    /// One flight per package in this slice.
    std::uint32_t flight_id{0};

    /// Element-wise equality (tests assert bus content == recorded intents).
    bool operator==(const MissionIntent&) const = default;
};

/// Tunables. Defaults documented in the header doc; every field is
/// pinned by the golden test through the generated summary.
struct CampaignConfig {
    /// Air tasking cycle period (FreeFalcon's ATM::Task cadence; the
    /// plan's 30-minute validation run is one cycle at the default).
    CampaignTime air_task_cycle_sec{1800};
    /// First synthetic package id (deterministic counter start).
    std::uint32_t first_package_id{1};
    /// C2: the reinforcement cadence — FreeFalcon's gate is
    /// `CurrentTime > LastReinforcement + Rate`, and the rate is a
    /// RUNTIME DIFFICULTY SETTING in the reference (campaign ratios,
    /// not wire data), so it lives here as a tunable. The anchor IS
    /// wire data (the .cmp header's last_reinforcement, exposed via
    /// ICampaignSource). The default is DISABLED (0): a fresh ledger
    /// attached to a default-configured Campaign must change nothing
    /// (the C1 golden identity — a stale .cmp anchor would otherwise
    /// fire the cadence the moment a ledger lands). Hosts opt in
    /// (campaign_qc's --tasking arms 12 h unless overridden); 0 stays
    /// the pre-C2 depletion-only behavior.
    CampaignTime reinforcement_period_sec{0};
};

class Campaign {
public:
    /// Bind the campaign to its world sources, the profile table, and
    /// the message bus. All references must outlive the Campaign.
    /// \param camp    campaign-level state (per-team aircraft pools)
    /// \param teams   team slots + stance matrix (belligerence source)
    /// \param units   unit roster (squadrons: owner, specialty, aircraft)
    /// \param profiles the validated mission profile table (B.1)
    /// \param bus     intents publish here
    /// \param cfg     tunables (see CampaignConfig)
    Campaign(const f4::world::ICampaignSource& camp,
             const f4::world::ITeamSource& teams,
             const f4::world::IUnitCoreSource& units,
             const MissionProfileTable& profiles,
             f4::messaging::MessageBus& bus,
             const CampaignConfig& cfg = {});

    /// Advance the clock by `delta_sec` and fire every tasking cycle that
    /// has come due (a delta spanning several cycles fires them all, in
    /// order). Pure with respect to the sources: calling tick with the
    /// same history always yields the same intents.
    void tick(CampaignTime delta_sec);

    /// Campaign-relative clock (seconds).
    [[nodiscard]] CampaignTime clock() const noexcept { return clock_; }

    /// Tasking cycles fired so far.
    [[nodiscard]] int cycles_fired() const noexcept { return cycles_fired_; }

    /// Reinforcement ticks fired so far (C2 — the cadence fires when
    /// campaign time passes the .cmp anchor + period; see
    /// CampaignConfig::reinforcement_period_sec). Requires a ledger
    /// (the refill lands in the write model); the legacy no-ledger
    /// mode never fires.
    [[nodiscard]] int reinforcement_fires() const noexcept {
        return reinforcement_fires_;
    }

    /// Every intent published since construction, in publish order.
    [[nodiscard]] const std::vector<MissionIntent>& intents() const noexcept {
        return intents_;
    }

    /// Attach the C1 result ledger — the war-loop feedback. While
    /// attached, the LEDGER IS THE TASKING POOL (C2): the cycle's
    /// availability gates read the ledger's one-pool numbers
    /// (squadron_tasking_available: snapshot − draws − non-drawn
    /// losses + reinforcements) and every generated mission debits it
    /// (apply_mission_draw) — cycles and combat deplete ONE pool, and
    /// the reinforcement cadence refills it in place. The Campaign's
    /// own pool_/available counters are UNTOUCHED in this mode (the
    /// no-ledger path keeps them — B.3's behavior, byte-identical
    /// goldens).
    ///
    /// The ledger is BORROWED and MUTABLE (the set_brain_archetype /
    /// set_weapon_table pattern; C1 read it, C2 also writes draws and
    /// fires reinforcement into it): null detaches, and a ledger
    /// whose slots carry no events reports the same numbers the
    /// Campaign's own pool would — attaching a fresh ledger changes
    /// nothing until draws or losses land in it (the golden identity).
    void set_result_ledger(CampaignResultLedger* ledger) noexcept {
        result_ledger_ = ledger;
    }

    /// Slots of the teams currently at war (lowest slot first) — the
    /// stance-matrix belligerence rule (FreeFalcon ID_HOSTILE sign test
    /// over NAMED slots), shared with the sim-side team resolution.
    [[nodiscard]] std::vector<int> belligerent_teams() const;

    /// Deterministic summary of the run so far — the recorder's
    /// to_summary_json pattern. Byte-stable across identical runs; the
    /// golden test compares two executions byte-for-byte.
    [[nodiscard]] std::string to_summary_json() const;

private:
    /// One tasking cycle: per belligerent team, evaluate profiles and
    /// publish intents. Returns the intents generated THIS cycle.
    void run_tasking_cycle_();

    /// C2: the reinforcement cadence — fire while now (epoch + clock)
    /// has passed last_reinforce_ + period. Each fire delivers into
    /// the ledger (deficits refilled from the wire's per-squadron
    /// budgets) and advances the anchor to now (catch-up-once — a
    /// stale .cmp timer fires ONE tick, not the months it is behind).
    /// No-op without a ledger (the refill is write-model state) or
    /// with reinforcement disabled.
    void fire_reinforcements_();

    /// Aircraft pool snapshot for one team (roster-backed when nonzero,
    /// else the campaign source's per-team count).
    [[nodiscard]] int team_aircraft_pool_(int team_slot) const;

    const f4::world::ICampaignSource& camp_;
    const f4::world::ITeamSource& teams_;
    const f4::world::IUnitCoreSource& units_;
    const MissionProfileTable& profiles_;
    f4::messaging::MessageBus& bus_;
    CampaignConfig cfg_;

    CampaignTime clock_{0};
    int cycles_fired_{0};
    CampaignTime next_cycle_{0};   ///< due time of the next tasking cycle

    /// Live aircraft pool per team slot, deducted by each generated
    /// mission (the attrition ledger B.3 builds on). Index = slot.
    /// NOT touched while a result ledger is attached (the ledger owns
    /// the tasking pool then — one pool, not two).
    std::vector<int> pool_;

    /// C2 reinforcement cadence state (absolute campaign times — the
    /// save's own epoch, bridged through ICampaignSource):
    ///   epoch_            — current_time at construction (the anchor's
    ///                       time base; the relative clock + epoch = now)
    ///   last_reinforce_  — the anchor from the .cmp header
    ///                       (last_reinforcement), advanced to "now" on
    ///                       each fire (FreeFalcon's catch-up-once shape
    ///                       — a stale anchor fires once, not N times).
    CampaignTime epoch_{0};
    CampaignTime last_reinforce_{0};
    int reinforcement_fires_{0};

    /// C1/C2 result ledger (optional, non-owning, MUTABLE — tasking
    /// writes draws into it) — when set, it IS the tasking pool. See
    /// set_result_ledger().
    CampaignResultLedger* result_ledger_ = nullptr;

    /// Aircraft available per squadron (parallel to squadrons_), updated
    /// as missions draw aircraft down.
    struct SquadronRef {
        std::uint32_t id_num;
        std::uint8_t owner;
        std::uint8_t specialty;
        std::string name;
        int available;             ///< aircraft still available this run
    };
    std::vector<SquadronRef> squadrons_;

    /// All intents in publish order (mirrors what the bus saw).
    std::vector<MissionIntent> intents_;
};

// ============================================================================
// emit_flight_intents — MissionIntents from LIVE saved flights (B.3 tranche)
// ============================================================================
//
// The M4.7 Campaign::tick generates intents synthetically (profile ladder
// over squadron availability). Real campaign saves — TestCamp.cam et al. —
// already carry the output of FreeFalcon's ATM: 449 tasked Flight units
// with mission bytes, TOTs, targets, packages, squadrons and callsigns.
// This function turns those live flights into the SAME MissionIntent
// contract, so B.3's sim-side spawner consumes one message shape whether
// the tasking source is the synthetic ladder or a decoded save.
//
// Semantics (differences from the synthetic path, all deliberate):
//   * issued_time  — the `now` argument (callers pass the save's
//                    campaign.current_time; the flights predate it).
//   * time_on_target — the flight's ABSOLUTE CampaignTime as stored in the
//                    save (not rebased to campaign-relative). The spawner
//                    anchors campaign time itself; keeping the save's epoch
//                    intact preserves round-trip fidelity.
//   * flight_id / package_id — the flight's and its package's VU_ID.nums
//                    (the synthetic path's counter ids never appear).
//   * aircraft_count — the flight's roster, decoded as the same 2-bit
//                    group packing used for battalions (sum of groups;
//                    0xA0 = 4 aircraft). 0 when the save carries none.
//   * squadron_name / team_name — resolved from the unit + team data when
//                    the respective source pointers are non-null.
//
// Determinism: pure function of the sources — no RNG, no state.
[[nodiscard]] std::vector<MissionIntent>
emit_flight_intents(const f4::world::IUnitCoreSource& units,
                    const f4::world::IFlightSource& flights,
                    f4::messaging::MessageBus& bus,
                    CampaignTime now,
                    const f4::world::ITeamSource* teams = nullptr);

} // namespace f4::campaign
