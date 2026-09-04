// f4-world-convert/include/f4/world_convert/campaign_saver.hpp
//
// CampaignSaver — the integration bridge between the campaign simulation's
// mutated state and the binary save format.
//
// The campaign simulation (f4-campaign + f4-simulation) mutates a WorldState
// via apply_to(ledger, ws): current_time advances, team pools attrit,
// squadrons accumulate kills, objectives take damage. CampaignSaver persists
// those mutations back to a .cam file by:
//   1. Reading the original world JSON (which carries the campaign block +
//      subfiles_b64 from `cam2json --preserve-subfiles`)
//   2. Parsing the campaign block into a CampaignHeader
//   3. Overwriting the mutated fields from CampaignMutations
//   4. Re-encoding the .cmp via encode_cmp
//   5. Assembling the .cam via CamWriter (new .cmp + all other subfiles
//      passed through verbatim from subfiles_b64)
//
// The .uni (squadron kills) and .obj (objective fstatus) mutations are
// documented follow-ons — they require decoding the sub-file, mutating the
// decode struct, and re-encoding. This tranche lands the .cmp path (the
// most common modified-save case: campaign clock + team pools + timers).
//
// CampaignMutations is a plain-data struct (no f4-world dependency) so the
// host fills it from WorldState without pulling f4-world-convert into a
// dependency cycle. See Docs/SAVE_WRITE_PLAN.md §5.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::world_convert {

/// The campaign-level mutations to apply when saving. Fields default to
/// INT32_MIN as a sentinel meaning "not set — keep the original value from
/// the world JSON's campaign block." Set a field to its mutated value to
/// override it in the re-encoded .cmp.
struct CampaignMutations {
    /// Campaign clock (CampaignTime, game ticks). Advanced by the sim's
    /// tick loop. INT32_MIN = keep original.
    int32_t current_time = INT32_MIN;

    /// Maintenance-timer anchors (absolute CampaignTime). Advanced by the
    /// campaign's resupply/repair/reinforcement cadences. INT32_MIN = keep.
    int32_t last_resupply = INT32_MIN;
    int32_t last_repair = INT32_MIN;
    int32_t last_reinforcement = INT32_MIN;

    /// Team aircraft pools (te_number_aircraft[8]). Attrited by combat
    /// losses, refilled by reinforcements. Empty = keep all originals;
    /// a non-empty vector overrides entries [0..min(size,8)).
    std::vector<int32_t> te_number_aircraft;

    /// gCampDataVersion. If 0, read from the world JSON's "version" field.
    int camp_version = 0;
};

/// Save a modified campaign to a .cam file.
///
/// @param world_json    the original world JSON (produced by
///                      `cam2json --preserve-subfiles` — must carry a
///                      "subfiles_b64" block and a "campaign" block)
/// @param output_cam    the path to write the reassembled .cam
/// @param mut           the campaign-level mutations to apply
/// @return the number of bytes written
/// @throws std::runtime_error if the world JSON lacks the required blocks
///         or the re-encode fails
std::size_t save_campaign(const std::string& world_json,
                           const std::filesystem::path& output_cam,
                           const CampaignMutations& mut);

/// Build the modified .cam bytes in memory (for testing). Same as
/// save_campaign() but returns the bytes instead of writing to disk.
[[nodiscard]] std::vector<uint8_t> build_campaign(
    const std::string& world_json, const CampaignMutations& mut);

} // namespace f4::world_convert
