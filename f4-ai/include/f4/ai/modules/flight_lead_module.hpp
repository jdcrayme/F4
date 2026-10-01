// f4-ai/include/f4/ai/modules/flight_lead_module.hpp
//
// FlightLeadModule — the lead half of flight command (AI_IMPLEMENTATION_PLAN.md
// §16 Step 14; FreeFalcon flitlead.cpp's CommandFlight(), the per-frame
// flight-level decision routine; CheckLead()).
//
// THE ROLE. Step 11 landed the WINGMAN half (WingmanModule — formation
// keeping + the engagement sort). The LEAD half is this module: it sits
// beside WingmanModule in the same DigitalBrain and is active only when
// the brain IS the lead (the host registers the flight roster on it).
// It never steers — the lead flies its own mission module like any
// single-ship. What it does is the flight-level COMMAND work: it watches
// the wingmen (via the per-tick status echoes the host pushes) and turns
// what it sees into edge-triggered orders on the closed WingRadio
// vocabulary, plus the order state the host reads back and applies to
// the wingman brains.
//
// THE PICTURE (engine-agnostic — the same contract as every module,
// mirrored): the module never touches the world, the bus, or another
// entity. The host:
//   * at spawn     — register_wingman(id) for each wingman of the flight
//                    (resolved from the scenario's lead_callsign refs);
//   * each tick    — report_wingman(echo) with the wingman's status (the
//                    host reads the wingman's brain: its WingmanModule
//                    state, its FuelState, its engagement, and whether
//                    its fusion holds the lead's target);
//   * after update — order_for(id) drains the module's pending order for
//                    that wingman and queues it on the wingman brain.
// The brain (which owns the bus) publishes the module's radio rows as
// WingRadioMessage — the transcript renders them; the AI never formats
// text.
//
// THE DECISIONS (the CommandFlight() tranche, smallest-first):
//
//   1. REJOIN — the wingman's own WingState echo reports Rejoining (the
//      Step-11 module's blowout rule, seen from the other side): the
//      lead orders the rejoin and the order latches until the wingman
//      reports Following again (one order per blowout, not one per
//      tick). The host applies it as WingmanModule::command_rejoin() —
//      the same SM transition the wingman's own blowout fires.
//
//   2. ENGAGE — the lead is fighting (its engagement id is live) and
//      the wingman's fusion holds that bandit but is not on it: the
//      lead orders "engage my target". The wingman's targeting ranks
//      the ordered id as a PREFERENCE among the tracks it already
//      holds — sensor truth still wins (an order can never conjure a
//      track). The order follows the lead's target: a new bandit
//      re-fires; the wingman joining the fight re-arms the latch.
//
//   3. RTB — the wingman's fuel echo reports Bingo (the brain's own
//      fuel policy — FrameExec step 2's reserve, seen from the other
//      side): the lead orders RTB once per flight, and — v1's "both
//      RTB" (the reference's wingman-count thresholds are a v2
//      refinement) — latches the LEAD's own stand-down too: lead_rtb()
//      goes up, the brain's engagement ladder takes the same RTB path
//      bingo does, and the flight goes home together.
//
// THE GATE lives in the host (the scenario ai block's "flight_lead"):
// an unarmed lead brain never registers a roster, never receives
// echoes, never publishes a row — leads behave exactly as today,
// byte-identically. The module itself is inert by construction: no
// roster = update() returns nothing.
//
// Dependencies: f4-ai (WingState, WingRadio). C++20.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "f4/ai/modules/wingman_module.hpp"
#include "f4/ai/wingradio.hpp"

namespace f4::ai::modules {

// ============================================================================
// FlightLeadModule
// ============================================================================
class FlightLeadModule {
public:
    /// Per-wingman status echo — the host pushes one per wingman per
    /// tick, read off the wingman's brain (the host is the lead's eyes
    /// on its own flight). All state is the wingman's OWN reporting:
    /// the formation state its WingmanModule holds, the fuel its brain
    /// gauges, the target its fusion fights, and whether its sensors
    /// hold the lead's bandit at all.
    struct WingmanEcho {
        std::uint64_t entity_id{0};
        bool alive{false};  ///< not killed (a corpse commands nothing)
        WingState wing_state{WingState::None};  ///< the Step-11 SM's state
        bool bingo{false};  ///< the wingman's FuelState == Bingo
        std::uint64_t engaged_id{0};  ///< the bandit it is fighting (0 = none)
        /// The wingman's fusion holds the lead's current target as a
        /// visible hostile (the engage rule's "sees it too" guard).
        bool sees_lead_target{false};
    };

    /// One radio row for the brain to publish (the module is pure — it
    /// returns the rows, the brain puts them on the bus).
    struct RadioRow {
        std::uint64_t peer_id{0};
        std::uint64_t target_id{0};
        WingRadio event{WingRadio::Ack};
    };

    /// The order the host applies to a wingman brain (drained once —
    /// the same one-delivery rule the radio rows follow).
    struct Order {
        WingRadio event{WingRadio::Ack};
        std::uint64_t target_id{0};
    };

    // --- Roster in (host pushes at spawn) --------------------------------
    /// Register a wingman of this flight. Duplicate ids collapse (a
    /// 4-ship's lead resolves three pairs onto the same roster slot).
    void register_wingman(std::uint64_t wingman_id);

    // --- Status in (host pushes each tick, before the brains run) -------
    /// Upsert the wingman's echo for this tick. Unreported wingmen keep
    /// their LAST echo (a dead/despawned wingman simply stops being
    /// pushed; the alive flag carried in the echo is what quiets it).
    void report_wingman(const WingmanEcho& echo);

    /// The lead's current engagement (the brain reads its own
    /// combat_engagement_id() before update; 0 = the lead is not
    /// fighting). Drives the engage rule.
    void set_lead_engagement(std::uint64_t target_id) noexcept {
        lead_engaged_id_ = target_id;
    }

    // --- Per-tick decisions ----------------------------------------------
    /// The CommandFlight() step: re-evaluate every wingman's echo against
    /// the edge rules, return the radio rows to publish (drained — the
    /// brain publishes exactly these, once), and latch the pending
    /// orders the host drains through order_for().
    [[nodiscard]] std::vector<RadioRow> update();

    // --- Order out (host reads after the brains run) ----------------------
    /// The pending order for this wingman, if one fired this tick.
    /// One delivery: a returned order is consumed (the host queues it
    /// on the wingman brain; the wingman acks on its next update).
    [[nodiscard]] std::optional<Order> order_for(std::uint64_t wingman_id);

    /// v1's both-RTB: latched when any wingman's bingo fired the RTB
    /// order. The lead brain reads this and takes its own RTB path
    /// (the engagement stand-down) — the flight goes home together.
    [[nodiscard]] bool lead_rtb() const noexcept { return lead_rtb_; }

    // --- State reporting (tests + host diagnostics) ------------------------
    [[nodiscard]] std::size_t roster_size() const noexcept {
        return slots_.size();
    }
    [[nodiscard]] std::uint64_t lead_engagement() const noexcept {
        return lead_engaged_id_;
    }

    /// Clear everything (host re-task / flight dissolved).
    void reset();

private:
    /// One wingman's bookkeeping slot.
    struct WingSlot {
        std::uint64_t entity_id{0};
        WingmanEcho echo{};
        // REJOIN latch: the order is out while the wingman reports
        // Rejoining; re-armed when it reports Following again.
        bool rejoin_latched_{false};
        // ENGAGE latch: the ordered bandit (0 = no engage order out).
        // Cleared when the wingman joins the fight on it or dies; a
        // lead target change re-fires against the new bandit.
        std::uint64_t ordered_target_{0};
        // RTB latch: one order per flight.
        bool rtb_fired_{false};
        // The pending order the host has not drained yet (one slot —
        // a newer order replaces an undelivered one; the radio rows
        // already went out either way).
        bool order_pending_{false};
        Order pending{};
    };

    WingSlot* find_slot(std::uint64_t wingman_id);
    const WingSlot* find_slot(std::uint64_t wingman_id) const;
    void emit(WingSlot& slot, WingRadio event, std::uint64_t target_id,
              std::vector<RadioRow>& rows);

    std::vector<WingSlot> slots_{};
    std::uint64_t lead_engaged_id_{0};
    bool lead_rtb_{false};
};

} // namespace f4::ai::modules
