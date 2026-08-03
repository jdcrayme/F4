// f4-world-viewer/src/file_dialog.cpp

#include <f4/viewer/file_dialog.hpp>

#include <tinyfiledialogs.h>

#include <cstring>
#include <string>
#include <vector>

namespace f4::viewer {

namespace {

/// Convert our std::string filter ("JSON (*.json)|All files (*.*)") into
/// the format tinyfiledialogs v3.x wants: a single description string +
/// a vector of pattern strings ("*.json", "*.txt", ...).
///
/// We split on '|'. Each segment is "Description (*.ext1;*.ext2)" — we
/// pull the patterns out of the parens, split on ';', and add them all
/// to the patterns vector. The first segment's description becomes the
/// aSingleFilterDescription argument; subsequent segments are appended
/// as additional patterns (with their descriptions ignored — the v3.x
/// API only supports one description for the whole set).
struct FilterArrays {
    std::string description;             // aSingleFilterDescription
    std::vector<std::string> storage;    // owns the pattern strings
    std::vector<const char*> patterns;   // pointers into storage, + nullptr
};

FilterArrays split_filters(const std::string& filters) {
    FilterArrays out;
    if (filters.empty()) return out;

    std::string remaining = filters;
    bool first = true;
    while (!remaining.empty()) {
        const auto pipe = remaining.find('|');
        std::string token = (pipe == std::string::npos)
                              ? remaining
                              : remaining.substr(0, pipe);
        if (pipe == std::string::npos) {
            remaining.clear();
        } else {
            remaining = remaining.substr(pipe + 1);
        }

        // Token looks like "Description (*.ext1;*.ext2)".
        std::string desc = token;
        std::string pats;
        const auto open_paren = token.find(" (");
        if (open_paren != std::string::npos) {
            desc = token.substr(0, open_paren);
            const auto close_paren = token.find(')', open_paren);
            if (close_paren != std::string::npos) {
                pats = token.substr(open_paren + 2, close_paren - open_paren - 2);
            }
        }

        if (first) {
            out.description = desc;
            first = false;
        }
        // Split pats on ';', add each pattern.
        std::string::size_type start = 0;
        while (start < pats.size()) {
            const auto semi = pats.find(';', start);
            std::string one = (semi == std::string::npos)
                                ? pats.substr(start)
                                : pats.substr(start, semi - start);
            if (!one.empty()) {
                out.storage.push_back(one);
            }
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
    }

    for (const auto& s : out.storage) {
        out.patterns.push_back(s.c_str());
    }
    out.patterns.push_back(nullptr);
    return out;
}

} // namespace

std::filesystem::path pick_open_file(const std::string& title,
                                      const std::string& filters,
                                      const std::filesystem::path& default_path) {
    auto fa = split_filters(filters);
    const char* default_str = default_path.empty() ? nullptr
                                                    : default_path.string().c_str();
    const char* desc = fa.description.empty() ? nullptr : fa.description.c_str();
    const char* result = tinyfd_openFileDialog(
        title.c_str(),
        default_str,
        static_cast<int>(fa.patterns.size()) - 1,  // -1 for the trailing null
        fa.patterns.empty() ? nullptr : fa.patterns.data(),
        desc,
        0  // allow_multiple_selects = false
    );
    return result ? std::filesystem::path(result) : std::filesystem::path{};
}

std::filesystem::path pick_save_file(const std::string& title,
                                      const std::string& filters,
                                      const std::filesystem::path& default_path) {
    auto fa = split_filters(filters);
    const char* default_str = default_path.empty() ? nullptr
                                                    : default_path.string().c_str();
    const char* desc = fa.description.empty() ? nullptr : fa.description.c_str();
    const char* result = tinyfd_saveFileDialog(
        title.c_str(),
        default_str,
        static_cast<int>(fa.patterns.size()) - 1,
        fa.patterns.empty() ? nullptr : fa.patterns.data(),
        desc
    );
    return result ? std::filesystem::path(result) : std::filesystem::path{};
}

std::filesystem::path pick_folder(const std::string& title,
                                   const std::filesystem::path& default_path) {
    const char* default_str = default_path.empty() ? nullptr
                                                    : default_path.string().c_str();
    const char* result = tinyfd_selectFolderDialog(title.c_str(), default_str);
    return result ? std::filesystem::path(result) : std::filesystem::path{};
}

bool show_message_box(const std::string& title,
                      const std::string& message,
                      const std::string& kind) {
    const char* icon;
    if (kind == "warning")      icon = "warning";
    else if (kind == "error")   icon = "error";
    else if (kind == "question") icon = "question";
    else                         icon = "info";

    if (kind == "question") {
        return tinyfd_messageBox(title.c_str(), message.c_str(),
                                  "yesno", icon, 1) == 1;
    }
    tinyfd_messageBox(title.c_str(), message.c_str(), "ok", icon, 1);
    return true;
}

} // namespace f4::viewer
