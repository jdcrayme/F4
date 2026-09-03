// f4-campaign/src/world_writeback.cpp
//
// apply_to(WorldState) implementation — see world_writeback.hpp.

#include <f4/campaign/world_writeback.hpp>

#include <algorithm>

namespace f4::campaign {

WorldWritebackResult apply_to(const CampaignResultLedger& ledger,
                              f4::world::WorldState& ws) {
    WorldWritebackResult out;

    // --- Team pools ------------------------------------------------------
    // Only teams with losses (activity). A pristine ledger must leave the
    // world byte-identical on these fields — the zero-event round-trip
    // contract.
    for (const auto& team : ledger.teams()) {
        if (team.losses <= 0) continue;
        // te_number_aircraft is slot-indexed; find the slot's index.
        // The vector may be shorter than 8 or slot-ordered differently
        // (defensive: match by index == slot, the wire's own layout).
        const auto slot = team.slot;
        if (slot < 0 ||
            static_cast<std::size_t>(slot) >=
                ws.campaign.te_number_aircraft.size()) {
            continue;
        }
        ws.campaign.te_number_aircraft[static_cast<std::size_t>(slot)] =
            team.aircraft_remaining;
        ++out.team_pools_written;
    }

    // --- Squadron counters -------------------------------------------------
    // Walk the WORLD's squadrons (wire order) and match against the
    // ledger's entries. Counters are absolutes — the ledger was seeded
    // from this very world, so a write reproduces seed + run deltas.
    // ACTIVITY = non-zero run delta: a mid-campaign save seeds non-zero
    // absolutes (TestCamp's squadrons carry kill history), and a
    // zero-event ledger must leave those untouched.
    for (auto& unit : ws.units) {
        if (unit.unit_class != f4::entities::UnitClass::Squadron) continue;
        const auto* entry = ledger.squadron(unit.id_num);
        if (entry == nullptr) continue;
        const bool active = entry->run_aa_kills != 0 ||
                            entry->run_ag_kills != 0 ||
                            entry->run_losses != 0;
        if (!active) continue;
        unit.aa_kills = entry->aa_kills;             // int16 -> int16
        unit.ag_kills = entry->ag_kills;             // int16 -> int16
        unit.total_losses = static_cast<std::uint8_t>(
            std::min<std::uint16_t>(entry->total_losses, 255));
        ++out.squadrons_written;
    }

    // Unmatched ledger squadrons (run activity but no world unit): loud.
    for (const auto& entry : ledger.squadrons()) {
        const bool active = entry.run_aa_kills != 0 ||
                            entry.run_ag_kills != 0 ||
                            entry.run_losses != 0;
        if (!active) continue;
        const auto it = std::find_if(
            ws.units.begin(), ws.units.end(),
            [&entry](const f4::world::UnitState& u) {
                return u.unit_class == f4::entities::UnitClass::Squadron &&
                       u.id_num == entry.vu;
            });
        if (it == ws.units.end()) {
            out.unmatched_squadrons.push_back(entry.vu);
        }
    }

    // --- Objective damage bitmaps ------------------------------------------
    // VU match; the ledger's fstatus IS the save-format face (2 bits per
    // feature — the .obd delta's own encoding). Only damaged objectives
    // are in the ledger, so pristine worlds stay untouched.
    for (const auto& rec : ledger.objective_damage()) {
        bool matched = false;
        for (auto& obj : ws.objectives) {
            if (obj.id_num != rec.objective) continue;
            obj.fstatus = rec.fstatus;
            matched = true;
            break;
        }
        if (matched) {
            ++out.objectives_written;
        } else {
            out.unmatched_objectives.push_back(rec.objective);
        }
    }

    return out;
}

} // namespace f4::campaign
