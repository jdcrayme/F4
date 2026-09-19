// f4-campaign/include/f4/campaign/naval_writeback.hpp
//
// apply_naval_to(WorldState) — CAMP-DOM-6, the task-force movement's
// WorldState sync (the ground_writeback.hpp pattern, one layer over).
//
// OPT-IN header for the same reason ground_writeback.hpp is: it pulls
// the full WorldState layout, which only the loop-closing consumer
// needs.
//
// THE TIMING DIFFERENCE (the one thing the ground twin does not share):
// the naval engine has NO ledger books to sync (movement is not a war
// fact the books own — see naval_war.hpp) and its serving face is the
// WorldState itself (the `taskforces` query reads the WorldState, the
// wire-state rule). So the session calls THIS per update — after the
// engine's tick — and the save path needs nothing new: the rows are
// already current, the save's own call is the idempotent second touch.
//
// What it writes: per task force with activity (the engine's dirty
// flag — only forces that MOVED), the row's x / y (grid, moved),
// heading (the wire's byte, quantized from the walk), last_move
// (absolute campaign time). dest_x / dest_y are CONSUMED, never
// written (the wire's own order field — no naval orders cycle exists
// to reassign it). roster / supply are carried by the engine but
// never mutated by it — the sync does not touch them (a mirror that
// only moved must not normalize the row's other bytes).
//
// ACTIVITY-gated like every write-back: a task force that never moved
// is not written. A movement-quiet war round-trips the WorldState
// byte-identically on these fields — and a movement-OFF session
// (no engine at all) never touches any row (the golden identity).
//
// C++20.

#pragma once

#include <f4/campaign/naval_war.hpp>
#include <f4/world/detail/world_state.hpp>

#include <cstdint>
#include <vector>

namespace f4::campaign {

/// What apply_naval_to() did — the session's sync accounting, the
/// tests assert it.
struct NavalWritebackResult {
    /// Task force rows written (the engine dirtied them).
    int task_forces_written = 0;
    /// Engine VUs with activity but no matching WorldState unit (the
    /// loud rule — cannot happen with the ws_index addressing, which
    /// is the point of carrying it; the vector stays for the result
    /// shape's symmetry with the ground twin).
    std::vector<std::uint32_t> unmatched_task_forces;
};

/// Sync the naval engine's moved task forces into the WorldState (see
/// header comment). Only MOVED forces are written: a pristine war
/// leaves the world byte-identical on these fields (the zero-event
/// round-trip contract). Takes the engine NON-const — the per-unit
/// dirty flags clear on write (idempotent: the save path's second
/// touch writes nothing).
[[nodiscard]] NavalWritebackResult
apply_naval_to(NavalWar& war, f4::world::WorldState& ws);

} // namespace f4::campaign
