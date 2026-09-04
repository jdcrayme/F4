// f4-world-convert/include/f4/world_convert/campaign_json.hpp
//
// from_world_json_campaign — parse the "campaign" JSON block (emitted by
// to_world_json) back into a CampaignHeader. This is the save-write path's
// campaign-state reader: a host (or json2cam --reencode-cmp) loads a world
// JSON, mutates the campaign block (advance current_time, apply ledger
// results, ...), and calls encode_cmp(header) to produce a re-encoded .cmp.
//
// The parser walks the JSON with f4::json::Reader, reading every field
// encode_cmp needs. Fields absent from the JSON (e.g. an older file predating
// the te_number_f16s / camp_map_b64 / squadrons / remaining_payload_b64
// additions) default to 0/empty — the encoder writes those as zero, which
// is the correct behavior for a fresh campaign.
//
// Only the .cmp campaign-header fields are parsed here. The .tea team
// enrichment (cteam, stance, ATM, ...) is NOT needed by encode_cmp — the
// .cmp team block carries only flags/colour/name/motto per slot. A host
// that wants to re-encode the .tea sub-file too uses the .tea encoder
// (follow-on; see Docs/SAVE_WRITE_PLAN.md §6).

#pragma once

#include <f4/world_convert/campaign_decoder.hpp>

#include <string>

namespace f4::world_convert {

/// Parse the "campaign" block from a world JSON document into a
/// CampaignHeader. Throws std::runtime_error if the block is absent or
/// malformed. `camp_version` defaults to 63 (the save1.cam fixture's
/// version); pass a different value if the JSON carries a different
/// "version" field (the caller reads it from the top-level "version" key).
[[nodiscard]] CampaignHeader from_world_json_campaign(
    const std::string& world_json, int camp_version = 63);

/// Convenience: parse the top-level "version" field from a world JSON.
/// Returns 63 (the default) if absent.
[[nodiscard]] int read_world_json_version(const std::string& world_json);

} // namespace f4::world_convert
