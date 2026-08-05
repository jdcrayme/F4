// f4-world-viewer/src/campaign_and_teams_view.cpp
//
// Two related ImGui windows:
//
//   1. "Campaign" — shows the CampaignState struct: current_time, the TE
//      (Tactical Engagement) block, and the two 8-element arrays.
//   2. "Teams" — shows the team roster with both .cmp-supplied fields
//      and .tea enrichment fields.
//
// Migrated from WorldState to EntityWorld (Step 4c).

#include "viewer_state.hpp"
#include <f4/viewer/enum_text.hpp>

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace f4::viewer {

namespace {

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

void ViewerApp::draw_campaign_and_teams_view() {
    if (!impl_->world_loaded) return;

    // === Campaign window ===
    ImGui::SetNextWindowPos(ImVec2(impl_->window_w - 340, 30),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 360), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Campaign", nullptr, ImGuiWindowFlags_NoCollapse)) {
        // Access campaign entity
        auto camp_h = impl_->handle(impl_->pop.campaign);
        auto* cs = camp_h.get<f4::entities::CampaignStateComponent>();

        if (!impl_->theater_name.empty()) {
            ImGui::Text("Theater:   %s", impl_->theater_name.c_str());
        }
        ImGui::Text("Version:   %d", impl_->world_version);
        ImGui::Separator();

        if (cs) {
            ImGui::TextUnformatted("Time");
            {
                char buf[64];
                format_campaign_time(cs->current_time, buf, sizeof(buf));
                ImGui::Text("  Current:   %s", buf);
                format_campaign_time(cs->te_start_time, buf, sizeof(buf));
                ImGui::Text("  TE start:  %s", buf);
                format_campaign_time(cs->te_time_limit, buf, sizeof(buf));
                ImGui::Text("  TE limit:  %s", buf);
            }
            ImGui::Separator();

            ImGui::TextUnformatted("Tactical Engagement");
            ImGui::Text("  Type:          %d", cs->te_type);
            ImGui::Text("  # teams:       %d", cs->te_number_teams);
            ImGui::Text("  Player team:   %d", cs->te_team);
            ImGui::Text("  Victory pts:   %d", cs->te_victory_points);
            ImGui::Text("  Flags:         0x%08x", cs->te_flags);
            ImGui::Separator();

            ImGui::TextUnformatted("Per-team");
            ImGui::Text("  slot  name         aircraft  pts");
            const std::size_t n_teams = std::max<std::size_t>(
                std::max(cs->te_number_aircraft.size(), cs->te_team_pts.size()),
                impl_->pop.teams.size());
            for (std::size_t i = 0; i < n_teams; ++i) {
                const char* name = "?";
                std::string team_name_buf;
                if (i < impl_->pop.teams.size()) {
                    auto h = impl_->handle(impl_->pop.teams[i]);
                    auto* cid = h.get<f4::entities::CampaignIdentityComponent>();
                    if (cid) {
                        team_name_buf = cid->callsign;
                        name = team_name_buf.empty() ? "(empty)" : team_name_buf.c_str();
                    }
                }
                const int32_t aircraft = (i < cs->te_number_aircraft.size())
                    ? cs->te_number_aircraft[i] : 0;
                const int32_t pts = (i < cs->te_team_pts.size())
                    ? cs->te_team_pts[i] : 0;
                ImGui::Text("  %-5ld %-12s %-9d %d",
                            static_cast<long>(i), name, aircraft, pts);
            }
        }
    }
    ImGui::End();

    // === Teams window ===
    ImGui::SetNextWindowPos(ImVec2(impl_->window_w - 340, 410),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 440), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Teams", nullptr, ImGuiWindowFlags_NoCollapse)) {
        if (impl_->pop.teams.empty()) {
            ImGui::TextDisabled("(no teams loaded)");
            ImGui::End();
            return;
        }

        // Stance matrix
        if (ImGui::TreeNode("Stance Matrix",
                            "Stance Matrix (row → col)")) {
            ImGui::TextUnformatted("        ");
            for (int col = 0; col < 8; ++col) {
                if (col < static_cast<int>(impl_->pop.teams.size())) {
                    auto h = impl_->handle(impl_->pop.teams[col]);
                    auto* cid = h.get<f4::entities::CampaignIdentityComponent>();
                    if (cid && !cid->callsign.empty()) {
                        char hdr[5] = "????";
                        std::snprintf(hdr, sizeof(hdr), "%-4.4s", cid->callsign.c_str());
                        ImGui::TextUnformatted(hdr);
                    } else {
                        ImGui::TextDisabled("??   ");
                    }
                } else {
                    ImGui::TextDisabled("??   ");
                }
                ImGui::SameLine();
            }
            ImGui::TextUnformatted("");

            for (int row = 0; row < 8; ++row) {
                if (row < static_cast<int>(impl_->pop.teams.size())) {
                    auto h = impl_->handle(impl_->pop.teams[row]);
                    auto* cid = h.get<f4::entities::CampaignIdentityComponent>();
                    char row_label[24];
                    std::snprintf(row_label, sizeof(row_label), "%-5d %-12s",
                                  row,
                                  (cid && !cid->callsign.empty()) ? cid->callsign.c_str() : "(empty)");
                    ImGui::TextUnformatted(row_label);
                } else {
                    ImGui::Text("%-5d (empty)         ", row);
                }
                ImGui::SameLine();
                for (int col = 0; col < 8; ++col) {
                    int16_t s = 0;
                    bool have = false;
                    if (row < static_cast<int>(impl_->pop.teams.size())) {
                        auto h = impl_->handle(impl_->pop.teams[row]);
                        auto* tc = h.get<f4::entities::TeamComponent>();
                        if (tc && col < static_cast<int>(tc->stance.size())) {
                            s = tc->stance[col];
                            have = true;
                        }
                    }
                    char cell[16];
                    if (have) {
                        std::snprintf(cell, sizeof(cell), "%-5d", s);
                    } else {
                        std::snprintf(cell, sizeof(cell), "  -  ");
                    }
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
        for (std::size_t i = 0; i < impl_->pop.teams.size(); ++i) {
            auto h = impl_->handle(impl_->pop.teams[i]);
            auto* cid = h.get<f4::entities::CampaignIdentityComponent>();
            auto* tc = h.get<f4::entities::TeamComponent>();
            const auto& t_name = cid ? cid->callsign : std::string();
            char label[64];
            std::snprintf(label, sizeof(label), "[%ld] %s",
                          static_cast<long>(i),
                          t_name.empty() ? "(empty)" : t_name.c_str());
            if (ImGui::TreeNode(label)) {
                if (tc) {
                    ImGui::Text("flags:     0x%02x", tc->flags);
                    ImGui::Text("colour:    %d", tc->colour);
                    if (!tc->motto.empty()) {
                        ImGui::TextWrapped("motto:     %s", tc->motto.c_str());
                    }

                    // .tea enrichment — detect by checking if stance is non-empty
                    bool tea_loaded = !tc->stance.empty();
                    if (tea_loaded) {
                        ImGui::Separator();
                        ImGui::TextUnformatted(".tea enrichment");

                        // Country memberships
                        if (!tc->member.empty()) {
                            ImGui::Text("  member countries:");
                            for (std::size_t j = 0; j < tc->member.size(); ++j) {
                                if (j && (j % 8 == 0)) ImGui::TextUnformatted("    ");
                                ImGui::SameLine();
                                const int m = tc->member[j];
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

                        ImGui::Text("  first_colonel:   %d", tc->first_colonel);
                        ImGui::Text("  first_commander: %d", tc->first_commander);
                        ImGui::Text("  first_wingman:   %d", tc->first_wingman);
                        ImGui::Text("  last_wingman:    %d", tc->last_wingman);

                        ImGui::TextUnformatted("  experience");
                        ImGui::Text("    air:          %d", tc->air_experience);
                        ImGui::Text("    air_defense:  %d", tc->air_defense_experience);
                        ImGui::Text("    ground:       %d", tc->ground_experience);
                        ImGui::Text("    naval:        %d", tc->naval_experience);
                    } else {
                        ImGui::TextDisabled("  (.tea enrichment not loaded for this slot)");
                    }
                }
                ImGui::TreePop();
            }
        }
    }
    ImGui::End();
}

} // namespace f4::viewer
