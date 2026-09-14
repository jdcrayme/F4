// f4-campaign/src/flight_writeback.cpp
//
// flight_writeback — see flight_writeback.hpp for the contract. The
// world scan is a single pass over ws.units (the engine's dirty set is
// what costs; the world side is a vu → index lookup in wire order).

#include <f4/campaign/flight_writeback.hpp>

#include <algorithm>
#include <cmath>

namespace f4::campaign {

FlightWritebackResult apply_flights_to(const FlightAggregateEngine& engine,
                                       f4::world::WorldState& ws) {
    FlightWritebackResult result;

    // The engine's own vu → dirty index map (only dirty flights cost a
    // world lookup — the activity rule the ground write-back uses).
    for (const auto& f : engine.flights()) {
        if (!f.dirty) continue;
        auto* unit = static_cast<f4::world::UnitState*>(nullptr);
        for (auto& u : ws.units) {
            if (u.id_num == f.vu &&
                u.unit_class == f4::entities::UnitClass::Flight) {
                unit = &u;
                break;
            }
        }
        if (unit == nullptr) continue;

        ++result.flights_synced;

        // Position: sub-grid doubles → the wire's int16 grid (rounded,
        // clamped). Only writes on change (the mirror's own rule).
        const auto gx = static_cast<std::int16_t>(std::clamp(
            std::llround(f.fx), static_cast<long long>(-32768),
            static_cast<long long>(32767)));
        const auto gy = static_cast<std::int16_t>(std::clamp(
            std::llround(f.fy), static_cast<long long>(-32768),
            static_cast<long long>(32767)));
        if (unit->x != gx || unit->y != gy) {
            unit->x = gx;
            unit->y = gy;
            ++result.positions_moved;
        }

        if (unit->flight_altitude != f.altitude_ft) {
            unit->flight_altitude = f.altitude_ft;
        }

        // Fuel: monotone (the aggregate's burn never decreases — the
        // same rule the engine's fold-back enforces).
        if (unit->fuel_burnt != f.fuel_burnt) {
            unit->fuel_burnt = std::max(unit->fuel_burnt, f.fuel_burnt);
            ++result.fuels_updated;
        }
    }

    return result;
}

} // namespace f4::campaign
