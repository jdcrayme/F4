// f4-simulation/include/f4/simulation/combat_transcript.hpp
//
// CombatTranscript — observes the sim MessageBus and formats combat events
// as human-readable brevity radio calls (the "fight narration").
//
// Engine-agnostic by design: no renderer, no window, no Raylib. The
// scenario player polls the ring buffer to draw its COMBAT panel; tests
// assert the calls directly. This is the M4 "combat observability" half of
// the deferred recorder work (Docs/COMBAT_CHAIN_PLAN.md M4) — the events
// were already on the bus since M1/M2, nothing formatted them for humans.
//
// Subscribed events (state TRANSITIONS only — the bus never carries
// per-frame telemetry, per the project bus convention):
//   sensors::RadarTrackAcquiredMessage  -> "Radar contact, <target>."
//   sensors::RadarTrackDroppedMessage   -> "Lost the picture on <target>."
//   sensors::RwrWarningMessage (Lock)   -> "Spike from <emitter>, <nm> NM."
//   sensors::RwrWarningMessage (Launch) -> "Missile launch, <emitter>! Break."
//   weapons::MissileLaunchedMessage     -> "<FOX n>, <weapon> away on <target>."
//   weapons::MissileDetonatedMessage    -> "<C2> Missile impact on <target> — <cause>."
//   weapons::DamageAppliedMessage       -> "<C2> <target> is hit — <dmg> damage, <hp> HP left."
//   weapons::EntityKilledMessage        -> "<C2> Splash <target>! Shot down by <shooter>."
//
// Callsigns resolve through the aircraft_entities() <-> scenario().aircraft
// index alignment (entity id -> callsign, "#<hex>" fallback for missiles
// and other non-scenario entities). A/A launch brevity follows the
// weapon's guidance kind: ActiveRadar=FOX 3, SemiActiveRadar=FOX 1,
// Ir=FOX 2 (missile_brevity_word(), exposed for tests).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace f4::simulation {

class Simulation;

class CombatTranscript {
public:
    /// How loud the line is — hosts color-code their panels off this.
    enum class Severity : std::uint8_t {
        Info = 0,     // radar contacts, track drops, impacts
        Warning = 1,  // spikes, launches, damage
        Kill = 2,     // splash calls
    };

    struct Entry {
        double time_s = 0.0;
        std::string speaker;                 // callsign or "C2"
        std::string text;                    // brevity line (no speaker prefix)
        Severity severity = Severity::Info;
    };

    /// Subscribe to the sim's bus + build the callsign map. The transcript
    /// keeps only the bus pointer handed out by the bus subscribe() calls —
    /// it never dereferences the Simulation after attach returns (the
    /// weapon-table pointer is captured for launch-name resolution).
    void attach(Simulation& sim);

    // ── Ring buffer (oldest-first absolute indices, RadioLog's API shape)
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const Entry* at(std::size_t i) const noexcept;
    void clear() noexcept;
    /// Ring capacity (default 64). Shrinking keeps the newest entries.
    void set_capacity(std::size_t entries) noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return ring_.size(); }

    /// Entity id -> callsign ("#<hex>" fallback). Exposed so hosts can
    /// label markers with the same strings the transcript uses.
    [[nodiscard]] std::string callsign_of(std::uint64_t entity_id) const;

private:
    void push(double time_s, std::string speaker, std::string text,
              Severity severity);

    // Callsign map: scenario aircraft are few (2-4), linear scan is free
    // and keeps attach() allocation-simple.
    std::vector<std::pair<std::uint64_t, std::string>> callsigns_;

    std::vector<Entry> ring_{};
    std::size_t begin_ = 0;
    std::size_t size_ = 0;
};

/// The A/A launch brevity word for a missile's guidance kind
/// (ActiveRadar -> "FOX 3", SemiActiveRadar -> "FOX 1", Ir -> "FOX 2",
/// anything else -> "missile"). Free function: unit-testable without a sim.
[[nodiscard]] std::string missile_brevity_word(int guidance_kind);

} // namespace f4::simulation
