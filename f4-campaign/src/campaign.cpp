// f4-campaign/src/campaign.cpp
//
// Implementation of the headless campaign engine — see campaign.hpp for
// the M4.7-skeleton semantics (cycle cadence, availability gates, TOT
// rule, determinism contract).

#include <f4/campaign/campaign.hpp>

#include <f4/json/f4_json.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace f4::campaign {

namespace {

// Squared nothing, magic nothing — the cycle arithmetic works in whole
// seconds; the mid-window TOT rounds half a minute up deterministically.
CampaignTime midpoint_tot(CampaignTime now, int min_time_min, int max_time_min) {
    const CampaignTime mid_min =
        (static_cast<CampaignTime>(min_time_min) + max_time_min) / 2;
    return now + mid_min * 60;
}

} // namespace

Campaign::Campaign(const f4::world::ICampaignSource& camp,
                   const f4::world::ITeamSource& teams,
                   const f4::world::IUnitCoreSource& units,
                   const MissionProfileTable& profiles,
                   f4::messaging::MessageBus& bus,
                   const CampaignConfig& cfg)
    : camp_(camp)
    , teams_(teams)
    , units_(units)
    , profiles_(profiles)
    , bus_(bus)
    , cfg_(cfg) {
    if (cfg_.air_task_cycle_sec <= 0) {
        throw std::runtime_error(
            "campaign: air_task_cycle_sec must be positive (got " +
            std::to_string(cfg_.air_task_cycle_sec) + ")");
    }

    // Snapshot the squadron roster once: the campaign tasking reads
    // squadron identity/ownership/specialty and tracks each squadron's
    // available aircraft. Snapshotting keeps tick() off the source
    // (the sources are read here and never mutate).
    pool_.assign(8, 0);
    const int n = units_.unit_count();
    for (int i = 0; i < n; ++i) {
        if (units_.unit_class(i) != f4::entities::UnitClass::Squadron) continue;
        const auto* sq = units_.as_squadron(i);
        if (!sq) continue;   // inconsistent source; skip defensively

        SquadronRef ref;
        ref.id_num = units_.id_num(i);
        ref.owner = units_.owner(i);
        ref.specialty = sq->specialty(i);
        ref.name = units_.class_name(i);
        // Availability: the squadron's own roster when the unit data
        // carries one; otherwise the campaign source's per-team pool is
        // shared across the team's squadrons at construction (each
        // squadron gets its draw immediately — deterministic because
        // the unit order is the wire order).
        const auto roster = units_.roster(i);
        ref.available = roster > 0 ? static_cast<int>(roster) : 0;
        squadrons_.push_back(std::move(ref));
    }

    // Seed the per-team pools from the campaign source (te_number_aircraft
    // is the TE block's per-team aircraft total; slot-indexed).
    const auto& team_pools = camp_.te_number_aircraft();
    for (int t = 0; t < teams_.team_count() && t < 8; ++t) {
        const int slot = teams_.slot(t);
        if (slot < 0 || slot >= 8) continue;
        if (static_cast<std::size_t>(slot) < team_pools.size()) {
            pool_[static_cast<std::size_t>(slot)] =
                team_pools[static_cast<std::size_t>(slot)];
        }
    }

    // Squadrons with their own roster (available > 0) keep it. Squadrons
    // without one share their team's pool evenly; the remainder stays in
    // the team pool (visible via team_aircraft_pool()).
    for (int t = 0; t < teams_.team_count() && t < 8; ++t) {
        const int slot = teams_.slot(t);
        if (slot < 0 || slot >= 8) continue;
        std::vector<SquadronRef*> shared_squadrons;
        for (auto& sq : squadrons_) {
            if (sq.owner == static_cast<std::uint8_t>(slot) && sq.available == 0) {
                shared_squadrons.push_back(&sq);
            }
        }
        if (shared_squadrons.empty()) continue;
        const int shared = pool_[static_cast<std::size_t>(slot)] /
                           static_cast<int>(shared_squadrons.size());
        for (auto* sq : shared_squadrons) {
            sq->available = shared;
        }
        pool_[static_cast<std::size_t>(slot)] -=
            shared * static_cast<int>(shared_squadrons.size());
    }
}

std::vector<int> Campaign::belligerent_teams() const {
    // Named slots first (same rule the sim-side team resolution uses —
    // unnamed/neutral slots are not belligerents).
    struct SlotTeam {
        int slot;
        const std::vector<int16_t>* stance;
    };
    std::vector<SlotTeam> named;
    for (int t = 0; t < teams_.team_count(); ++t) {
        if (teams_.name(t).empty()) continue;
        named.push_back({teams_.slot(t), &teams_.stance(t)});
    }

    std::vector<int> out;
    for (const auto& a : named) {
        const bool belligerent = std::any_of(
            named.begin(), named.end(), [&](const SlotTeam& b) {
                if (b.slot == a.slot) return false;
                const auto& st = *a.stance;
                return b.slot < static_cast<int>(st.size()) &&
                       st[static_cast<std::size_t>(b.slot)] < 0;
            });
        if (belligerent) out.push_back(a.slot);
    }
    std::sort(out.begin(), out.end());
    return out;
}

int Campaign::team_aircraft_pool_(int team_slot) const {
    if (team_slot < 0 || team_slot >= 8) return 0;
    return pool_[static_cast<std::size_t>(team_slot)];
}

void Campaign::tick(CampaignTime delta_sec) {
    if (delta_sec < 0) {
        throw std::runtime_error("campaign: negative tick delta (" +
                                 std::to_string(delta_sec) + ")");
    }
    clock_ += delta_sec;

    // Fire every cycle that has come due, in order. A delta spanning
    // several cycles fires them all — the same behavior as one big tick
    // and N small ones (asserted by the golden test).
    while (next_cycle_ + cfg_.air_task_cycle_sec <= clock_) {
        next_cycle_ += cfg_.air_task_cycle_sec;
        ++cycles_fired_;
        run_tasking_cycle_();
    }
}

void Campaign::run_tasking_cycle_() {
    const auto war_teams = belligerent_teams();

    for (const int slot : war_teams) {
        // The team's squadrons, wire order (deterministic).
        std::vector<SquadronRef*> team_squadrons;
        for (auto& sq : squadrons_) {
            if (sq.owner == static_cast<std::uint8_t>(slot)) {
                team_squadrons.push_back(&sq);
            }
        }
        if (team_squadrons.empty()) continue;   // nobody to task

        // Walk the profile table in wire-byte order — the free,
        // data-driven mission-menu ordering.
        for (std::uint8_t byte = 1; byte < kMissionTypeCount; ++byte) {
            const auto& profile = profiles_.for_mission(byte);

            // Capability gate: this slice cannot verify vehicle
            // capabilities (VEH_STEALTH, VTOL, ...) — profiles that
            // require one don't generate (conservative default).
            if (!profile.caps.empty()) continue;

            // Role gate: some team squadron must fly this role.
            SquadronRef* lead = nullptr;
            for (auto* sq : team_squadrons) {
                if (aro_name(sq->specialty) != profile.aro) continue;
                // Pick the squadron with the most available aircraft;
                // tie -> wire-order-first (strictly greater keeps the
                // earlier one, which is the deterministic tie-break).
                if (!lead || sq->available > lead->available) lead = sq;
            }
            if (!lead) continue;

            // Aircraft gate: the profile's default package size must be
            // coverable by the squadron's available aircraft.
            const int count = std::min(profile.str, lead->available);
            if (count <= 0) continue;

            MissionIntent intent;
            intent.issued_time = clock_;
            intent.time_on_target =
                midpoint_tot(clock_, profile.min_time, profile.max_time);
            intent.team = static_cast<std::uint8_t>(slot);
            for (int t = 0; t < teams_.team_count(); ++t) {
                if (teams_.slot(t) == slot) {
                    intent.team_name = teams_.name(t);
                    break;
                }
            }
            intent.mission_byte = byte;
            intent.mission_name = std::string(mission_type_name(byte));
            intent.aircraft_count = count;
            intent.squadron_id = lead->id_num;
            intent.squadron_name = lead->name;
            intent.package_id = cfg_.first_package_id +
                                static_cast<std::uint32_t>(intents_.size());
            intent.flight_id = intent.package_id;

            // Draw the aircraft down (the attrition ledger B.3 builds on).
            lead->available -= count;

            // Publish + record (the campaign's only outward coupling).
            bus_.publish(intent);
            intents_.push_back(intent);
        }
    }
}

std::string Campaign::to_summary_json() const {
    // Deterministic JSON: fixed key order, slot-ordered teams,
    // publish-ordered intents. No floats (everything integral).
    f4::json::Writer w;
    w.put("{\n  \"format\": \"f4-campaign-summary\",\n  \"version\": 1");
    w.put(",\n  \"clock_sec\": ");
    w.number(clock_);
    w.put(",\n  \"cycles_fired\": ");
    w.number(cycles_fired_);
    w.put(",\n  \"task_cycle_sec\": ");
    w.number(cfg_.air_task_cycle_sec);

    w.put(",\n  \"belligerent_teams\": [");
    {
        bool first = true;
        for (const int slot : belligerent_teams()) {
            if (!first) w.put(", ");
            first = false;
            std::string name;
            for (int t = 0; t < teams_.team_count(); ++t) {
                if (teams_.slot(t) == slot) {
                    name = teams_.name(t);
                    break;
                }
            }
            w.string(name);
        }
    }
    w.put("]");

    w.put(",\n  \"intents\": [");
    for (std::size_t i = 0; i < intents_.size(); ++i) {
        const auto& in = intents_[i];
        w.put(i ? ",\n    " : "\n    ");
        w.put("{\"time\": ");
        w.number(in.issued_time);
        w.put(", \"tot\": ");
        w.number(in.time_on_target);
        w.put(", \"team\": ");
        w.number(in.team);
        w.string_key(", team_name", in.team_name);
        w.put(", \"mission_byte\": ");
        w.number(in.mission_byte);
        w.string_key(", mission", in.mission_name);
        w.put(", \"aircraft\": ");
        w.number(in.aircraft_count);
        w.put(", \"squadron_id\": ");
        w.number(in.squadron_id);
        w.string_key(", squadron", in.squadron_name);
        w.put(", \"package_id\": ");
        w.number(in.package_id);
        w.put(", \"flight_id\": ");
        w.number(in.flight_id);
        w.put("}");
    }
    w.put("\n  ]");

    w.put(",\n  \"totals\": {");
    {
        // Per-team, per-mission counts — team slot order, then byte order.
        bool first_team = true;
        for (const int slot : belligerent_teams()) {
            // Count this team's missions by byte.
            std::vector<int> by_byte(kMissionTypeCount, 0);
            for (const auto& in : intents_) {
                if (in.team == static_cast<std::uint8_t>(slot)) {
                    ++by_byte[in.mission_byte];
                }
            }
            for (std::size_t b = 1; b < kMissionTypeCount; ++b) {
                if (by_byte[b] == 0) continue;
                w.put(first_team ? "\n    " : ",\n    ");
                first_team = false;
                std::string name;
                for (int t = 0; t < teams_.team_count(); ++t) {
                    if (teams_.slot(t) == slot) {
                        name = teams_.name(t);
                        break;
                    }
                }
                w.string(name + "/" + std::string(mission_type_name(static_cast<std::uint8_t>(b))));
                w.put(": ");
                w.number(by_byte[b]);
            }
        }
    }
    w.put("\n  }\n}\n");
    return w.str();
}

} // namespace f4::campaign
