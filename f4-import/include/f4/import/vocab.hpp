// f4-import/include/f4/import/vocab.hpp
//
// Family vocabulary tables — the "hard-coded list", done once and as
// data (Docs/AIRCRAFT_ANIMATION_PLAN.md §3.3).
//
// FreeFalcon's src/sim/include/dofsnswitches.h defines four PER-FAMILY
// index spaces (complex / simple / helicopter / air-defense) over the
// same 0..N DOF/switch numbers; within a family, individual models may
// repurpose indices (the header itself flags 6-8 and 25-27). The
// converter therefore classifies a model into a family, applies the
// family table to rename dof:unknown.N → dof:<semantic-id> and bind
// channels, and keeps everything unmapped as dof:unknown.N (preserved,
// doctor-linted) exactly per ASSET_PIPELINE_SPEC §6.7.
//
// Table JSON shape (f4-import/vocab/family/<family>.json):
// {
//   "f4": { "v": 1 },
//   "family": "complex",
//   "dof": { "0": { "id": "stab.l", "channel": "stab.l" }, ... },
//   "sw":  { "0": { "id": "ab",     "channel": "sw.ab" }, ... }
// }
// Keys are FreeFalcon dof/switch numbers. "channel" is the serialized
// f4::anim name; "id" is the §6 tag id used in node names.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace f4::import {

/// One index → (tag id, channel) binding.
struct AnimBinding {
    std::string id;        ///< §6 tag id, e.g. "gear_leg.0"
    std::string channel;   ///< f4::anim serialized channel, e.g. "gear_leg_pos.0"
};

/// One family's mapping from FreeFalcon DOF/switch numbers to semantic
/// tags + channels.
struct FamilyTable {
    std::string family;    ///< "complex", "simple", "heli", "airdef"
    std::map<int32_t, AnimBinding> dof;
    std::map<int32_t, AnimBinding> sw;

    /// Look up a DOF binding; nullptr when the index is unmapped.
    [[nodiscard]] const AnimBinding* dof_at(int32_t n) const noexcept {
        auto it = dof.find(n);
        return it == dof.end() ? nullptr : &it->second;
    }
    /// Look up a switch binding; nullptr when the index is unmapped.
    [[nodiscard]] const AnimBinding* sw_at(int32_t n) const noexcept {
        auto it = sw.find(n);
        return it == sw.end() ? nullptr : &it->second;
    }
};

/// Load a family table from JSON. Throws std::runtime_error on parse
/// errors (the importer treats a broken vocab as a hard error —
/// silently mis-tagging models would be worse).
[[nodiscard]] FamilyTable load_family_table(
    const std::filesystem::path& json_path);

/// Load every family JSON in a directory (vocab/family/*.json).
/// Returns an empty entry keyed "none" if the directory doesn't exist
/// (so callers degrade to unknown.N tagging rather than failing).
[[nodiscard]] std::map<std::string, FamilyTable> load_family_tables(
    const std::filesystem::path& family_dir);

/// Guess the family for a model from its DOF-index set and slot/switch
/// counts — the same signals ModelRecord::visual_class() uses, refined
/// for the animation families. The discriminator that counts alone
/// can't express: a HELICOPTER carries the FF rotor pair (dof 2 =
/// HELI_MAIN_ROTOR + dof 4 = HELI_TAIL_ROTOR) in its tree; a ground
/// unit with the same DOF count is a radar/turret model and must NOT
/// get the heli table (its sweep would stay unbound). `dof_indices` is
/// the distinct set collected from the parsed BSP tree's transform
/// nodes; `effective_dofs` is the record's declared count (the complex
/// threshold, which the LOD tree alone can undercount). Community
/// models that disagree with the guess are corrected through overrides
/// (future) or the doctor's warnings; a wrong guess degrades to
/// unmapped unknown.N tags, never to wrong geometry.
[[nodiscard]] std::string guess_family(
    const std::vector<int>& dof_indices,
    int effective_dofs,
    int effective_switches,
    int n_slots);

} // namespace f4::import
