// f4-install/include/f4/install/campaign.hpp
//
// One campaign save entry within a Falcon 4.0 / FreeFalcon installation.
//
// A "campaign" is a single saved game state — a .cam file produced when
// the player starts or saves a campaign. Each .cam is a self-contained
// archive (see f4-world-convert/include/f4/convert/cam_archive.hpp for
// the container format) holding campaign metadata, objectives, units,
// teams, weather, and version.
//
// On-disk layout varies between Falcon versions:
//
//   Vanilla Falcon 4.0:
//     <install>/campaign/save1.cam      (one flat directory)
//     <install>/campaign/save2.cam
//     <install>/campaign/save3.cam
//
//   FreeFalcon / multi-theater:
//     <install>/campaign/korea/save1.cam
//     <install>/campaign/korea/save2.cam
//     <install>/campaign/balkans/save1.cam
//
// We handle both by walking campaign/ recursively for *.cam files. When
// the parent directory matches a known theater key (e.g. "korea"), we
// tag the campaign with that theater; otherwise theater_key is left
// empty (the UI can prompt the user, or the .cmp metadata can be read
// later to infer it).
//
// We deliberately do NOT parse the .cam to extract display names at scan
// time — that requires loading + LZSS-decompressing every save, which is
// wasteful when the user just wants a file picker. The .cmp sub-file's
// team_name[0] field is the canonical display name; the viewer can read
// it lazily on selection.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace f4::install {

/// One campaign save (.cam file) discovered under <install>/campaign/.
struct Campaign {
    /// Filename stem — e.g. "save1" from "save1.cam".
    /// Used as a stable identifier within a theater.
    std::string stem;

    /// Absolute path to the .cam file.
    std::filesystem::path cam;

    /// Inferred theater key (e.g. "korea") from the parent directory,
    /// when it matches a known theater. Empty if the layout is flat
    /// (vanilla Falcon 4.0) or the parent dir is not a recognized theater.
    std::string theater_key;

    /// Display name for the campaign. Defaults to `stem` (e.g. "save1");
    /// the viewer may overwrite this with the team_name parsed from the
    /// .cmp sub-file when the user selects this campaign.
    std::string display_name;
};

/// Walk a campaign/ directory recursively for *.cam files.
///
/// For each .cam found, infers theater_key from the parent directory
/// name when that name appears in `known_theater_keys`. This avoids
/// false positives (e.g. a subdirectory named "backup" is not treated
/// as a theater even if it contains .cam files).
///
/// Results are sorted by (theater_key, stem) for stable UI ordering.
/// Theaters with empty theater_key (flat layout) sort first.
[[nodiscard]] std::vector<Campaign> scan_campaigns(const std::filesystem::path& campaign_dir,
                                                    const std::vector<std::string>& known_theater_keys = {});

} // namespace f4::install
