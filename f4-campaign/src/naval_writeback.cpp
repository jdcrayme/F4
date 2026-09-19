// f4-campaign/src/naval_writeback.cpp
//
// apply_naval_to(WorldState) implementation — see naval_writeback.hpp.

#include <f4/campaign/naval_writeback.hpp>

#include <algorithm>
#include <cstdint>

namespace f4::campaign {

NavalWritebackResult apply_naval_to(NavalWar& war,
                                    f4::world::WorldState& ws) {
    NavalWritebackResult out;

    for (const auto& tf : war.units()) {
        if (!tf.dirty) continue;

        // The ws_index address (the engine carried it from the
        // snapshot walk — the adapter serves exactly the WorldState's
        // wire order). Verified against the row's identity before the
        // write: a mismatch is loud (the result's unmatched list),
        // never a silent wrong-row write.
        if (tf.ws_index >= ws.units.size()) {
            out.unmatched_task_forces.push_back(tf.vu);
            continue;
        }
        auto& unit = ws.units[tf.ws_index];
        if (unit.id_num != tf.vu ||
            unit.unit_class != f4::entities::UnitClass::TaskForce) {
            out.unmatched_task_forces.push_back(tf.vu);
            continue;
        }

        unit.x = static_cast<std::int16_t>(tf.x);
        unit.y = static_cast<std::int16_t>(tf.y);
        unit.heading = tf.heading;
        unit.last_move = static_cast<std::int32_t>(
            std::min<std::int64_t>(tf.last_move, 2147483647));
        ++out.task_forces_written;
        // dest_x / dest_y are consumed, never written; roster /
        // supply are carried by the engine, never mutated by it.
    }

    // The moved flags clear as ONE step after the walk (the sync's
    // own accounting — idempotent: the save path's second touch
    // writes nothing).
    war.clear_dirty();

    return out;
}

} // namespace f4::campaign
