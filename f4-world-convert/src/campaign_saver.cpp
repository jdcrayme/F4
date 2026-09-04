// f4-world-convert/src/campaign_saver.cpp
//
// CampaignSaver — the integration bridge. See campaign_saver.hpp for the
// design. This is the "modified-save" API the campaign simulation calls
// to persist a mutated campaign state as a .cam file.

#include <f4/world_convert/campaign_saver.hpp>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/cam_writer.hpp>
#include <f4/world_convert/campaign_json.hpp>
#include <f4/world_convert/cmp_encoder.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace f4::world_convert {

namespace {

// The sentinel for "not set." INT32_MIN is outside the valid CampaignTime
// range (which is always non-negative — game ticks from epoch 0).
constexpr int32_t UNSET = INT32_MIN;

// Apply the mutations to a CampaignHeader parsed from the world JSON.
void apply_mutations(CampaignHeader& h, const CampaignMutations& mut) {
    if (mut.current_time != UNSET) h.current_time = mut.current_time;
    if (mut.last_resupply != UNSET) h.last_resupply = mut.last_resupply;
    if (mut.last_repair != UNSET) h.last_repair = mut.last_repair;
    if (mut.last_reinforcement != UNSET) h.last_reinforcement = mut.last_reinforcement;

    // Team pools: override entries [0..min(size,8)).
    if (!mut.te_number_aircraft.empty()) {
        if (h.te_number_aircraft.size() < 8) h.te_number_aircraft.resize(8, 0);
        const std::size_t n = std::min<std::size_t>(mut.te_number_aircraft.size(), 8);
        for (std::size_t i = 0; i < n; ++i) {
            h.te_number_aircraft[i] = mut.te_number_aircraft[i];
        }
    }
}

} // namespace

std::vector<uint8_t> build_campaign(const std::string& world_json,
                                      const CampaignMutations& mut) {
    // Determine the camp version.
    int camp_version = mut.camp_version;
    if (camp_version == 0) {
        camp_version = read_world_json_version(world_json);
    }

    // Parse the campaign block from the world JSON.
    CampaignHeader h = from_world_json_campaign(world_json, camp_version);

    // Apply the mutations.
    apply_mutations(h, mut);

    // Re-encode the .cmp.
    auto cmp_bytes = encode_cmp(h, camp_version);

    // Load the original subfiles from the world JSON (via the proven
    // cam_from_world_json path) and replace the .cmp with the re-encoded one.
    auto passthrough = cam_from_world_json(world_json);

    // Write to a temp file, load as CamArchive, swap .cmp, re-write.
    // (CamArchive::load takes a path; this is the simplest way to get
    // the subfiles into a CamWriter. A future refactor could add a
    // CamArchive::load_from_bytes() to avoid the temp file.)
    auto tmp = std::filesystem::temp_directory_path() / "f4_campaign_saver_tmp.cam";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(passthrough.data()),
                static_cast<std::streamsize>(passthrough.size()));
    }
    CamArchive cam;
    cam.load(tmp);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    // Find the .cmp sub-file's name (e.g. "save1.cmp") so we replace
    // the right entry.
    std::string cmp_name = "save1.cmp";
    for (const auto& sf : cam.subfiles()) {
        if (sf.ext() == "cmp") { cmp_name = sf.name; break; }
    }

    CamWriter w;
    for (const auto& sf : cam.subfiles()) {
        if (sf.ext() == "cmp") {
            w.add(cmp_name, cmp_bytes);   // the re-encoded .cmp
        } else {
            w.add(sf.name, sf.data);      // pass through verbatim
        }
    }
    return w.build();
}

std::size_t save_campaign(const std::string& world_json,
                           const std::filesystem::path& output_cam,
                           const CampaignMutations& mut) {
    auto bytes = build_campaign(world_json, mut);

    std::error_code mk_ec;
    std::filesystem::create_directories(output_cam.parent_path(), mk_ec);
    std::ofstream f(output_cam, std::ios::binary);
    if (!f) throw std::runtime_error("save_campaign: cannot write " + output_cam.string());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) throw std::runtime_error("save_campaign: short write to " + output_cam.string());

    return bytes.size();
}

} // namespace f4::world_convert
