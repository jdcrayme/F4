// f4-world-viewer/src/campaign_qc_view.cpp
//
// The "ATO / Tasking" window — the campaign-side half of the end-to-end QC
// loop (user directive: "how all this data should render in the world
// viewer. Flights, missions etc. — the primary QC for campaign logic").
//
// What it shows, top to bottom:
//   * World tasking summary (flights, tasked, packages, teams at war).
//   * Filters: mission-type combo (with live counts) + team combo (the
//     same team_filter the canvas uses) — the canvas, mission links and
//     this table all follow BOTH filters, so the map and the ATO agree.
//   * The ATO table itself: one row per tasked flight, sorted by TOT.
//     Columns: callsign, mission (name + category), team, package, TOT,
//     target (name; click selects + pans), squadron (name), route
//     waypoints count. Clicking a row selects the flight entity and
//     centers the camera on it — the inspector then shows its full plan.
//
// Row model is rebuilt only when the filters change (dirty flag), so the
// per-frame cost of a 449-flight ATO is an ImGui clipper walk, not 449
// component queries.
//
// Companion canvas overlays (canvas.cpp): mission-target links, package
// element links, the bullseye, and mission-filter dimming.

#include "viewer_state.hpp"
#include <f4/viewer/enum_text.hpp>

#include <f4/campaign/mission_type.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace f4::viewer {

namespace {

// One ATO row. POD, rebuilt on filter changes.
struct AtoRow {
    f4::entities::EntityId eid;
    f4::entities::EntityId target;        // resolved mission target (may be invalid)
    f4::entities::EntityId package;       // parent package (may be invalid)
    f4::entities::EntityId squadron;      // owning squadron (may be invalid)
    int32_t tot = 0;                      // absolute CampaignTime
    uint8_t mission = 0;
    uint8_t callsign_id = 0;
    uint8_t callsign_num = 0;
    int wp_count = 0;
    float gx = 0.0f, gy = 0.0f;           // grid position (camera focus)
    uint8_t owner = 0;
};

} // namespace

void ViewerApp::draw_campaign_qc_view() {
    if (!impl_->world_loaded || !impl_->show_ato) return;

    // Resolve an entity's display name: the NAME tag (class name) when
    // present, UnitCoreComponent::class_name for units, else empty. A
    // lambda because it needs ViewerApp::Impl (a private nested type the
    // file-scope helpers can't name).
    const auto entity_display_name =
        [this](f4::entities::EntityId eid) -> std::string {
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
        if (auto* ot = h.get<f4::entities::ObjectiveTypeComponent>()) {
            if (!ot->class_name.empty()) return ot->class_name;
        }
        return {};
    };

    // --- Row cache (rebuilt when the filters change) --------------------
    // Rebuild trigger: the filters (mission_filter, team_filter) or the
    // world itself changed. A generation counter guards the latter: any
    // world load bumps it (file_ops resets the filters).
    static std::vector<AtoRow> rows;
    static int cached_mission = -2;   // -2 = never built
    static uint8_t cached_team = 0xFE;
    static bool cached_open = false;
    static uint64_t world_stamp = 0;

    // Any world mutation invalidates: EntityWorld size is a cheap stamp.
    const auto stamp = impl_->eworld.size();
    const bool dirty = cached_mission != impl_->mission_filter ||
                       cached_team != impl_->team_filter ||
                       !cached_open ||
                       world_stamp != stamp;
    cached_mission = impl_->mission_filter;
    cached_team = impl_->team_filter;
    cached_open = impl_->show_ato;
    world_stamp = stamp;

    if (dirty) {
        rows.clear();
        for (const auto& eid : impl_->units()) {
            auto h = impl_->handle(eid);
            auto* uc = h.get<f4::entities::UnitCoreComponent>();
            auto* fp = h.get<f4::entities::FlightPlanComponent>();
            auto* tr = h.get<f4::entities::TransformComponent>();
            if (!uc || !fp || !tr) continue;
            if (fp->mission == 0) continue;  // untasked: not in the ATO

            // Both filters apply (the canvas uses the same pair).
            auto team_tag = h.get_tag(f4::entities::tags::TEAM);
            const uint8_t owner = (team_tag && team_tag->as_int())
                ? static_cast<uint8_t>(*team_tag->as_int()) : 0;
            if (impl_->team_filter != 0xFF && owner != impl_->team_filter) {
                continue;
            }
            if (impl_->mission_filter >= 0 &&
                static_cast<int>(fp->mission) != impl_->mission_filter) {
                continue;
            }

            AtoRow row;
            row.eid = eid;
            row.target = fp->target;
            row.package = fp->package;
            row.squadron = fp->squadron;
            row.tot = fp->time_on_target;
            row.mission = fp->mission;
            row.callsign_id = fp->callsign_id;
            row.callsign_num = fp->callsign_num;
            if (auto* wp = h.get<f4::entities::WaypointPlanComponent>()) {
                row.wp_count = static_cast<int>(wp->waypoints.size());
            }
            row.gx = impl_->grid_x(tr);
            row.gy = impl_->grid_y(tr);
            row.owner = owner;
            rows.push_back(row);
        }
        // TOT order — the natural ATO reading order.
        std::sort(rows.begin(), rows.end(),
                  [](const AtoRow& a, const AtoRow& b) {
                      return a.tot < b.tot;
                  });
    }

    // --- Mission-type histogram (for the combo labels) ------------------
    static std::vector<std::pair<uint8_t, int>> mission_counts;
    static uint64_t counts_stamp = 0;
    if (counts_stamp != world_stamp) {
        counts_stamp = world_stamp;
        mission_counts.clear();
        int counts[f4::campaign::kMissionTypeCount] = {};
        for (const auto& eid : impl_->units()) {
            auto h = impl_->handle(eid);
            auto* fp = h.get<f4::entities::FlightPlanComponent>();
            if (fp && fp->mission < f4::campaign::kMissionTypeCount) {
                ++counts[fp->mission];
            }
        }
        for (int b = 1; b < static_cast<int>(f4::campaign::kMissionTypeCount);
             ++b) {
            if (counts[b] > 0) {
                mission_counts.emplace_back(static_cast<uint8_t>(b), counts[b]);
            }
        }
    }

    // --- Window ----------------------------------------------------------
    ImGui::SetNextWindowPos(ImVec2(30, 470), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 330), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ATO / Tasking", &impl_->show_ato,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // Summary line.
    {
        int packages = 0;
        for (const auto& eid : impl_->units()) {
            auto h = impl_->handle(eid);
            if (h.get<f4::entities::PackageSupportComponent>()) ++packages;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "flights %zu   tasked ATO rows %zu   packages %d   "
                      "teams %zu",
                      impl_->units().size(), rows.size(), packages,
                      impl_->teams().size());
        ImGui::TextUnformatted(buf);
    }

    // Filters row: mission combo + team combo + clear button.
    if (ImGui::BeginTable("##ato_filters", 3,
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("mission", impl_->mission_filter < 0
                                  ? "All missions"
                                  : std::string(
                                        f4::campaign::mission_type_name(
                                            static_cast<uint8_t>(
                                                impl_->mission_filter)))
                                        .c_str())) {
            if (ImGui::Selectable("All missions",
                                  impl_->mission_filter < 0)) {
                impl_->mission_filter = -1;
            }
            for (const auto& [byte, count] : mission_counts) {
                char label[64];
                std::snprintf(label, sizeof(label), "%s (%d)",
                              f4::campaign::mission_type_name(byte).data(),
                              count);
                if (ImGui::Selectable(
                        label,
                        impl_->mission_filter == static_cast<int>(byte))) {
                    impl_->mission_filter = static_cast<int>(byte);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("team", impl_->team_filter == 0xFF
                                  ? "All teams"
                                  : impl_->team_name_for_slot(
                                        impl_->team_filter))) {
            if (ImGui::Selectable("All teams",
                                  impl_->team_filter == 0xFF)) {
                impl_->team_filter = 0xFF;
            }
            for (std::size_t i = 0; i < impl_->teams().size(); ++i) {
                auto h = impl_->handle(impl_->teams()[i]);
                auto* cid = h.get<f4::entities::CampaignIdentityComponent>();
                const char* nm = (cid && !cid->callsign.empty())
                    ? cid->callsign.c_str() : "(empty)";
                if (ImGui::Selectable(nm,
                        impl_->team_filter == static_cast<uint8_t>(i))) {
                    impl_->team_filter = static_cast<uint8_t>(i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextColumn();
        if (ImGui::Button("Reset filters")) {
            impl_->mission_filter = -1;
            impl_->team_filter = 0xFF;
        }
        ImGui::EndTable();
    }

    ImGui::Separator();

    // --- The ATO table ---------------------------------------------------
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuterH |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Sortable;
    if (ImGui::BeginTable("ato", 8, table_flags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("callsign", ImGuiTableColumnFlags_WidthFixed,
                                84.0f, 0);
        ImGui::TableSetupColumn("mission", ImGuiTableColumnFlags_WidthFixed,
                                150.0f, 1);
        ImGui::TableSetupColumn("team", ImGuiTableColumnFlags_WidthFixed,
                                56.0f, 2);
        ImGui::TableSetupColumn("pkg", ImGuiTableColumnFlags_WidthFixed,
                                64.0f, 3);
        ImGui::TableSetupColumn("TOT", ImGuiTableColumnFlags_WidthFixed,
                                100.0f, 4);
        ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthStretch,
                                160.0f, 5);
        ImGui::TableSetupColumn("squadron", ImGuiTableColumnFlags_WidthStretch,
                                140.0f, 6);
        ImGui::TableSetupColumn("wps", ImGuiTableColumnFlags_WidthFixed,
                                32.0f, 7);
        ImGui::TableHeadersRow();

        // Simple in-place sort when the user clicks a header (stable, on
        // the cached rows — no re-query).
        if (ImGuiTableSortSpecs* specs =
                ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty) {
                const auto col = specs->Specs[0].ColumnUserID;
                const auto dir = specs->Specs[0].SortDirection;
                std::stable_sort(rows.begin(), rows.end(),
                    [col, dir](const AtoRow& a, const AtoRow& b) {
                        bool lt;
                        switch (col) {
                            case 0: lt = a.callsign_id < b.callsign_id; break;
                            case 1: lt = a.mission < b.mission; break;
                            case 2: lt = a.owner < b.owner; break;
                            case 4: lt = a.tot < b.tot; break;
                            case 7: lt = a.wp_count < b.wp_count; break;
                            default: lt = a.eid.value < b.eid.value; break;
                        }
                        return dir == ImGuiSortDirection_Ascending ? lt : !lt;
                    });
                specs->SpecsDirty = false;
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& row = rows[static_cast<std::size_t>(i)];
                const bool selected =
                    impl_->sel_kind == Impl::SelectionKind::Unit &&
                    impl_->sel_entity == row.eid;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                // Whole-row selectable: clicking selects the flight and
                // pans the camera onto it.
                char cs[24];
                std::snprintf(cs, sizeof(cs), "CS%03u-%u", row.callsign_id,
                              row.callsign_num);
                if (ImGui::Selectable(cs, selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    impl_->sel_kind = Impl::SelectionKind::Unit;
                    impl_->sel_entity = row.eid;
                    impl_->cam_x = row.gx;
                    impl_->cam_y = row.gy;
                    impl_->cam_zoom =
                        std::max(impl_->cam_zoom, 2.0f);
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    f4::campaign::mission_type_name(row.mission).data());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(impl_->team_name_for_slot(row.owner));

                ImGui::TableNextColumn();
                if (row.package.valid()) {
                    auto pb = Impl::pb_int(
                        impl_->handle(row.package)
                            .get<f4::entities::PropertyBag>(),
                        "vu_id_num", 0);
                    ImGui::Text("%llu",
                                static_cast<unsigned long long>(pb));
                } else {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableNextColumn();
                {
                    char tbuf[24];
                    f4::viewer::format_campaign_time(row.tot, tbuf,
                                                     sizeof(tbuf));
                    ImGui::TextUnformatted(tbuf);
                }

                ImGui::TableNextColumn();
                {
                    const std::string name =
                        entity_display_name(row.target);
                    if (row.target.valid() && !name.empty()) {
                        // Clicking the target name selects the TARGET.
                        if (ImGui::Selectable(name.c_str(), false,
                                              ImGuiSelectableFlags_None)) {
                            impl_->sel_kind =
                                Impl::SelectionKind::Objective;
                            impl_->sel_entity = row.target;
                            auto th = impl_->handle(row.target);
                            if (auto* ttr =
                                    th.get<f4::entities::
                                               TransformComponent>()) {
                                impl_->cam_x = impl_->grid_x(ttr);
                                impl_->cam_y = impl_->grid_y(ttr);
                                impl_->cam_zoom =
                                    std::max(impl_->cam_zoom, 4.0f);
                            }
                        }
                    } else if (row.target.valid()) {
                        ImGui::TextUnformatted("(objective)");
                    } else {
                        ImGui::TextDisabled("-");
                    }
                }

                ImGui::TableNextColumn();
                {
                    const std::string name =
                        entity_display_name(row.squadron);
                    if (!name.empty()) {
                        ImGui::TextUnformatted(name.c_str());
                    } else if (row.squadron.valid()) {
                        ImGui::TextUnformatted("(sqdn)");
                    } else {
                        ImGui::TextDisabled("-");
                    }
                }

                ImGui::TableNextColumn();
                ImGui::Text("%d", row.wp_count);
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace f4::viewer
