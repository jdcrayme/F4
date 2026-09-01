// f4-weapons/include/f4/weapons/gun.hpp
//
// GunStream — a burst of gun rounds simulated as ballistic tracer points.
//
// WHY TRACERS ARE NOT ENTITIES: a 6,000 rpm gun emits 100 rounds/second;
// entity-per-bullet would flood the ECS (thousands of live bullets, each a
// map lookup + spatial-index entry) for projectiles whose individual
// identity nobody consumes. A burst is one GunStream object holding a small
// vector of points; hit detection runs against the EntityWorld directly.
//
// Motion model per tracer (per tick):
//   vel.z -= g * dt;  pos += vel * dt
//   (drag is omitted at M1 — within the 1-2 s tracer lifetime over combat
//   ranges the drop/error from drag is inside the dispersion we already
//   model explicitly; revisit with A-G gun employment)
//
// Dispersion: at spawn each round's direction gets a seeded-random angular
// offset up to GunConfig::dispersion_rad (uniform in disc, isotropic).
// Seeded std::mt19937 — deterministic per seed for replayable tests.
//
// Hit detection: each tick every tracer checks entities that carry a
// TransformComponent against the SEGMENT the tracer swept THIS tick
// ([pre-move position, post-move position], derivable exactly from the
// semi-implicit Euler update: p_new = p_old + v_new*dt). A round hits
// when the segment's closest approach to an entity's center is within
// kGunHitRadiusFt (shooter excluded). The segment check is REQUIRED,
// not an optimization nicety: at 3,400 ft/s a tracer covers ~57 ft per
// 1/60 s tick — more than the 40 ft hit radius — so point-in-sphere
// checks tunnel straight through a target between ticks.
// A hit applies damage via apply_damage() (round power vs hit points, roll
// drawn from the stream's RNG) and removes the tracer. Targets without a
// DamageStateComponent still consume the round (sparks, no damage event).
//
// The bus integration follows the FlightModel pattern: optional MessageBus*
// (set_message_bus); GunFiredMessage on the first round of a burst,
// DamageAppliedMessage / EntityKilledMessage on hits. No bus = silent
// (the math still happens). Message sim_time_s is stamped by the host
// sweep (set_sim_time) — the stream owns ballistics, not the clock.
//
// OWNERSHIP: a GunStream belongs to a shooter system — in the sim it
// lives inside GunComponent (gun_component.hpp) and is driven by the
// update_guns world sweep; a test harness may drive it directly. It does
// NOT attach itself to the ECS.

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/geo/position.hpp>
#include <f4/math/vec3.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/weapons/damage.hpp>

#include <random>
#include <vector>

namespace f4::weapons {

/// Radius (feet) inside which a tracer counts as a hit on an entity.
/// Matches the per-round lethal radius so every detected hit lands inside
/// the damage falloff cone (a hit at the edge does zero damage).
inline constexpr double kGunHitRadiusFt = 40.0;

struct GunConfig {
    double muzzle_velocity_fps = 3400.0;  // M61A1
    double rounds_per_minute   = 6000.0;  // firing rate while bursting
    double dispersion_rad      = 0.004;   // ~0.23 deg cone half-angle
    double round_power_lb      = 0.22;    // HEI round vs hit points
    double lethal_radius_ft    = 40.0;    // per-round damage falloff scale
    double max_flight_s        = 2.0;     // tracer lifetime (self-clean)
};

/// One ballistic tracer point.
struct GunTracer {
    f4::geo::WorldPosition position{};
    f4::math::Vec3<double> velocity{};
    double age_s = 0.0;
};

/// One hit event returned by tick() (the effect is ALREADY applied to the
/// world before the event is returned — callers use it for bookkeeping).
struct GunHit {
    std::uint64_t target_id = 0;
    std::uint64_t shooter_id = 0;
    double damage = 0.0;
    bool killed = false;
};

class GunStream {
public:
    explicit GunStream(GunConfig config = {}, std::uint32_t seed = 0);

    void set_message_bus(messaging::MessageBus* bus) { bus_ = bus; }

    /// Sim-time stamp for every message this stream publishes (the host
    /// sweep calls this every tick before tick(); 0.0 until then — the
    /// same ownership split MissileSimComponent uses for its clock).
    void set_sim_time(double t) noexcept { sim_time_s_ = t; }

    /// Weapon-class handle carried on GunFiredMessage (name resolution for
    /// the recorder/transcript). Set once at attach time by the host.
    void set_weapon_handle(std::uint32_t h) noexcept { weapon_handle_ = h; }

    /// Begin a burst of `rounds` at the aim target `aim_target_id` (the
    /// GunFiredMessage's hint; 0 = none). Rounds emit at rounds_per_minute
    /// from the geometry passed to tick() until the burst is exhausted.
    void start_burst(int rounds, std::uint64_t aim_target_id = 0);

    /// Rounds left to emit in the current burst (rounded up).
    [[nodiscard]] int rounds_remaining() const noexcept {
        return static_cast<int>(std::ceil(burst_remaining_));
    }
    /// Live tracers in flight.
    [[nodiscard]] std::size_t tracer_count() const noexcept { return tracers_.size(); }
    /// Read-only view of live tracers (tests, debug overlays).
    [[nodiscard]] const std::vector<GunTracer>& tracers() const noexcept { return tracers_; }

    /// Advance the gun one tick.
    ///
    ///   muzzle     world position rounds are emitted from this tick
    ///   direction  UNIT firing direction this tick
    ///   shooter_id excluded from hit detection (a gun must not shoot
    ///              itself; typically the owning entity)
    ///
    /// While a burst is active, rounds emit from `muzzle` along `direction`
    /// at the configured rate — call sites that fire from a moving aircraft
    /// pass that aircraft's muzzle position and boresight each tick.
    /// Returns the hits applied THIS tick.
    std::vector<GunHit> tick(double dt,
                             entities::EntityWorld& world,
                             std::uint64_t shooter_id,
                             const f4::geo::WorldPosition& muzzle,
                             const f4::math::Vec3<double>& direction);

private:
    void emit_round(const f4::geo::WorldPosition& muzzle,
                    const f4::math::Vec3<double>& unit_direction);

    GunConfig cfg_;
    std::mt19937 rng_;
    std::vector<GunTracer> tracers_;
    double burst_remaining_ = 0.0;   // fractional: emission is rate-based
    double emit_carry_ = 0.0;        // rounds owed but not yet emitted
    int burst_size_ = 0;             // rounds requested for the current burst
    bool burst_announced_ = false;   // GunFiredMessage sent for this burst?
    std::uint64_t burst_aim_id_ = 0; // aim hint carried on GunFiredMessage
    std::uint32_t weapon_handle_ = 0;
    double sim_time_s_ = 0.0;        // host-stamped clock for messages
    messaging::MessageBus* bus_ = nullptr;
};

} // namespace f4::weapons
