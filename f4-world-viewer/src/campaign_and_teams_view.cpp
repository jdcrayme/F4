// f4-world-viewer/src/campaign_and_teams_view.cpp
//
// Two related ImGui windows:
//
//   1. "Campaign" — shows the CampaignState struct: current_time, the TE
//      (Tactical Engagement) block (te_start_time, te_time_limit,
//      te_victory_points, te_type, te_number_teams, te_team, te_flags),
//      and the two 8-element arrays (te_number_aircraft, te_team_pts).
//      All fields are already loaded into WorldState::campaign — this
//      window just renders them. Useful for debugging TE scenario
//      setup and tracking victory point scoring during a campaign.
//
//   2. "Teams" — shows the team roster with both .cmp-supplied fields
//      (slot, flags, colour, name, motto) and .tea enrichment fields
//      (cteam, team_flags, member[] country memberships, stance[] matrix,
//      first_colonel/commander/wingman pilot slot indices, and the four
//      experience values). The stance matrix is rendered as an 8x8 grid
//      so team-vs-team relationships are visible at a glance.
//
// Both windows auto-show when a world is loaded. They can be collapsed
// or closed via the standard ImGui controls; they'll reappear next time
// a world is loaded.

#include "viewer_state.hpp"
#include <f4/viewer/enum_text.hpp>

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace f4::viewer {

namespace {

// Format a CampaignTime value (game seconds since campaign start) as
// "DDd HH:MM:SS" for readability. The exact unit of CampaignTime is
// not well-documented in the FreeFalcon source — it's typically treated
// as seconds, but some scenarios (TE saves) use it as an absolute
// timestamp. We show both the raw value and the decomposed form so the
// user can pick the right interpretation.
void format_campaign_time(int32_t t, char* buf, std::size_t buf_size) {
    if (t <= 0) {
        std::snprintf(buf, buf_size, "%d (0d 00:00:00)", t);
        return;
    }
    const int32_t total_sec = t;
    const int days = total_sec / 86400;
    const int hours = (total_sec % 86400) / 3600;
    const int mins = (total_sec % 3600) / 60;
    const int secs = total_sec % 60;
    std::snprintf(buf, buf_size, "%d (%dd %02d:%02d:%02d)",
                  t, days, hours, mins, secs);
}

} // namespace

// ---------------------------------------------------------------------------
// ViewerApp::draw_campaign_and_teams_view
// ---------------------------------------------------------------------------
void ViewerApp::draw_campaign_and_teams_view() {
    if (!impl_->world_loaded) return;

    // === Campaign window ===
    ImGui::SetNextWindowPos(ImVec2(impl_->window_w - 340, 30),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 360), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Campaign", nullptr, ImGuiWindowFlags_NoCollapse)) {
        const auto& c = impl_->world.campaign;

        if (!impl_->world.theater.empty()) {
            ImGui::Text("Theater:   %s", impl_->world.theater.c_str());
        }
        ImGui::Text("Version:   %d", impl_->world.version);
        ImGui::Separator();

        ImGui::TextUnformatted("Time");
        {
            char buf[64];
            format_campaign_time(c.current_time, buf, sizeof(buf));
            ImGui::Text("  Current:   %s", buf);
            format_campaign_time(c.te_start_time, buf, sizeof(buf));
            ImGui::Text("  TE start:  %s", buf);
            format_campaign_time(c.te_time_limit, buf, sizeof(buf));
            ImGui::Text("  TE limit:  %s", buf);
        }
        ImGui::Separator();

        ImGui::TextUnformatted("Tactical Engagement");
        ImGui::Text("  Type:          %d", c.te_type);
        ImGui::Text("  # teams:       %d", c.te_number_teams);
        ImGui::Text("  Player team:   %d", c.te_team);
        ImGui::Text("  Victory pts:   %d", c.te_victory_points);
        ImGui::Text("  Flags:         0x%08x", c.te_flags);
        ImGui::Separator();

        // Per-team aircraft counts and victory points — render as a small
        // table with one row per team, aligned with the team names from
        // WorldState.teams[].
        ImGui::TextUnformatted("Per-team");
        ImGui::Text("  slot  name         aircraft  pts");
        const std::size_t n_teams = std::max<std::size_t>(
            std::max(c.te_number_aircraft.size(), c.te_team_pts.size()),
            impl_->world.teams.size());
        for (std::size_t i = 0; i < n_teams; ++i) {
            const char* name = "?";
            if (i < impl_->world.teams.size()) {
                const auto& t = impl_->world.teams[i];
                name = t.name.empty() ? "(empty)" : t.name.c_str();
            }
            const int32_t aircraft = (i < c.te_number_aircraft.size())
                ? c.te_number_aircraft[i] : 0;
            const int32_t pts = (i < c.te_team_pts.size())
                ? c.te_team_pts[i] : 0;
            ImGui::Text("  %-5ld %-12s %-9d %d",
                        static_cast<long>(i), name, aircraft, pts);
        }
    }
    ImGui::End();

    // === Teams window ===
    // Shows the team roster with .cmp + .tea enrichment. Renders below
    // the Campaign window by default.
    ImGui::SetNextWindowPos(ImVec2(impl_->window_w - 340, 410),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 440), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Teams", nullptr, ImGuiWindowFlags_NoCollapse)) {
        if (impl_->world.teams.empty()) {
            ImGui::TextDisabled("(no teams loaded)");
            ImGui::End();
            return;
        }

        // Stance matrix (8x8 grid of int16 values, rows = "from team",
        // cols = "to team"). Rendered first so it's visible at a glance.
        // We color-code: positive = allied (green), negative = hostile
        // (red), zero = neutral (gray).
        if (ImGui::TreeNode("Stance Matrix",
                            "Stance Matrix (row → col)")) {
            // Header row: short team-name abbreviations (first 4 chars).
            ImGui::TextUnformatted("        ");
            for (int col = 0; col < 8; ++col) {
                if (col < static_cast<int>(impl_->world.teams.size())) {
                    const auto& tn = impl_->world.teams[col];
                    if (!tn.name.empty()) {
                        char hdr[5] = "????";
                        std::snprintf(hdr, sizeof(hdr), "%-4.4s", tn.name.c_str());
                        ImGui::TextUnformatted(hdr);
                    } else {
                        ImGui::TextDisabled("??   ");
                    }
                } else {
                    ImGui::TextDisabled("??   ");
                }
                ImGui::SameLine();
            }
            ImGui::TextUnformatted("");  // end header row

            for (int row = 0; row < 8; ++row) {
                if (row < static_cast<int>(impl_->world.teams.size())) {
                    const auto& tr = impl_->world.teams[row];
                    char row_label[24];
                    std::snprintf(row_label, sizeof(row_label), "%-5d %-12s",
                                  row,
                                  tr.name.empty() ? "(empty)" : tr.name.c_str());
                    ImGui::TextUnformatted(row_label);
                } else {
                    ImGui::Text("%-5d (empty)         ", row);
                }
                ImGui::SameLine();
                // Render 8 stance cells in a row, color-coded.
                for (int col = 0; col < 8; ++col) {
                    int16_t s = 0;
                    bool have = false;
                    if (row < static_cast<int>(impl_->world.teams.size())) {
                        const auto& tr = impl_->world.teams[row];
                        if (col < static_cast<int>(tr.stance.size())) {
                            s = tr.stance[col];
                            have = true;
                        }
                    }
                    char cell[16];
                    if (have) {
                        std::snprintf(cell, sizeof(cell), "%-5d", s);
                    } else {
                        std::snprintf(cell, sizeof(cell), "  -  ");
                    }
                    // Color: positive = green (allied), negative = red
                    // (hostile), zero = gray (neutral), missing = dim gray.
                    if (!have) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImVec4(0.45f, 0.45f, 0.45f, 0.5f));
                    } else if (s > 0) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                    } else if (s < 0) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                    }
                    ImGui::TextUnformatted(cell);
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted("");
            }
            ImGui::TextDisabled("  (positive=allied, negative=hostile, 0=neutral)");
            ImGui::TreePop();
        }

        ImGui::Separator();

        // Per-team detail tree.
        for (std::size_t i = 0; i < impl_->world.teams.size(); ++i) {
            const auto& t = impl_->world.teams[i];
            char label[64];
            std::snprintf(label, sizeof(label), "[%ld] %s%s",
                          static_cast<long>(i),
                          t.name.empty() ? "(empty)" : t.name.c_str(),
                          t.tea_loaded ? "  (+.tea)" : "");
            if (ImGui::TreeNode(label)) {
                // .cmp block — always present.
                ImGui::Text("flags:     0x%02x", t.flags);
                ImGui::Text("colour:    %d", t.colour);
                if (!t.motto.empty()) {
                    ImGui::TextWrapped("motto:     %s", t.motto.c_str());
                }

                if (t.tea_loaded) {
                    ImGui::Separator();
                    ImGui::TextUnformatted(".tea enrichment");
                    ImGui::Text("  cteam:          %d", t.cteam);
                    ImGui::Text("  team_flags:     0x%04x", static_cast<unsigned>(t.team_flags));

                    // Country memberships — render as a row of 8 cells.
                    if (!t.member.empty()) {
                        ImGui::Text("  member countries:");
                        for (std::size_t j = 0; j < t.member.size(); ++j) {
                            if (j && (j % 8 == 0)) ImGui::TextUnformatted("    ");
                            ImGui::SameLine();
                            const int m = t.member[j];
                            if (m) {
                                ImGui::PushStyleColor(ImGuiCol_Text,
                                    ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                                ImGui::Text("[%ld:%d]", static_cast<long>(j), m);
                                ImGui::PopStyleColor();
                            } else {
                                ImGui::PushStyleColor(ImGuiCol_Text,
                                    ImVec4(0.45f, 0.45f, 0.45f, 0.5f));
                                ImGui::Text("[%ld:0]", static_cast<long>(j));
                                ImGui::PopStyleColor();
                            }
                        }
                    }

                    // Command chain — pilot slot indices.
                    ImGui::Text("  first_colonel:   %d", t.first_colonel);
                    ImGui::Text("  first_commander: %d", t.first_commander);
                    ImGui::Text("  first_wingman:   %d", t.first_wingman);
                    ImGui::Text("  last_wingman:    %d", t.last_wingman);

                    // Experience values — render as a small bar chart.
                    ImGui::TextUnformatted("  experience");
                    ImGui::Text("    air:          %d", t.air_experience);
                    ImGui::Text("    air_defense:  %d", t.air_defense_experience);
                    ImGui::Text("    ground:       %d", t.ground_experience);
                    ImGui::Text("    naval:        %d", t.naval_experience);
                } else {
                    ImGui::TextDisabled("  (.tea enrichment not loaded for this slot)");
                }

                ImGui::TreePop();
            }
        }
    }
    ImGui::End();
}

} // namespace f4::viewer
