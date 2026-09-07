// f4-world-convert/include/f4/world_convert/save_writeback.hpp
//
// The runtime-mutated save path (SAVE_WRITE_PLAN §6.1 + §2e's .uni/.obj
// follow-on): turn "the campaign loop mutated a WorldState" into a .cam
// that loads — in FreeFalcon or F4 — to the mutated state.
//
// The flow this composes:
//
//   1. cam2json --preserve-subfiles save1.cam  → original.world.json
//      (the full-fidelity doc: world blocks + subfiles_b64)
//   2. f4-world loads it, the campaign loop runs, apply_to(ledger, ws)
//      and apply_ground_to(engine, ws) mutate the WorldState
//   3. ws.to_json_string()                     → mutated.world.json
//      (the §6.1 emitter's PROJECTION — world-schema fields only)
//   4. derive_save_mutations(original, mutated)
//      diffs the two documents over the fields the campaign loop OWNS
//      (the campaign clock/timers/pools; objective fstatus + owner;
//      squadron counters + battalion movement/state) and returns the
//      changed records
//   5. build_campaign_with_mutations(original, mutations)
//      re-encodes .cmp from the ORIGINAL's full-fidelity campaign block
//      with the derived mutations applied, DECODES the original .obj/.uni
//      from subfiles_b64, overwrites the mutated fields on the decode
//      structs, re-encodes them, and assembles the .cam — every other
//      sub-file passes through verbatim.
//
// Why diff-then-overwrite (rather than re-encoding from the mutated
// projection): the world JSON schema is a LOSSY projection of the decode
// structs (spot_time, spotted, base_flags, the .tea team-status block,
// squadron stores[] and their kin are not part of the schema). Re-encoding
// from the projection alone would ZERO those fields. Starting from the
// original decode and overwriting only the owned fields is struct-faithful:
// untouched records and fields come out byte-identical to the original
// decode, which is the same bar the §2b-§2e encoders set.
//
// C++20.

#pragma once

#include <f4/world_convert/campaign_saver.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace f4::world_convert {

/// One objective's save-side mutation. C1 writes the synced damage bitmap
/// (fstatus, by VU_ID match); G1 flips the owner on capture. An empty
/// fstatus / nullopt owner = keep the original.
struct ObjectiveSaveMutation {
    uint32_t id_num = 0;
    std::vector<uint8_t> fstatus;
    std::optional<uint8_t> owner;
};

/// One unit's save-side mutation. The surface is exactly the campaign
/// loop's write-back: C1's squadron counters (aa/ag kills, total losses —
/// written as absolutes) and G1's battalion movement/state (grid x/y,
/// roster, losses, supply/morale/fatigue, heading, timers, destination).
/// as/an kills and missions_flown ride along (the C1 write-back doesn't
/// touch them today, but a future tranche that does should not need this
/// header to grow). nullopt = keep the original value.
struct UnitSaveMutation {
    uint32_t id_num = 0;

    // --- Squadron counters (C1) ---
    std::optional<int> aa_kills;
    std::optional<int> ag_kills;
    std::optional<int> as_kills;
    std::optional<int> an_kills;
    std::optional<int> missions_flown;
    std::optional<int> total_losses;

    // --- Battalion movement/state (G1) ---
    std::optional<int> x;
    std::optional<int> y;
    std::optional<int> dest_x;
    std::optional<int> dest_y;
    std::optional<uint32_t> roster;   // packed 2-bit × 16 groups
    std::optional<int> losses;
    std::optional<int> supply;
    std::optional<int> morale;
    std::optional<int> fatigue;
    std::optional<int> heading;
    std::optional<int32_t> last_move;
    std::optional<int32_t> last_combat;
};

/// The diff result: what changed between the original and the mutated
/// world documents, expressed as mutations over the decode structs.
struct DerivedSaveMutations {
    /// Campaign-level fields (clock, maintenance timers, team pools).
    /// UNSET sentinels inside mean "unchanged — keep the original".
    CampaignMutations campaign;

    /// Objectives whose fstatus or owner changed.
    std::vector<ObjectiveSaveMutation> objectives;

    /// Units whose counters or battalion state changed.
    std::vector<UnitSaveMutation> units;

    /// True when nothing differs on any owned field (a ground- and
    /// air-quiet run). build_campaign_with_mutations() on an unchanged
    /// diff still re-encodes — the .cmp/.obj/.uni round-trips are
    /// struct-identical — but a caller can use this to skip the work.
    [[nodiscard]] bool empty() const noexcept {
        return campaign.current_time == INT32_MIN &&
               campaign.last_resupply == INT32_MIN &&
               campaign.last_repair == INT32_MIN &&
               campaign.last_reinforcement == INT32_MIN &&
               campaign.te_number_aircraft.empty() &&
               objectives.empty() && units.empty();
    }
};

/// Diff two world JSON documents (the original cam2json output and the
/// WorldState::to_json_string() emission of the mutated state) over the
/// campaign loop's owned fields. Both documents share the world-JSON
/// schema (the same key vocabulary the f4-world parser reads); a field
/// absent from either document is treated as unchanged.
[[nodiscard]] DerivedSaveMutations derive_save_mutations(
    const std::string& original_json, const std::string& mutated_json);

/// CampaignSaver with the full mutation surface. See the header comment
/// for the composition. Throws on malformed input (missing subfiles_b64,
/// decode failure, unknown camp version) with the offending detail.
[[nodiscard]] std::vector<uint8_t> build_campaign_with_mutations(
    const std::string& original_json, const DerivedSaveMutations& mut);

/// File-writing form of build_campaign_with_mutations(). Returns the
/// number of bytes written.
[[nodiscard]] std::size_t save_campaign_with_mutations(
    const std::string& original_json,
    const std::filesystem::path& output_cam,
    const DerivedSaveMutations& mut);

} // namespace f4::world_convert
