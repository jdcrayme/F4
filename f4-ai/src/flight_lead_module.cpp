// f4-ai/src/flight_lead_module.cpp
//
// FlightLeadModule implementation — see the header for the design notes
// (the CommandFlight() tranche: Rejoin / EngageMyTarget / RTB, one
// edge-triggered order each, on the closed WingRadio vocabulary).

#include "f4/ai/modules/flight_lead_module.hpp"

namespace f4::ai::modules {

// ============================================================================
// Roster + status in
// ============================================================================

void FlightLeadModule::register_wingman(std::uint64_t wingman_id) {
    if (wingman_id == 0) return;  // no such entity — nothing to register
    if (find_slot(wingman_id) != nullptr) return;  // duplicate registration
    WingSlot slot;
    slot.entity_id = wingman_id;
    slots_.push_back(std::move(slot));
}

void FlightLeadModule::report_wingman(const WingmanEcho& echo) {
    WingSlot* slot = find_slot(echo.entity_id);
    if (slot == nullptr) return;  // not on the roster — not this flight's
    slot->echo = echo;
}

// ============================================================================
// Per-tick decisions — the CommandFlight() tranche
// ============================================================================

std::vector<FlightLeadModule::RadioRow> FlightLeadModule::update() {
    std::vector<RadioRow> rows;
    if (slots_.empty()) return rows;  // no roster — the module is inert

    rows.reserve(slots_.size());
    for (WingSlot& slot : slots_) {
        const WingmanEcho& e = slot.echo;

        // A corpse is quiet: no orders, every latch cleared (if the
        // wingman slot is re-used by a later registration — or the host
        // re-reports the entity — the rules start fresh).
        if (!e.alive) {
            slot.rejoin_latched_ = false;
            slot.ordered_target_ = 0;
            continue;
        }

        // -- 1. REJOIN ---------------------------------------------------
        // The wingman's own blowout rule (the Step-11 module's rejoin
        // ring), seen from the other side: the lead makes it a command
        // and holds the order until the wingman reports back on
        // station. One order per blowout.
        if (e.wing_state == WingState::Rejoining && !slot.rejoin_latched_) {
            slot.rejoin_latched_ = true;
            emit(slot, WingRadio::OrderRejoin, 0, rows);
        } else if (e.wing_state == WingState::Following &&
                   slot.rejoin_latched_) {
            slot.rejoin_latched_ = false;  // back in station — re-arm
        }

        // -- 2. ENGAGE ---------------------------------------------------
        // The lead is fighting a bandit the wingman can see and is not
        // already on: "engage my target". The order follows the lead's
        // target (a new bandit re-fires) and re-arms when the wingman
        // joins the fight on the ordered id. The wingman's targeting
        // treats the ordered id as a preference among ITS OWN tracks —
        // sensor truth still wins.
        if (lead_engaged_id_ != 0 && e.sees_lead_target &&
            e.engaged_id != lead_engaged_id_ &&
            slot.ordered_target_ != lead_engaged_id_) {
            slot.ordered_target_ = lead_engaged_id_;
            emit(slot, WingRadio::OrderEngageMyTarget, lead_engaged_id_,
                 rows);
        } else if (slot.ordered_target_ != 0 &&
                   e.engaged_id == slot.ordered_target_) {
            slot.ordered_target_ = 0;  // wingman is on it — re-arm
        }

        // -- 3. RTB ------------------------------------------------------
        // The wingman's own fuel reserve (FrameExec step 2's bingo),
        // seen from the other side. One order per flight; v1 takes the
        // LEAD home with it (lead_rtb_ — both RTB; the reference's
        // wingman-count thresholds are a v2 refinement).
        if (e.bingo && !slot.rtb_fired_) {
            slot.rtb_fired_ = true;
            emit(slot, WingRadio::OrderRTB, 0, rows);
            lead_rtb_ = true;
        }
    }
    return rows;
}

std::optional<FlightLeadModule::Order>
FlightLeadModule::order_for(std::uint64_t wingman_id) {
    WingSlot* slot = find_slot(wingman_id);
    if (slot == nullptr || !slot->order_pending_) return std::nullopt;
    slot->order_pending_ = false;  // one delivery
    return slot->pending;
}

void FlightLeadModule::reset() {
    slots_.clear();
    lead_engaged_id_ = 0;
    lead_rtb_ = false;
}

// ============================================================================
// Internals
// ============================================================================

FlightLeadModule::WingSlot*
FlightLeadModule::find_slot(std::uint64_t wingman_id) {
    for (WingSlot& slot : slots_) {
        if (slot.entity_id == wingman_id) return &slot;
    }
    return nullptr;
}

const FlightLeadModule::WingSlot*
FlightLeadModule::find_slot(std::uint64_t wingman_id) const {
    for (const WingSlot& slot : slots_) {
        if (slot.entity_id == wingman_id) return &slot;
    }
    return nullptr;
}

void FlightLeadModule::emit(WingSlot& slot, WingRadio event,
                            std::uint64_t target_id,
                            std::vector<RadioRow>& rows) {
    rows.push_back(RadioRow{slot.entity_id, target_id, event});
    slot.order_pending_ = true;
    slot.pending = Order{event, target_id};
}

} // namespace f4::ai::modules
