// f4-weapons/include/f4/weapons/countermeasures.hpp
//
// Countermeasures — dispensers, decoys, and the seeker-seduction model.
// The consumption half of the MissileModule's defeat intents (f4-ai's
// should_chaff()/should_flare() documented "no countermeasure
// consumption model exists yet" — this is it).
//
// Three pieces, mirroring the missile stack's shape:
//
//   CountermeasureComponent — the dispenser: chaff/flare rounds, salvo
//       sizes, salvo interval. Passive; the HOST (combat bridge) calls
//       deploy_countermeasure() when the brain's defeat module raises
//       its intents.
//
//   DecoyComponent + DecoySimComponent — decoys are ENTITIES (the
//       FreeFalcon VuEntity model: a flare is a real object with its
//       own id, team, and lifecycle, exactly like a missile). The sim
//       component is behavioral at priority 42 — after the radar pass
//       (45), before the missile sims (40) — so a seeker reading this
//       tick sees this tick's decoy positions.
//
//   make_decoy_aware_seeker_source() — the production user of
//       MissileComponent's documented seeker_source hook ("the hook
//       through which track quality, seeker gimbal error, or jamming
//       enter the flyout"). Built per missile at launch; each tick it
//       asks: is there an enemy decoy inside MY seeker cone, and does
//       the seeker take the bait?
//
// Seduction model (documented, data-driven where the data exists):
//
//   IR missiles (GuidanceKind::Ir) roll the SEEKER CARD's flare_chance
//       (SimData SENSDATA/IRST — aim9l 0.2, aim9p 0.4, sa7 0.5, ...)
//       once per flare as it enters the cone. Success = the seeker
//       tracks the flare until it burns out; the missile chases it
//       down and the fuze fires against the flare, far from the jet.
//
//   Radar missiles roll chaff_transfer_probability (0.5 — a tuning
//       constant documented as such until a data source exists) per
//       chaff bloom inside the cone. Success = the seeker tracks the
//       bloom (its 25 m^2 return dominates the fighter's 10) until it
//       decays.
//
//   A failed roll never re-rolls the same decoy (one honest chance per
//   bloom — 60 Hz re-rolls would be certain seduction); a burnt-out
//   decoy frees the seeker to re-acquire the target or the next bloom.
//
// THE DAMAGE COUPLING (why seduction saves the jet): a seduced missile
// detonates at the DECOY; the terminal handler measures the miss
// distance against the ASSIGNED target entity's position, so a burst
// at a flare reads as a large miss — the damage model's range falloff
// does the rest.
//
// Dependencies: f4-entities, f4-geo, f4-math, f4-messaging,
// f4-weapons' own missile_battery.hpp (the hook + MissileStatus).
// C++20.

#pragma once

#include <cstdint>
#include <functional>

#include <f4/entities/entity.hpp>
#include <f4/geo/position.hpp>
#include <f4/math/vec3.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/weapons/missile_battery.hpp>
#include <f4/weapons/weapon_types.hpp>

namespace f4::weapons {

// ============================================================================
// CountermeasureComponent — the dispenser (passive).
// ============================================================================
struct CountermeasureComponent : public entities::Component<CountermeasureComponent> {
    // The FreeFalcon VCD carries per-unit chaff/flare counts; until that
    // conversion lands these are the fighter defaults the combat bridge
    // arms (documented constants, not data claims).
    static constexpr int kDefaultChaffRounds = 30;
    static constexpr int kDefaultFlareRounds = 15;

    int chaff_rounds = kDefaultChaffRounds;
    int flare_rounds = kDefaultFlareRounds;
    int chaff_capacity = kDefaultChaffRounds;
    int flare_capacity = kDefaultFlareRounds;

    // Salvo doctrine: two bundles of chaff per pull (the classic split-S
    // salt-and-pepper), one flare per pull (AIM-9-class seekers are
    // defeated by the roll, not by pileup). The interval paces the
    // intents: the defeat module raises should_chaff every tick of the
    // beam; the dispenser releases at most one salvo per interval.
    int chaff_salvo = 2;
    int flare_salvo = 1;
    double salvo_interval_s = 0.5;

    double last_chaff_s = -1.0e12;
    double last_flare_s = -1.0e12;
};

// ============================================================================
// DecoyComponent / DecoySimComponent — the decoy entity (chaff bloom /
// flare).
// ============================================================================
enum class DecoyKind : std::uint8_t {
    Chaff = 0,
    Flare = 1,
};

[[nodiscard]] inline const char* decoy_kind_name(DecoyKind k) noexcept {
    return k == DecoyKind::Chaff ? "chaff" : "flare";
}

struct DecoyComponent : public entities::Component<DecoyComponent> {
    DecoyKind kind = DecoyKind::Chaff;
    std::uint64_t owner_id = 0;
    double born_s = 0.0;
    double ttl_s = 5.0;          // chaff cloud life; flares burn faster

    // Signature (what the decoy LOOKS like to a seeker):
    double rcs_m2 = 25.0;        // chaff bloom — dominates a fighter's 10
    double ir_signal = 8.0;      // flare — dominates a fighter's ir1 band

    [[nodiscard]] bool expired_at(double now_s) const noexcept {
        return now_s - born_s > ttl_s;
    }
};

class DecoySimComponent
    : public entities::BehavioralComponent<DecoySimComponent> {
public:
    int priority() const noexcept override { return 42; }  // before missiles

    void on_attached(entities::EntityHandle& self) override { owner_ = self; }
    void update(double dt, messaging::MessageBus& bus) override;

    // Motion tuning (public: data cards, live-tunable).
    //   chaff — a cloud decelerating hard toward the airstream's rest
    //   flare — a falling candle: gravity, light drag
    double chaff_drag_per_s = 2.5;
    double flare_drag_per_s = 0.35;
    bool flare_gravity = true;

private:
    entities::EntityHandle owner_{};
};

// ============================================================================
// deploy / sweep / count
// ============================================================================

/// Dispense one salvo of `kind` from `aircraft` (the host executes the
/// brain's defeat intent). Requires on the aircraft: TransformComponent,
/// CountermeasureComponent. Debits the store, spawns the salvo's decoy
/// entities (TEAM tag copied — a red flare never seduces a red missile;
/// ROLE tag "decoy"), and publishes one CountermeasureDeployedMessage
/// per salvo. Returns the number of decoys created (0 when dry, out of
/// interval, or missing components — a refusal changes nothing).
[[nodiscard]] int deploy_countermeasure(
    entities::EntityWorld& world,
    messaging::MessageBus& bus,
    const entities::EntityHandle& aircraft,
    DecoyKind kind,
    double sim_time_s,
    std::uint32_t rng_seed);

/// Destroy every decoy past its ttl. Host-side sweep (between ticks,
/// never inside update_all — the missile sweep's rule). Returns the
/// count destroyed.
std::size_t sweep_expired_decoys(entities::EntityWorld& world,
                                 double sim_time_s);

/// Live (unexpired) decoys of `kind` — host/debug convenience.
[[nodiscard]] std::size_t count_live_decoys(
    const entities::EntityWorld& world, DecoyKind kind, double sim_time_s);

// ============================================================================
// The decoy-aware seeker source — the seduction model.
// ============================================================================

/// Per-missile configuration, resolved by the host at launch from the
/// weapon record + the IR seeker card (SimData irstdata).
struct SeekerCountermeasureConfig {
    GuidanceKind guidance = GuidanceKind::None;
    /// P(a flare takes this seeker) — the card's flare_chance for IR
    /// missiles; unused for radar. The host's default when no card
    /// matches: kDefaultIrFlareChance below.
    double flare_chance = 0.3;
    /// P(a chaff bloom takes this radar seeker) — tuning constant.
    double chaff_transfer_p = 0.5;
    /// The seeker envelope (from the weapon record): decoys outside it
    /// cannot bait this seeker.
    double seeker_half_angle_rad = 0.0;
    double seeker_max_range_ft = 0.0;

    /// The missile entity (the closure reads its own transform each
    /// tick for the cone geometry) and the shooter's team (same-team
    /// decoys never seduce — the IFF rule missiles already follow).
    std::uint64_t missile_id = 0;
    std::string shooter_team;
    std::uint32_t rng_seed = 0;
};

/// The default P(flare seduces) when the weapon's IR seeker card is
/// unknown (no irstdata match). Documented tuning constant.
inline constexpr double kDefaultIrFlareChance = 0.3;

/// Build the SeekerSourceFn for a missile flying against countermeasure-
/// equipped targets. State lives in a shared_ptr (component copies keep
/// one book: seduced id, failed rolls, RNG).
[[nodiscard]] MissileComponent::SeekerSourceFn
make_decoy_aware_seeker_source(const SeekerCountermeasureConfig& cfg);

} // namespace f4::weapons
