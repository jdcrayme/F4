// f4-world-viewer/src/campaign_and_teams_view.cpp
//
// The "Campaign Info" window — one window, two tabs:
//
//   1. "Campaign" — the CampaignState struct: current_time, the TE
//      (Tactical Engagement) block, and the two 8-element arrays.
//   2. "Teams" — the team roster with both .cmp-supplied fields
//      and .tea enrichment fields.
//
// (Migrated from WorldState to EntityWorld, Step 4c. Was two separate
// always-open windows on the right edge; merged into one tabbed window
// during the UI cleanup pass so a loaded world doesn't blanket the
// screen. Visibility: show_campaign_info — Windows menu / close button.)

#include "viewer_state.hpp"
#include <f4/viewer/enum_text.hpp>

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace f4::viewer {

// format_campaign_time lives in enum_text.hpp (shared with the ATO view
// and the inspector since the B.3 QC tranche).

void ViewerApp::draw_campaign_and_teams_view() {
    if (!impl_->world_loaded || !impl_->show_campaign_info) return;

    ImGui::SetNextWindowPos(ImVec2(620, 410), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440, 330), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Campaign Info", &impl_->show_campaign_info,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!ImGui::BeginTabBar("##campaign_info_tabs")) {
        ImGui::End();
        return;
    }

    // The Teams tab body — a lambda so it stays out of the way of the
    // tab scaffolding above (same pattern as campaign_qc_view's helpers).
    const auto draw_teams_body = [this]() {
        // Stance matrix
        if (ImGui::TreeNode("Stance Matrix",
                            "Stance Matrix (row → col)")) {
            ImGui::TextUnformatted("        ");
            for (int col = 0; col < 8; ++col) {
                if (col < static_cast<int>(impl_->teams().size())) {
                    auto h = impl_->handle(impl_->teams()[col]);
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
                if (row < static_cast<int>(impl_->teams().size())) {
                    auto h = impl_->handle(impl_->teams()[row]);
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
                    if (row < static_cast<int>(impl_->teams().size())) {
                        auto h = impl_->handle(impl_->teams()[row]);
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

        // Per-team detail tree. Each row leads with the team's map
        // color — the color key the old Legend window carried, moved
        // here where the team data actually lives.
        for (std::size_t i = 0; i < impl_->teams().size(); ++i) {
            auto h = impl_->handle(impl_->teams()[i]);
            auto* cid = h.get<f4::entities::CampaignIdentityComponent>();
            auto* tc = h.get<f4::entities::TeamComponent>();
            const auto& t_name = cid ? cid->callsign : std::string();
            char label[64];
            std::snprintf(label, sizeof(label), "[%ld] %s",
                          static_cast<long>(i),
                          t_name.empty() ? "(empty)" : t_name.c_str());
            ImGui::PushID(static_cast<int>(i));
            const auto cc = color_for_owner(static_cast<uint8_t>(i));
            ImGui::ColorButton("##teamcolor",
                ImVec4(cc.r / 255.0f, cc.g / 255.0f, cc.b / 255.0f, 1.0f),
                ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
            ImGui::PopID();
            ImGui::SameLine();
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
    };

    // === Campaign tab ===
    if (ImGui::BeginTabItem("Campaign")) {
        // Access campaign entity (Phase C/D: O(1) tag-index lookup via
        // campaign_entity() helper, which calls with_tag_ref(ROLE="campaign")).
        auto camp_h = impl_->handle(impl_->campaign_entity());
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
                impl_->teams().size());
            for (std::size_t i = 0; i < n_teams; ++i) {
                const char* name = "?";
                std::string team_name_buf;
                if (i < impl_->teams().size()) {
                    auto h = impl_->handle(impl_->teams()[i]);
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
        ImGui::EndTabItem();
    }

    // === Teams tab ===
    if (ImGui::BeginTabItem("Teams")) {
        if (impl_->teams().empty()) {
            ImGui::TextDisabled("(no teams loaded)");
        } else {
            draw_teams_body();
        }
        ImGui::EndTabItem();
    }

    // === Structure tab ===
    // The brigade → battalion org tree, restored here after the map's
    // hierarchy lines were removed (the structure is reference data:
    // no C2 model consumes it yet, so drawing it as map lines implied
    // a command relationship that doesn't exist).
    if (ImGui::BeginTabItem("Structure")) {
        // Entity display name: the NAME tag, else the unit class name.
        const auto unit_display_name = [this](f4::entities::EntityId eid)
            -> std::string {
            if (!eid.valid()) return {};
            auto h = impl_->handle(eid);
            auto name_tag = h.get_tag(f4::entities::tags::NAME);
            if (name_tag && name_tag->as_string() &&
                !name_tag->as_string()->empty()) {
                return *name_tag->as_string();
            }
            if (auto* uc = h.get<f4::entities::UnitCoreComponent>()) {
                if (!uc->class_name.empty()) return uc->class_name;
            }
            return {};
        };

        int brigades = 0;
        for (const auto& eid : impl_->units()) {
            auto h = impl_->handle(eid);
            auto* uc = h.get<f4::entities::UnitCoreComponent>();
            if (!uc || uc->unit_class != f4::entities::UnitClass::Brigade) {
                continue;
            }
            auto* hier = h.get<f4::entities::HierarchyComponent>();
            if (!hier || hier->children.empty()) continue;
            ++brigades;

            const std::string label = unit_display_name(eid);
            if (ImGui::TreeNode(reinterpret_cast<const void*>(eid.value),
                                "%s", label.empty() ? "(brigade)"
                                                    : label.c_str())) {
                for (const auto child : hier->children) {
                    const std::string bn = unit_display_name(child);
                    ImGui::BulletText("%s",
                                      bn.empty() ? "(battalion)"
                                                 : bn.c_str());
                }
                ImGui::TreePop();
            }
        }
        if (brigades == 0) {
            ImGui::TextDisabled(
                "(no brigade structure in this world)");
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
}

} // namespace f4::viewer
