// f4-world-viewer/src/inspector_panel.cpp
//
// ViewerApp::draw_inspector — the right-side Inspector panel showing
// the full detail of the currently-selected objective or unit
// (position, owner, class name, links, features, vehicle groups,
// waypoints, mission context, ground layout summary, etc.).
//
// Migrated from WorldState to EntityWorld (Step 4c).

#include "viewer_state.hpp"
#include "diagnostics.hpp"

#include <f4/terrain/terrain_data.hpp>
#include <f4/viewer/enum_text.hpp>
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
        ImGui::SetNextWindowPos(ImVec2(impl_->window_w - 320, 250), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(310, 380), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoCollapse)) {
            if (impl_->sel_kind == Impl::SelectionKind::None || !impl_->sel_entity.valid()) {
                ImGui::TextDisabled("Nothing selected");
                ImGui::TextDisabled("Click an objective or unit to inspect.");
            } else if (impl_->sel_kind == Impl::SelectionKind::Objective) {
                auto h = impl_->handle(impl_->sel_entity);
                auto* tr = h.get<f4::entities::TransformComponent>();
                auto* own = h.get<f4::entities::OwnershipComponent>();
                auto* ot = h.get<f4::entities::ObjectiveTypeComponent>();
                auto* pri = h.get<f4::entities::ObjectivePriorityComponent>();
                auto* pb = h.get<f4::entities::PropertyBag>();
                if (!tr || !own || !ot) {
                    ImGui::TextDisabled("Invalid entity");
                } else {
                const float gx = impl_->grid_x(tr), gy = impl_->grid_y(tr);
                const char* team_name = impl_->world_loaded
                    ? impl_->team_name_for_slot(own->team) : "(no world)";
                const uint8_t obj_type = impl_->obj_type_from_pb(pb);
                const std::string obj_type_name_str =
                    (obj_type > 0)
                        ? f4::world_convert::objective_type_name(
                              static_cast<int16_t>(obj_type))
                        : std::string("Unknown");
                ImGui::Text("Objective");
                ImGui::Separator();
                if (!ot->class_name.empty()) {
                    ImGui::Text("Name:      %s", ot->class_name.c_str());
                }
                ImGui::Text("Type:      %s (%d)", obj_type_name_str.c_str(), obj_type);
                ImGui::Text("Entity:    %d", ot->type);
                ImGui::Text("Position:  (%.0f, %.0f, %.0f ft)", gx, gy, tr->position.z);
                ImGui::Text("Owner:     %d (%s)", own->team, team_name);
                if (pri) {
                    ImGui::Text("Priority:  %d", pri->priority);
                    ImGui::Text("Name ID:   %d", pri->nameid);
                }
                ImGui::Text("Camp ID:   %d", static_cast<int>(impl_->pb_int(pb, "camp_id")));
                ImGui::Text("First own: %d (%s)", own->first_owner,
                            f4::viewer::control_name(own->first_owner));
                // parent_id
                {
                    const char* parent_label = "(none)";
                    std::string parent_buf;
                    const uint32_t parent_id = pri ? pri->parent_id : 0;
                    if (parent_id != 0) {
                        auto it = impl_->pop.objective_id_map.find(parent_id);
                        if (it != impl_->pop.objective_id_map.end()) {
                            auto ph = impl_->handle(it->second);
                            auto* p_ot = ph.get<f4::entities::ObjectiveTypeComponent>();
                            if (p_ot && !p_ot->class_name.empty()) {
                                parent_buf = p_ot->class_name;
                            } else {
                                parent_buf = "objective";
                            }
                            parent_label = parent_buf.c_str();
                        }
                    }
                    ImGui::Text("Parent ID: 0x%08x  %s", parent_id, parent_label);
                }
                // Network links
                {
                    auto* nl = h.get<f4::entities::NetworkLinksComponent>();
                    ImGui::Text("Links:     %d (road/rail)", nl ? static_cast<int>(nl->links.size()) : 0);
                }
                ImGui::Text("VU_ID:     0x%08x/0x%08x",
                    static_cast<uint32_t>(impl_->pb_int(pb, "vu_id_creator")),
                    static_cast<uint32_t>(impl_->pb_int(pb, "vu_id_num")));
                ImGui::Separator();
                // Supply
                {
                    auto* sup = h.get<f4::entities::SupplyStateComponent>();
                    ImGui::Text("Supply:    %d", sup ? sup->supply : 0);
                    ImGui::Text("Fuel:      %d", sup ? sup->fuel : 0);
                    ImGui::Text("Losses:    %d", sup ? sup->losses : 0);
                    ImGui::Text("Last rep:  %d", sup ? sup->last_repair : 0);
                }
                // obj_flags
                if (pri) {
                    char flag_buf[128];
                    f4::viewer::obj_flags_text(pri->obj_flags, flag_buf, sizeof(flag_buf));
                    ImGui::Text("Obj flags: 0x%08x (%s)", pri->obj_flags, flag_buf);
                }
                // FeatureSet
                {
                    auto* fs = h.get<f4::entities::FeatureSetComponent>();
                    if (fs && (fs->features_count > 0 || fs->deag_distance > 0 || fs->pt_data_index > 0)) {
                        ImGui::Separator();
                        ImGui::TextUnformatted("Objective class data (OCD):");
                        ImGui::Text("Features:  %d", fs->features_count);
                        ImGui::Text("Deag dist: %d", fs->deag_distance);
                        ImGui::Text("Radar feat:%d", fs->radar_feature);
                        ImGui::Text("PT index:  %d", fs->pt_data_index);
                    }
                }
                // Radar
                {
                    auto* rad = h.get<f4::entities::RadarComponent>();
                    if (rad) {
                        ImGui::Separator();
                        if (rad->range_km > 0.0f || !rad->name.empty()) {
                            ImGui::Text("Radar: %s", rad->name.empty() ? "(unknown)" : rad->name.c_str());
                            ImGui::Text("  range: %.1f km", rad->range_km);
                            if (rad->radar_type_idx >= 0) {
                                ImGui::Text("  type idx: %d", static_cast<int>(rad->radar_type_idx));
                            }
                        }
                        ImGui::TextUnformatted("Detection arcs:");
                        for (int i = 0; i < 8; ++i) {
                            ImGui::Text("  arc %d: %.3f", i, rad->detect_ratio[i]);
                        }
                    }
                }
                // Ground layout
                {
                    auto* gl = h.get<f4::entities::GroundLayoutComponent>();
                    if (gl && !gl->layouts.empty()) {
                        ImGui::Separator();
                        if (ImGui::TreeNode("Ground Layout", "Ground Layout (%d lists)", static_cast<int>(gl->layouts.size()))) {
                            for (std::size_t li = 0; li < gl->layouts.size(); ++li) {
                                const auto& layout = gl->layouts[li];
                                const char* type_str =
                                    f4::world_convert::point_list_type_name(layout.type);
                                char label[96];
                                std::snprintf(label, sizeof(label), "[%zu] %s (runway %d, %d pts, %.0f deg)",
                                              li, type_str, layout.runway_num,
                                              static_cast<int>(layout.points.size()), layout.heading_deg);
                                if (ImGui::TreeNode(label)) {
                                    ImGui::Text("  type: %d (%s)  count: %d  ltrt: %d (%s)",
                                                layout.type, type_str, layout.count, layout.ltrt,
                                                f4::viewer::ltrt_name(layout.ltrt));
                                    int pi = 0;
                                    for (const auto& pt : layout.points) {
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
                }
                }
            } else if (impl_->sel_kind == Impl::SelectionKind::Unit) {
                auto h = impl_->handle(impl_->sel_entity);
                auto* tr = h.get<f4::entities::TransformComponent>();
                auto* uc = h.get<f4::entities::UnitCoreComponent>();
                auto* pb = h.get<f4::entities::PropertyBag>();
                if (!tr || !uc) {
                    ImGui::TextDisabled("Invalid entity");
                } else {
                const float ux = impl_->grid_x(tr), uy = impl_->grid_y(tr);
                auto team_tag = h.get_tag(f4::entities::tags::TEAM);
                const uint8_t owner = (team_tag && team_tag->as_int()) ? static_cast<uint8_t>(*team_tag->as_int()) : 0;
                const char* team_name = impl_->world_loaded
                    ? impl_->team_name_for_slot(owner) : "(no world)";
                const char* subtype_str = f4::world_convert::unit_subtype_name(
                    uc->domain, uc->unit_subtype);
                ImGui::Text("Unit");
                ImGui::Separator();
                if (!uc->class_name.empty()) {
                    ImGui::Text("Name:      %s", uc->class_name.c_str());
                }
                ImGui::Text("Class:     %s (%s)",
                            f4::entities::unit_class_name(uc->unit_class),
                            subtype_str);
                ImGui::Text("Type:      %d", uc->class_table_index);
                ImGui::Text("Subtype:   %d (%s)", uc->unit_subtype, subtype_str);
                ImGui::Text("Domain:    %d (%s)", uc->domain,
                            f4::viewer::domain_name(uc->domain));
                ImGui::Text("Position:  (%.0f, %.0f, %.0f ft)", ux, uy, tr->position.z);
                ImGui::Text("Owner:     %d (%s)", owner, team_name);
                // Movement orders (promoted from PropertyBag to MovementOrdersComponent)
                if (auto* mo = h.get<f4::entities::MovementOrdersComponent>()) {
                    ImGui::Text("Destination:(%d, %d)",
                                static_cast<int>(mo->dest_x),
                                static_cast<int>(mo->dest_y));
                    if (mo->movement_type > 0 || mo->movement_speed > 0) {
                        ImGui::Separator();
                        ImGui::TextUnformatted("Movement (UCD):");
                        if (!mo->movement_type_name.empty()) {
                            ImGui::Text("  Type:     %s (%d)",
                                        mo->movement_type_name.c_str(),
                                        mo->movement_type);
                        } else {
                            ImGui::Text("  Type:     %d", mo->movement_type);
                        }
                        ImGui::Text("  Speed:    %d", mo->movement_speed);
                        ImGui::Text("  Range:    %d km", static_cast<int>(mo->max_range));
                    }
                } else {
                    ImGui::TextDisabled("Destination: (no movement orders)");
                }
                ImGui::Text("Name ID:   %d", static_cast<int>(impl_->pb_int(pb, "name_id")));
                ImGui::Text("Camp ID:   %d", static_cast<int>(impl_->pb_int(pb, "camp_id")));
                ImGui::Text("Reinforc.: %d", static_cast<int>(impl_->pb_int(pb, "reinforcement")));
                ImGui::Text("Waypoints: %d", static_cast<int>(impl_->pb_int(pb, "wp_count")));
                ImGui::Text("Losses:    %d", static_cast<int>(impl_->pb_int(pb, "losses")));
                // Roster
                {
                    int total_vehicles = 0;
                    for (int vg = 0; vg < 16; ++vg) {
                        total_vehicles += (uc->roster >> (vg * 2)) & 0x03;
                    }
                    ImGui::Text("Roster:    0x%08x (%d vehicles)", uc->roster, total_vehicles);
                }
                // Vehicle composition
                {
                    auto* vc = h.get<f4::entities::VehicleCompositionComponent>();
                    if (vc && !vc->groups.empty()) {
                        if (ImGui::TreeNode("Vehicle Groups", "Vehicle Groups (%d)", static_cast<int>(vc->groups.size()))) {
                            ImGui::Text("grp  type  count  live  name         HP   speed");
                            int nominal_total = 0;
                            int live_total = 0;
                            for (const auto& vg : vc->groups) {
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
                }
                ImGui::Text("Entity:    %d", static_cast<int>(impl_->pb_int(pb, "entity_type")));
                ImGui::Text("VU_ID:     0x%08x/0x%08x",
                    static_cast<uint32_t>(impl_->pb_int(pb, "vu_id_creator")),
                    static_cast<uint32_t>(impl_->pb_int(pb, "vu_id_num")));
                // Subclass-specific fields:
                ImGui::Separator();
                switch (uc->unit_class) {
                    case f4::entities::UnitClass::Battalion: {
                        auto* gt = h.get<f4::entities::GroundTacticalComponent>();
                        if (gt) {
                            ImGui::Text("Supply:    %d%%", gt->supply);
                            ImGui::Text("Morale:    %d%%", gt->morale);
                            ImGui::Text("Fatigue:   %d%%", gt->fatigue);
                            ImGui::Text("Heading:   %d deg", static_cast<int>(gt->heading * 360 / 256));
                            ImGui::Text("Final hdg: %d deg", static_cast<int>(gt->final_heading * 360 / 256));
                            ImGui::Text("Last move: %d", gt->last_move);
                            ImGui::Text("Last cmbt: %d", gt->last_combat);
                        }
                        {
                            auto* hier = h.get<f4::entities::HierarchyComponent>();
                            const char* parent_label = "(none)";
                            std::string parent_buf;
                            // The raw parent_id VU_ID was removed from HierarchyComponent;
                            // use the resolved parent EntityId directly.
                            if (hier && hier->parent.valid()) {
                                auto ph = impl_->handle(hier->parent);
                                auto* p_uc = ph.get<f4::entities::UnitCoreComponent>();
                                if (p_uc && !p_uc->class_name.empty()) {
                                    parent_buf = p_uc->class_name;
                                } else {
                                    parent_buf = "unit";
                                }
                                parent_label = parent_buf.c_str();
                            }
                            // Display the resolved EntityId (slot:generation) instead of
                            // the raw VU_ID, which is no longer stored on the component.
                            const uint64_t parent_raw = hier ? hier->parent.value : 0;
                            ImGui::Text("Parent:    eid:%016llx  %s",
                                        static_cast<unsigned long long>(parent_raw),
                                        parent_label);
                        }
                        break;
                    }
                    case f4::entities::UnitClass::Brigade: {
                        auto* gt = h.get<f4::entities::GroundTacticalComponent>();
                        if (gt) {
                            ImGui::Text("Supply:    %d%%", gt->supply);
                            ImGui::Text("Morale:    %d%%", gt->morale);
                            ImGui::Text("Fatigue:   %d%%", gt->fatigue);
                        }
                        {
                            auto* hier = h.get<f4::entities::HierarchyComponent>();
                            // The raw element_ids VU_ID vector was removed;
                            // use the resolved children EntityIds.
                            ImGui::Text("Elements:  %d", hier ? static_cast<int>(hier->children.size()) : 0);
                            if (hier && ImGui::TreeNode("Child battalions")) {
                                for (const auto& child_eid : hier->children) {
                                    const char* child_label = "(missing)";
                                    std::string child_buf;
                                    if (child_eid.valid()) {
                                        auto ch = impl_->handle(child_eid);
                                        auto* c_uc = ch.get<f4::entities::UnitCoreComponent>();
                                        if (c_uc && !c_uc->class_name.empty()) {
                                            child_buf = c_uc->class_name;
                                        } else {
                                            child_buf = "unit";
                                        }
                                        child_label = child_buf.c_str();
                                    }
                                    ImGui::Text("  eid:%016llx  %s",
                                                static_cast<unsigned long long>(child_eid.value),
                                                child_label);
                                }
                                ImGui::TreePop();
                            }
                        }
                        break;
                    }
                    case f4::entities::UnitClass::Squadron: {
                        auto* sq = h.get<f4::entities::SquadronComponent>();
                        if (sq) {
                            ImGui::Text("Fuel:      %d lbs", sq->fuel);
                            {
                                const char* ab_label = "(none)";
                                std::string ab_buf;
                                // The raw airbase_id VU_ID was removed from SquadronComponent;
                                // use the resolved airbase EntityId directly.
                                if (sq->airbase.valid()) {
                                    auto ah = impl_->handle(sq->airbase);
                                    auto* a_ot = ah.get<f4::entities::ObjectiveTypeComponent>();
                                    if (a_ot && !a_ot->class_name.empty()) {
                                        ab_buf = a_ot->class_name;
                                    } else {
                                        ab_buf = "objective";
                                    }
                                    ab_label = ab_buf.c_str();
                                }
                                ImGui::Text("Airbase:   eid:%016llx  %s",
                                            static_cast<unsigned long long>(sq->airbase.value),
                                            ab_label);
                            }
                            ImGui::Text("Specialty: %d (%s)", sq->specialty,
                                        f4::viewer::squadron_specialty_name(sq->specialty));
                            ImGui::Separator();
                            ImGui::Text("AA kills:  %d", sq->aa_kills);
                            ImGui::Text("AG kills:  %d", sq->ag_kills);
                            ImGui::Text("AS kills:  %d", sq->as_kills);
                            ImGui::Text("AN kills:  %d", sq->an_kills);
                            ImGui::Text("Missions:  %d", sq->missions_flown);
                            ImGui::Text("Score:     %d", sq->mission_score);
                            ImGui::Text("Total loss:%d", sq->total_losses);
                            ImGui::Text("Pilot loss:%d", sq->pilot_losses);
                            ImGui::Text("Patch:     %d", sq->squadron_patch);
                            if (ImGui::TreeNode("Pilots", "Pilots (%d)", static_cast<int>(sq->pilots.size()))) {
                                ImGui::Text("ID    Skill        Rating     Status  AA  AG  Missions");
                                for (const auto& p : sq->pilots) {
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
                        }
                        break;
                    }
                    case f4::entities::UnitClass::TaskForce: {
                        auto* gt = h.get<f4::entities::GroundTacticalComponent>();
                        if (gt) {
                            ImGui::Text("Supply:    %d%%", gt->supply);
                        }
                        break;
                    }
                    case f4::entities::UnitClass::Flight:
                    case f4::entities::UnitClass::Package:
                    case f4::entities::UnitClass::Unknown:
                        break;
                }
                // Waypoint list
                {
                    auto* wp = h.get<f4::entities::WaypointPlanComponent>();
                    if (wp && !wp->waypoints.empty()) {
                        ImGui::Separator();
                        if (ImGui::TreeNode("Waypoints", "Waypoints (%d)", static_cast<int>(wp->waypoints.size()))) {
                            ImGui::Text("idx  x    y    z    action              depart");
                            int wi = 0;
                            for (const auto& w : wp->waypoints) {
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
            }
        }
        ImGui::End();
}

} // namespace f4::viewer
