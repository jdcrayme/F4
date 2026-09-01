// f4-simulation/src/combat_transcript.cpp
//
// CombatTranscript implementation — see combat_transcript.hpp for the
// design notes. Every event subscribed here is a bus TRANSITION (the bus
// convention: no per-frame telemetry), so the transcript's volume is
// bounded by the fight itself, not the tick rate.

#include "f4/simulation/combat_transcript.hpp"

#include "f4/simulation/simulation.hpp"

#include <f4/sensors/messages.hpp>
#include <f4/sensors/rwr.hpp>
#include <f4/weapons/messages.hpp>
#include <f4/weapons/weapon_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace f4::simulation {

namespace {

constexpr double kFeetPerNm = 6076.11548;

/// "77.4" — one decimal, no trailing ".0" noise beyond it.
std::string fmt1(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

std::string fmt0(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
}

} // namespace

// ── brevity word ───────────────────────────────────────────────────────────

std::string missile_brevity_word(int guidance_kind) {
    using GK = f4::weapons::GuidanceKind;
    switch (static_cast<GK>(guidance_kind)) {
        case GK::ActiveRadar:    return "FOX 3";
        case GK::SemiActiveRadar: return "FOX 1";
        case GK::Ir:             return "FOX 2";
        case GK::None:           break;
    }
    return "missile";
}

// ── ring buffer ────────────────────────────────────────────────────────────

const CombatTranscript::Entry* CombatTranscript::at(std::size_t i) const noexcept {
    if (i >= size_) return nullptr;
    return &ring_[(begin_ + i) % ring_.size()];
}

void CombatTranscript::clear() noexcept {
    begin_ = 0;
    size_ = 0;
}

void CombatTranscript::set_capacity(std::size_t entries) noexcept {
    if (entries == 0) entries = 1;

    // Collect the newest `entries` items (oldest-first), then rebuild.
    std::vector<Entry> keep;
    keep.reserve(std::min(entries, size_));
    const std::size_t first = size_ > entries ? size_ - entries : 0;
    for (std::size_t i = first; i < size_; ++i) keep.push_back(*at(i));

    ring_.assign(entries, Entry{});
    begin_ = 0;
    size_ = keep.size();
    for (std::size_t i = 0; i < keep.size(); ++i) ring_[i] = std::move(keep[i]);
}

void CombatTranscript::push(double time_s, std::string speaker,
                            std::string text, Severity severity) {
    if (ring_.empty()) ring_.assign(64, Entry{});

    Entry& slot = ring_[(begin_ + size_) % ring_.size()];
    if (size_ == ring_.size()) {
        // Full: overwrite the oldest (begin_ advances).
        begin_ = (begin_ + 1) % ring_.size();
    } else {
        ++size_;
    }
    slot.time_s = time_s;
    slot.speaker = std::move(speaker);
    slot.text = std::move(text);
    slot.severity = severity;
}

// ── callsigns ──────────────────────────────────────────────────────────────

std::string CombatTranscript::callsign_of(std::uint64_t entity_id) const {
    for (const auto& [id, name] : callsigns_) {
        if (id == entity_id) return name;
    }
    char fallback[24];
    std::snprintf(fallback, sizeof(fallback), "#%llx",
                  static_cast<unsigned long long>(entity_id));
    return fallback;
}

// ── attach ─────────────────────────────────────────────────────────────────

void CombatTranscript::attach(Simulation& sim) {
    callsigns_.clear();
    clear();

    // The callsign map: scenario().aircraft[i] spawned as
    // aircraft_entities()[i] (the spawn loop's documented order —
    // test_combat_integration relies on the same alignment).
    const auto& ids = sim.aircraft_entities();
    const auto& defs = sim.scenario().aircraft;
    const std::size_t n = std::min(ids.size(), defs.size());
    callsigns_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        callsigns_.emplace_back(ids[i].value, defs[i].callsign);
    }

    // Weapon-class lookup for launch calls (the table is a Simulation
    // member and outlives the transcript's use within the sim run).
    const auto* weapons = &sim.weapon_table();

    auto& bus = sim.bus();

    // ── Sensors: track picture ─────────────────────────────────────────
    bus.subscribe<sensors::RadarTrackAcquiredMessage>(
        [this](const sensors::RadarTrackAcquiredMessage& m) {
            push(m.time_s, callsign_of(m.radar_entity_id),
                 "Radar contact, " + callsign_of(m.target_entity_id) + ".",
                 Severity::Info);
        });

    bus.subscribe<sensors::RadarTrackDroppedMessage>(
        [this](const sensors::RadarTrackDroppedMessage& m) {
            push(m.time_s, callsign_of(m.radar_entity_id),
                 "Lost the picture on " + callsign_of(m.target_entity_id) + ".",
                 Severity::Info);
        });

    // ── Sensors: RWR transitions (victim speaks) ───────────────────────
    bus.subscribe<sensors::RwrWarningMessage>(
        [this](const sensors::RwrWarningMessage& m) {
            const std::string emitter = callsign_of(m.emitter_id);
            if (m.type == sensors::RwrWarningType::Lock) {
                push(m.time_s, callsign_of(m.victim_id),
                     "Spike from " + emitter + ", " +
                         fmt0(m.range_ft / kFeetPerNm) + " NM.",
                     Severity::Warning);
            } else if (m.type == sensors::RwrWarningType::Launch) {
                push(m.time_s, callsign_of(m.victim_id),
                     "Missile launch, " + emitter + "! Break, break.",
                     Severity::Warning);
            }
            // Search strobes are component state, never bus traffic.
        });

    // ── Weapons: the engagement ────────────────────────────────────────
    bus.subscribe<weapons::MissileLaunchedMessage>(
        [this, weapons](const weapons::MissileLaunchedMessage& m) {
            std::string weapon_name = "missile";
            std::string word = "missile";
            if (const auto* rec = weapons->get(m.weapon_handle)) {
                weapon_name = rec->name;
                word = missile_brevity_word(
                    static_cast<int>(rec->guidance));
            }
            push(m.sim_time_s, callsign_of(m.shooter_id),
                 word + ", " + weapon_name + " away on " +
                     callsign_of(m.target_id) + ".",
                 Severity::Warning);
        });

    bus.subscribe<weapons::MissileDetonatedMessage>(
        [this](const weapons::MissileDetonatedMessage& m) {
            const bool hit = m.cause == weapons::MissileEndCause::TargetHit;
            push(m.sim_time_s, "C2",
                 std::string(hit ? "Missile impact on " : "Missile end on ") +
                     callsign_of(m.target_id) + " — " +
                     weapons::missile_end_cause_name(m.cause) + ".",
                 Severity::Info);
        });

    bus.subscribe<weapons::DamageAppliedMessage>(
        [this](const weapons::DamageAppliedMessage& m) {
            push(m.sim_time_s, "C2",
                 callsign_of(m.target_id) + " is hit — " + fmt0(m.damage) +
                     " damage, " + fmt0(m.hit_points_after) + " HP left.",
                 m.killed ? Severity::Kill : Severity::Info);
        });

    bus.subscribe<weapons::EntityKilledMessage>(
        [this](const weapons::EntityKilledMessage& m) {
            push(m.sim_time_s, "C2",
                 "Splash " + callsign_of(m.target_id) + "! Shot down by " +
                     callsign_of(m.shooter_id) + ".",
                 Severity::Kill);
        });

    // GunFiredMessage deliberately NOT subscribed: the M3 brain never
    // fires guns (BVR doctrine), and a gun-fight transcript belongs with
    // the WVR module that will actually produce the bursts.
}

} // namespace f4::simulation
