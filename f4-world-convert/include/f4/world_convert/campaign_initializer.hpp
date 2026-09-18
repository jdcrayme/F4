// f4-world-convert/include/f4/world_convert/campaign_initializer.hpp
//
// CAMP-INIT-1 — create-from-parameters (CAMP_HOST_PLAN.md §8).
//
// CampaignInitializer turns a ScenarioPack into a FRESH .cam campaign
// archive — the inverse problem campaign_saver solves for an existing
// one. Where the saver re-encodes a decoded baseline, the initializer
// SYNTHESIZES the decode structs from the pack and feeds them through
// the Task-70 encoder stack (encode_cmp / encode_obj / encode_obd /
// encode_tea / encode_uni) into a CamWriter container:
//
//   ScenarioPack → { CampaignHeader, DecodedObjectives, DecodedTeams,
//                    DecodedUnits } → encoders → CamWriter → .cam bytes
//
// Byte-identity by construction: the synthesis is a pure function of
// the pack (the pack's seed lands in the .cmp's CreationRand field, so
// it is part of the input, not an entropy source) — two builds of one
// pack produce byte-identical archives, and any pack difference that
// matters moves the bytes.
//
// The world JSON the runtime consumes is produced by THE EXISTING
// READER — build() bytes → CamArchive::load_from_memory → to_world_json
// — so "a generated save decodes in the existing reader" holds by
// construction, not by a second hand-rolled projection.
//
// Fresh-save conventions (documented deviations from a stock save):
//   * the .evt/.plt/.pst/.wth sub-files ride EMPTY — no decoder or
//     encoder exists for those types (SAVE_WRITE_PLAN's passthrough
//     set); nothing in this repo's reader or runtime reads them. A
//     donor-copy mode can fill them later without touching the gates.
//   * the .obd is the canonical zero-delta form (10 bytes — exactly
//     what save1.cam carries).
//   * the maintenance anchors (last_resupply/repair/reinforcement)
//     start at the pack's current_time — one full cadence period of
//     grace before the first resupply/repair/reinforcement cycle.
//   * the camp map is the nearest-objective ownership fill (2 bits per
//     cell) — deterministic and inert for the runtime.
//
// Dependencies: scenario_pack.hpp, the decoder/encoder headers,
// cam_writer.hpp, class_table.hpp; f4-lzss via the encoders.

#pragma once

#include <f4/world_convert/campaign_decoder.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/scenario_pack.hpp>
#include <f4/world_convert/team_decoder.hpp>
#include <f4/world_convert/unit_decoder.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f4::world_convert {

/// The synthesized decode structs (the initializer's intermediate form —
/// exactly what the encoders consume and the decoders verify).
struct SynthesizedCampaign {
    int camp_version = 71;
    CampaignHeader header;
    DecodedObjectives objectives;
    DecodedObjectiveDeltas deltas;   // always the empty form
    DecodedTeams teams;
    DecodedUnits units;
};

/// One build's output: the synthesized structs and the assembled .cam
/// bytes (the Task-70 encoder stack over them, containered by
/// CamWriter in the stock manifest order).
struct InitBuildResult {
    SynthesizedCampaign world;
    std::vector<uint8_t> cam_bytes;
};

/// Statistics for the CLI / tests.
struct InitStats {
    int objectives = 0;
    int squadrons = 0;
    int battalions = 0;
    int named_teams = 0;
    uint32_t last_index_num = 0;
    std::size_t cam_bytes = 0;
};

class CampaignInitializer {
public:
    /// Build a fresh campaign from a validated pack. `ct` is required —
    /// the pack's named kinds resolve to class-table entity types
    /// (first-match, deterministic); a missing kind is a named error.
    /// Throws std::runtime_error (prefix `init: `) on any fault.
    [[nodiscard]] static InitBuildResult build(const ScenarioPack& pack,
                                               const ClassTable& ct,
                                               InitStats* stats = nullptr);

    /// The reader-side projection: decode the generated .cam bytes with
    /// the EXISTING reader (CamArchive) and emit the world JSON the
    /// runtime consumes — the same code path cam2json drives.
    [[nodiscard]] static std::string to_world_json(
        const InitBuildResult& built, const ClassTable& ct,
        const ScenarioPack& pack);
};

} // namespace f4::world_convert
