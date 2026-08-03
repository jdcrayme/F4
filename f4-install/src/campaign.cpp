// f4-install/src/campaign.cpp

#include <f4/install/campaign.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>

namespace f4::install {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// True if `name` ends with `.cam` (case-insensitive).
bool has_cam_extension(const std::string& name) {
    const std::string lower = to_lower(name);
    return lower.size() > 4 &&
           lower.compare(lower.size() - 4, 4, ".cam") == 0;
}

} // namespace

std::vector<Campaign> scan_campaigns(const std::filesystem::path& campaign_dir,
                                      const std::vector<std::string>& known_theater_keys) {
    std::vector<Campaign> out;
    std::error_code ec;
    if (!std::filesystem::exists(campaign_dir, ec)) return out;

    // Lowercase the known theater keys once for O(1) lookup.
    std::unordered_set<std::string> known;
    known.reserve(known_theater_keys.size());
    for (const auto& k : known_theater_keys) {
        known.insert(to_lower(k));
    }

    // recursive_directory_iterator handles both the flat layout
    // (campaign/save1.cam — visited directly) and the nested layout
    // (campaign/korea/save1.cam — parent dir name is the theater key).
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(campaign_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (!has_cam_extension(entry.path().filename().string())) continue;

        Campaign c;
        c.cam = entry.path();
        c.stem = entry.path().stem().string();

        // Infer theater from parent directory if it matches a known key.
        const auto parent = entry.path().parent_path();
        if (!parent.empty() && parent != campaign_dir) {
            const std::string parent_name = to_lower(parent.filename().string());
            if (known.contains(parent_name)) {
                c.theater_key = parent_name;
            }
        }

        c.display_name = c.stem;
        out.push_back(std::move(c));
    }

    // Stable sort: (theater_key, stem). Empty theater_key sorts first
    // (so flat-layout campaigns come before per-theater ones).
    std::sort(out.begin(), out.end(), [](const Campaign& a, const Campaign& b) {
        if (a.theater_key != b.theater_key) return a.theater_key < b.theater_key;
        return a.stem < b.stem;
    });
    return out;
}

} // namespace f4::install
