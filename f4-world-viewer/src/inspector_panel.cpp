// f4-world-viewer/src/inspector_panel.cpp
//
// ViewerApp::draw_inspector — the right-side Inspector panel showing
// the full detail of the currently-selected objective or unit
// (position, owner, class name, links, features, vehicle groups,
// waypoints, mission context, ground layout summary, etc.).
//
// POLISH-2.6: extracted out of imgui_panels.cpp's draw_imgui() to
// keep that function focused on menu/layer/legend/modal plumbing.
// The inspector is the single largest panel (~360 LoC) and benefits
// most from isolation: changes to the inspector don't require re-
// reading the rest of draw_imgui, and the rest of draw_imgui doesn't
// have to scroll past 360 lines of inspector code to reach the modals.
//
// Called from draw_imgui() between the Legend panel and the Status bar.
// No behavior change vs. the inlined version — same ImGui calls in the
// same order.

#include "viewer_state.hpp"
#include "diagnostics.hpp"

#include <f4/terrain/terrain_data.hpp>
#include <f4/viewer/enum_text.hpp>
#include <f4/world/world_state.hpp>
#include <f4/world_convert/class_table.hpp>      // unit_subtype_name(), DOMAIN_*
#include <f4/world_convert/objective_decoder.hpp> // objective_type_name()
#include <f4/world_convert/theater_data.hpp>     // point_type_name(), point_list_type_name()

#include <imgui.h>

#include <cstdio>
#include <string>

namespace f4::viewer {

// ---------------------------------------------------------------------------
// Inspector panel
// ---------------------------------------------------------------------------
void ViewerApp::draw_inspector() {
        // --- Inspector (right side, below legend) ---
        ImGui::SetNextWindowPos(ImVec2(impl_->window_w - 320, 250), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(310, 380), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoCollapse)) {
            if (impl_->sel_kind == Impl::SelectionKind::None || impl_->sel_index < 0) {
                ImGui::TextDisabled("Nothing selected");
                ImGui::TextDisabled("Click an objective or unit to inspect.");
            } else if (impl_->sel_kind == Impl::SelectionKind::Objective) {
                const auto& o = impl_->world.objectives[impl_->sel_index];
                // Resolve the team name from WorldState.teams[] when available.
                // Falls back to "(unknown)" if no world loaded or owner out of range.
                const char* team_name = "(no world)";
                if (impl_->world_loaded && o.owner < impl_->world.teams.size()) {
                    const auto& t = impl_->world.teams[o.owner];
                    team_name = t.name.empty() ? "(empty)" : t.name.c_str();
                }
                // Resolve the objective type name (e.g. "Airbase"). Falls back to
                // "Unknown" when objective_type == 0 (no class table loaded).
                const std::string obj_type_name_str =
                    (o.objective_type > 0)
                        ? f4::world_convert::objective_type_name(
                              static_cast<int16_t>(o.objective_type))
                        : std::string("Unknown");
                ImGui::Text("Objective #%d", impl_->sel_index);
                ImGui::Separator();
                // Show the objective's class name (e.g. "02_20 Airbase 2") when
                // available — much more useful than just "Airbase". Falls back
                // to the objective_type name when no class_name was loaded.
                if (!o.class_name.empty()) {
                    ImGui::Text("Name:      %s", o.class_name.c_str());
                }
                ImGui::Text("Type:      %s (%d)", obj_type_name_str.c_str(), o.objective_type);
                ImGui::Text("Entity:    %d", o.type);
                ImGui::Text("Position:  (%d, %d, %.0f ft)", o.x, o.y, o.z);
                ImGui::Text("Owner:     %d (%s)", o.owner, team_name);
                ImGui::Text("Priority:  %d", o.priority);
                ImGui::Text("Camp ID:   %d", o.camp_id);
                ImGui::Text("Name ID:   %d", o.nameid);
                // first_owner uses the same Control enum as owner — decode it.
                ImGui::Text("First own: %d (%s)", o.first_owner,
                            f4::viewer::control_name(o.first_owner));
                // parent_id resolves to the parent objective's name when we can
                // find it in the VU_ID → objective index map.
                {
                    const char* parent_label = "(none)";
                    std::string parent_buf;
                    auto it = impl_->obj_id_to_index.find(o.parent_id);
                    if (o.parent_id != 0 &&
                        it != impl_->obj_id_to_index.end() &&
                        it->second < static_cast<int>(impl_->world.objectives.size())) {
                        const auto& par = impl_->world.objectives[it->second];
                        if (!par.class_name.empty()) {
                            parent_buf = par.class_name + " (#" +
                                         std::to_string(it->second) + ")";
                            parent_label = parent_buf.c_str();
                        } else {
                            parent_buf = "objective #" + std::to_string(it->second);
                            parent_label = parent_buf.c_str();
                        }
                    }
                    ImGui::Text("Parent ID: 0x%08x  %s", o.parent_id, parent_label);
                }
                ImGui::Text("Links:     %d (road/rail)", static_cast<int>(o.links.size()));
                ImGui::Text("VU_ID:     0x%08x/0x%08x", o.id_creator, o.id_num);
                ImGui::Separator();
                ImGui::Text("Supply:    %d", o.supply);
                ImGui::Text("Fuel:      %d", o.fuel);
                ImGui::Text("Losses:    %d", o.losses);
                ImGui::Text("Last rep:  %d", o.last_repair);
                // obj_flags is a bitmap — decode the well-known bits.
                {
                    char flag_buf[128];
                    f4::viewer::obj_flags_text(o.obj_flags, flag_buf, sizeof(flag_buf));
                    ImGui::Text("Obj flags: 0x%08x (%s)", o.obj_flags, flag_buf);
                }
                // Theater static-data enrichment (from Falcon4.OCD):
                if (o.features_count > 0 || o.deag_distance > 0 || o.pt_data_index > 0) {
                    ImGui::Separator();
                    ImGui::TextUnformatted("Objective class data (OCD):");
                    ImGui::Text("Features:  %d", o.features_count);
                    ImGui::Text("Deag dist: %d", o.deag_distance);
                    ImGui::Text("Radar feat:%d", o.radar_feature);
                    ImGui::Text("PT index:  %d", o.pt_data_index);
                }
                if (o.has_radar) {
                    ImGui::Separator();
                    // Phase 3: show real radar name + range from RCD.
                    if (o.radar_range_km > 0.0f || !o.radar_name.empty()) {
                        ImGui::Text("Radar: %s", o.radar_name.empty() ? "(unknown)" : o.radar_name.c_str());
                        ImGui::Text("  range: %.1f km", o.radar_range_km);
                        if (o.radar_type_idx >= 0) {
                            ImGui::Text("  type idx: %d", static_cast<int>(o.radar_type_idx));
                        }
                    }
                    ImGui::TextUnformatted("Detection arcs:");
                    for (int i = 0; i < 8; ++i) {
                        ImGui::Text("  arc %d: %.3f", i, o.detect_ratio[i]);
                    }
                }
                // Airbase ground layout (from Falcon4.PHD/PD): show runway/
                // taxiway/parking lists with their points.
                if (!o.ground_layout.empty()) {
                    ImGui::Separator();
                    if (ImGui::TreeNode("Ground Layout", "Ground Layout (%d lists)", static_cast<int>(o.ground_layout.size()))) {
                        for (std::size_t li = 0; li < o.ground_layout.size(); ++li) {
                            const auto& gl = o.ground_layout[li];
                            // Use the proper PointListType decoder from
                            // f4-world-convert instead of the inline switch.
                            const char* type_str =
                                f4::world_convert::point_list_type_name(gl.type);
                            char label[96];
                            std::snprintf(label, sizeof(label), "[%zu] %s (runway %d, %d pts, %.0f deg)",
                                          li, type_str, gl.runway_num,
                                          static_cast<int>(gl.points.size()), gl.heading_deg);
                            if (ImGui::TreeNode(label)) {
                                // Decode ltrt: -1=Left, +1=Right, 0=Center.
                                ImGui::Text("  type: %d (%s)  count: %d  ltrt: %d (%s)",
                                            gl.type, type_str, gl.count, gl.ltrt,
                                            f4::viewer::ltrt_name(gl.ltrt));
                                int pi = 0;
                                for (const auto& pt : gl.points) {
                                    // Decode point type (1=Runway, 3=Taxi,
                                    // 11=SmallPark, ...) and point flags bitmap
                                    // (PT_FIRST / PT_LAST / PT_OCCUPIED).
                                    char flag_buf[32];
                                    f4::viewer::point_flags_text(
                                        pt.flags, flag_buf, sizeof(flag_buf));
                                    ImGui::Text("  pt %d: (%.0f, %.0f) type=%d (%s) flags=0x%02x (%s)",
                                                pi++, pt.x, pt.y, pt.type,
                                                f4::world_convert::point_type_name(pt.type),
                                                pt.flags, flag_buf);
                                }
                                ImGui::TreePop();
                            }
                        }
                        ImGui::TreePop();
                    }
                }
            } else if (impl_->sel_kind == Impl::SelectionKind::Unit) {
                const auto& u = impl_->world.units[impl_->sel_index];
                const char* team_name = "(no world)";
                if (impl_->world_loaded && u.owner < impl_->world.teams.size()) {
                    const auto& t = impl_->world.teams[u.owner];
                    team_name = t.name.empty() ? "(empty)" : t.name.c_str();
                }
                // Subtype name (e.g. "Armor", "Fighter-Bomber"). Uses the
                // domain+subtype pair emitted by the converter. Falls back to
                // "Unknown" when subtype == 0 (no class table loaded).
                const char* subtype_str = f4::world_convert::unit_subtype_name(
                    u.domain, u.unit_subtype);
                ImGui::Text("Unit #%d", impl_->sel_index);
                ImGui::Separator();
                // Show the unit's class name (e.g. "Patrol", "Armor Battalion")
                // when available — much more useful than just "battalion".
                if (!u.class_name.empty()) {
                    ImGui::Text("Name:      %s", u.class_name.c_str());
                }
                ImGui::Text("Class:     %s (%s)",
                            f4::world::unit_class_name(u.unit_class),
                            subtype_str);
                ImGui::Text("Type:      %d", u.type);
                ImGui::Text("Subtype:   %d (%s)", u.unit_subtype, subtype_str);
                // Domain: 2=Air, 3=Land, 4=Sea — decode so the user doesn't have
                // to keep the enum table in their head.
                ImGui::Text("Domain:    %d (%s)", u.domain,
                            f4::viewer::domain_name(u.domain));
                ImGui::Text("Position:  (%d, %d, %.0f ft)", u.x, u.y, u.z);
                ImGui::Text("Owner:     %d (%s)", u.owner, team_name);
                ImGui::Text("Destination:(%d, %d)", u.dest_x, u.dest_y);
                ImGui::Text("Name ID:   %d", u.name_id);
                ImGui::Text("Camp ID:   %d", u.camp_id);
                ImGui::Text("Reinforc.: %d", u.reinforcement);
                ImGui::Text("Waypoints: %d", u.wp_count);
                ImGui::Text("Losses:    %d", u.losses);
                // Movement specs (from Falcon4.UCD):
                if (u.movement_type > 0 || u.movement_speed > 0) {
                    ImGui::Separator();
                    ImGui::TextUnformatted("Movement (UCD):");
                    if (!u.movement_type_name.empty()) {
                        ImGui::Text("  Type:     %s (%d)", u.movement_type_name.c_str(), u.movement_type);
                    } else {
                        ImGui::Text("  Type:     %d", u.movement_type);
                    }
                    ImGui::Text("  Speed:    %d", u.movement_speed);
                    ImGui::Text("  Range:    %d km", u.max_range);
                }
                // Roster: 16 groups × 2 bits. Show live vehicle count per group.
                // GetNumVehicles(vg) = (roster >> (vg*2)) & 0x03 — max 3/group.
                {
                    int total_vehicles = 0;
                    for (int vg = 0; vg < 16; ++vg) {
                        total_vehicles += (u.roster >> (vg * 2)) & 0x03;
                    }
                    ImGui::Text("Roster:    0x%08x (%d vehicles)", u.roster, total_vehicles);
                }
                // Vehicle composition (from Falcon4.UCD + VCD): per-group vehicle
                // types and names, with live counts from the roster.
                if (!u.vehicle_groups.empty()) {
                    if (ImGui::TreeNode("Vehicle Groups", "Vehicle Groups (%d)", static_cast<int>(u.vehicle_groups.size()))) {
                        ImGui::Text("grp  type  count  live  name         HP   speed");
                        int nominal_total = 0;
                        int live_total = 0;
                        for (const auto& vg : u.vehicle_groups) {
                            ImGui::Text("%-4d %-5d %-6d %-5d %-12s %-4d %d",
                                        vg.group, vg.vehicle_type, vg.count,
                                        vg.live_count,
                                        vg.vehicle_name.empty() ? "?" : vg.vehicle_name.c_str(),
                                        vg.hit_points, vg.max_speed);
                            nominal_total += vg.count;
                            live_total += vg.live_count;
                        }
                        ImGui::Separator();
                        ImGui::Text("Total:     %d nominal, %d live", nominal_total, live_total);
                        ImGui::TreePop();
                    }
                }
                ImGui::Text("Entity:    %d", u.entity_type);
                ImGui::Text("VU_ID:     0x%08x/0x%08x", u.id_creator, u.id_num);
                // Subclass-specific fields:
                ImGui::Separator();
                switch (u.unit_class) {
                    case f4::world::UnitClass::Battalion:
                        ImGui::Text("Supply:    %d%%", u.supply);
                        ImGui::Text("Morale:    %d%%", u.morale);
                        ImGui::Text("Fatigue:   %d%%", u.fatigue);
                        ImGui::Text("Heading:   %d deg", static_cast<int>(u.heading * 360 / 256));
                        ImGui::Text("Final hdg: %d deg", static_cast<int>(u.final_heading * 360 / 256));
                        ImGui::Text("Last move: %d", u.last_move);
                        ImGui::Text("Last cmbt: %d", u.last_combat);
                        // Resolve parent_id (VU_ID.num of the brigade) to a
                        // unit name via the unit_id_to_index lookup table.
                        {
                            const char* parent_label = "(none)";
                            std::string parent_buf;
                            auto it = impl_->unit_id_to_index.find(u.parent_id);
                            if (u.parent_id != 0 &&
                                it != impl_->unit_id_to_index.end() &&
                                it->second < static_cast<int>(impl_->world.units.size())) {
                                const auto& par = impl_->world.units[it->second];
                                if (!par.class_name.empty()) {
                                    parent_buf = par.class_name + " (unit #" +
                                                 std::to_string(it->second) + ")";
                                } else {
                                    parent_buf = "unit #" + std::to_string(it->second);
                                }
                                parent_label = parent_buf.c_str();
                            }
                            ImGui::Text("Parent:    0x%08x  %s", u.parent_id, parent_label);
                        }
                        break;
                    case f4::world::UnitClass::Brigade:
                        ImGui::Text("Supply:    %d%%", u.supply);
                        ImGui::Text("Morale:    %d%%", u.morale);
                        ImGui::Text("Fatigue:   %d%%", u.fatigue);
                        ImGui::Text("Elements:  %d", u.elements);
                        if (ImGui::TreeNode("Child battalions")) {
                            for (uint32_t eid : u.element_ids) {
                                // Resolve each child battalion VU_ID.num to
                                // its name via the unit_id_to_index map.
                                const char* child_label = "(missing)";
                                std::string child_buf;
                                auto it = impl_->unit_id_to_index.find(eid);
                                if (it != impl_->unit_id_to_index.end() &&
                                    it->second < static_cast<int>(impl_->world.units.size())) {
                                    const auto& child = impl_->world.units[it->second];
                                    if (!child.class_name.empty()) {
                                        child_buf = child.class_name + " (unit #" +
                                                    std::to_string(it->second) + ")";
                                    } else {
                                        child_buf = "unit #" + std::to_string(it->second);
                                    }
                                    child_label = child_buf.c_str();
                                }
                                ImGui::Text("  ID 0x%08x  %s", eid, child_label);
                            }
                            ImGui::TreePop();
                        }
                        break;
                    case f4::world::UnitClass::Squadron:
                        ImGui::Text("Fuel:      %d lbs", u.fuel);
                        // Resolve airbase_id (VU_ID.num) to the airbase
                        // objective's name via the obj_id_to_index map.
                        {
                            const char* ab_label = "(none)";
                            std::string ab_buf;
                            auto it = impl_->obj_id_to_index.find(u.airbase_id);
                            if (u.airbase_id != 0 &&
                                it != impl_->obj_id_to_index.end() &&
                                it->second < static_cast<int>(impl_->world.objectives.size())) {
                                const auto& ab = impl_->world.objectives[it->second];
                                if (!ab.class_name.empty()) {
                                    ab_buf = ab.class_name + " (obj #" +
                                             std::to_string(it->second) + ")";
                                } else {
                                    ab_buf = "objective #" + std::to_string(it->second);
                                }
                                ab_label = ab_buf.c_str();
                            }
                            ImGui::Text("Airbase:   0x%08x  %s", u.airbase_id, ab_label);
                        }
                        ImGui::Text("Specialty: %d (%s)", u.specialty,
                                    f4::viewer::squadron_specialty_name(u.specialty));
                        ImGui::Separator();
                        ImGui::Text("AA kills:  %d", u.aa_kills);
                        ImGui::Text("AG kills:  %d", u.ag_kills);
                        ImGui::Text("AS kills:  %d", u.as_kills);
                        ImGui::Text("AN kills:  %d", u.an_kills);
                        ImGui::Text("Missions:  %d", u.missions_flown);
                        ImGui::Text("Score:     %d", u.mission_score);
                        ImGui::Text("Total loss:%d", u.total_losses);
                        ImGui::Text("Pilot loss:%d", u.pilot_losses);
                        ImGui::Text("Patch:     %d", u.squadron_patch);
                        if (ImGui::TreeNode("Pilots", "Pilots (%d)", static_cast<int>(u.pilots.size()))) {
                            ImGui::Text("ID    Skill        Rating     Status  AA  AG  Missions");
                            for (const auto& p : u.pilots) {
                                // Decode skill (0=Recruit..3=Ace) and rating
                                // (same enum) instead of bare ints.
                                char skill_buf[32];
                                std::snprintf(skill_buf, sizeof(skill_buf),
                                              "%d (%s)", p.skill,
                                              f4::viewer::pilot_skill_name(p.skill));
                                char rating_buf[32];
                                std::snprintf(rating_buf, sizeof(rating_buf),
                                              "%d (%s)", p.rating,
                                              f4::viewer::pilot_skill_name(p.rating));
                                ImGui::Text("%-5ld %-12s %-10s %-7s %-3d %-3d %d",
                                            static_cast<long>(p.pilot_id),
                                            skill_buf, rating_buf,
                                            f4::viewer::pilot_status_name(p.status),
                                            p.aa_kills, p.ag_kills, p.missions_flown);
                            }
                            ImGui::TreePop();
                        }
                        break;
                    case f4::world::UnitClass::TaskForce:
                        ImGui::Text("Supply:    %d%%", u.supply);
                        break;
                    case f4::world::UnitClass::Flight:
                    case f4::world::UnitClass::Package:
                    case f4::world::UnitClass::Unknown:
                        break;
                }
                // Waypoint list (only if non-empty)
                if (!u.waypoints.empty()) {
                    ImGui::Separator();
                    if (ImGui::TreeNode("Waypoints", "Waypoints (%d)", static_cast<int>(u.waypoints.size()))) {
                        ImGui::Text("idx  x    y    z    action              depart");
                        int wi = 0;
                        for (const auto& w : u.waypoints) {
                            // Decode WP_ACTION (Takeoff/Land/Strike/CAP/...)
                            char action_buf[40];
                            std::snprintf(action_buf, sizeof(action_buf),
                                          "%d (%s)", static_cast<int>(w.action),
                                          f4::viewer::wp_action_name(w.action));
                            ImGui::Text("%-4d %-4d %-4d %-4d %-19s %d",
                                        wi++, w.x, w.y, w.z,
                                        action_buf, w.depart);
                        }
                        ImGui::TreePop();
                    }
                }
            }
        }
        ImGui::End();
}

} // namespace f4::viewer
