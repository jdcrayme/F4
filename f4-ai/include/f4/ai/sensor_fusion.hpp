// f4-ai/include/f4/ai/sensor_fusion.hpp
//
// SensorFusion — the AI's "eyes".
//
// Builds and maintains the AI's target list from EntityWorld. Queries every
// entity with a TransformComponent, computes geometry (range, aspect,
// bearing) via f4::geo, applies detection-source rules (radar/RWR/visual/GCI),
// and computes threat scores.
//
// This is the first f4-ai module because every other module needs a target
// list. It validates the EntityWorld query API (with_tag, with_component,
// within_radius) and the f4::geo::to_bra() geometry against real campaign
// entities.
//
// FreeFalcon reference:
//   - sfusion.cpp (~504 lines): threat scoring, detection sources, combat
//     class heuristic, skill-dependent update interval.
//   - targeting.cpp (~200 lines): target selection logic.
//   - digimain.cpp:566: FrameExec's "set target update rate" + "DoTargeting".
//
// Design:
//   - Ownship is identified by EntityId (uint64_t). We snapshot ownship
//     position/velocity at the start of each refresh — downstream tactic
//     modules see a consistent picture.
//   - Targets are stored in a std::vector<TargetInfo>. The vector is rebuilt
//     from scratch on each refresh (skill-dependent interval). Between
//     refreshes, the vector is stable — modules can hold TargetInfo* pointers
//     for the duration of one minor frame.
//   - EWMA smoothing: per-update delta is computed against the previous
//     refresh's snapshot, then blended 0.85 (old) / 0.15 (new). The rate is
//     normalized by the update interval so it's in per-second units.
//   - Hostility is determined by the entity's "team" tag, OWN-RELATIVE
//     since M3 tactics: hostile = (target team != ownship team). An
//     ownship without a team tag falls back to the legacy blue-perspective
//     rule (team "red" => hostile). (Future: real team comparison via the
//     campaign team-stance data.)
//   - Detection sources are computed AFTER geometry so they can be range-gated.
//     GCI is always true (it sees everything within the theater by definition).
//
// Threading: SensorFusion is NOT thread-safe. It lives on the sim thread
// and is updated once per minor frame from DigitalBrain::update().

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <f4/ai/ai_brain.hpp>
#include <f4/ai/target_info.hpp>
#include <f4/entities/entity.hpp>
#include <f4/geo/position.hpp>
#include <f4/messaging/f4_messaging.hpp>

namespace f4::ai {

/// SensorFusion configuration. Lives at namespace scope (not nested in the
/// class) so it can be used as a default argument inside the SensorFusion
/// declaration itself — default member initializers aren't complete until
/// the enclosing class definition ends, so `Config cfg = Config{}` would
/// fail to compile if Config were a nested struct.
struct SensorFusionConfig {
    double max_radar_range_nm{80.0};   // radar detection range
    double max_visual_range_nm{10.0};  // visual detection range (Mk.I eyeball)
    double max_engage_range_nm{40.0};  // default engagement range
    double max_rwr_range_nm{50.0};     // RWR detection range
    // EWMA weight for the new sample. 0.15 => 85% old / 15% new, matching
    // FreeFalcon's ataDot/rangeDot blend (digimain.cpp).
    double ewma_alpha{0.15};
};

class SensorFusion {
public:
    using Config = SensorFusionConfig;

    // ========================================================================
    // DetectionPolicy — M2 integration point (COMBAT_CHAIN_PLAN.md M2).
    //
    // Optional, NON-OWNING hook that replaces the built-in detection-source
    // rules. When set, SensorFusion::rebuild_target_list() asks the policy
    // for every candidate's radar/rwr/visual/gci flags instead of using the
    // legacy range-gated defaults (radar/RWR = hostile + range, GCI =
    // always true).
    //
    // This is how f4-sensors' track store / RWR state will feed the AI at
    // M3: an adapter implements classify() by querying the ownship's
    // RadarSimComponent tracks and RwrComponent warnings. The adapter itself
    // lives at the host/M3 layer — f4-ai must not link f4-sensors (tactics
    // consume sensors, never the reverse), so the interface travels as a
    // pure virtual and the wiring is the host's job.
    //
    // Lifetime: the caller owns the policy and must keep it alive for the
    // SensorFusion's lifetime (or until replaced). nullptr = legacy rules.
    // ========================================================================
    struct DetectionPolicy {
        struct Verdict {
            bool radar{false};
            bool rwr{false};
            bool visual{false};
            bool gci{false};
        };
        virtual ~DetectionPolicy() = default;
        [[nodiscard]] virtual Verdict classify(const TargetInfo& t) = 0;
    };

    SensorFusion() = default;

    // Non-copyable (holds pointers to EntityWorld / MessageBus).
    SensorFusion(const SensorFusion&) = delete;
    SensorFusion& operator=(const SensorFusion&) = delete;

    void initialize(std::uint64_t ownship_id,
                    entities::EntityWorld& world,
                    messaging::MessageBus& bus,
                    SkillLevel skill,
                    Config cfg = Config{});

    /// Per-frame update. Decays the update timer; when it hits zero,
    /// rebuilds the target list from the EntityWorld.
    void update(double dt);

    /// Force an immediate refresh of the target list (ignores update timer).
    /// Used when an event (RWR spike, missile launch detection) requires an
    /// immediate re-evaluation.
    void force_refresh();

    // --- Accessors ---

    [[nodiscard]] const std::vector<TargetInfo>& targets() const noexcept { return targets_; }
    [[nodiscard]] std::size_t target_count() const noexcept { return targets_.size(); }

    /// Highest threat_score among all visible targets (may be a missile or
    /// a non-fighter). Returns nullptr if no targets are visible.
    [[nodiscard]] const TargetInfo* primary_target() const noexcept;

    /// Highest threat_score among visible HOSTILE fighters and
    /// maneuvering aircraft (combat_class >= 2). Returns nullptr if none.
    ///
    /// HOSTILES ONLY (M3 tactics, 2-ship discipline): before the filter a
    /// visible friendly with no hostile in the picture won the query by
    /// default — in a 2v2 the wingman's own LEAD was its "threat target"
    /// pre-detection, and the BVR rung engaged a friendly. Friendlies stay
    /// in the target list (situational awareness); they just never arm a
    /// combat rung.
    [[nodiscard]] const TargetInfo* threat_target() const noexcept;

    /// The wingman's SORT (FreeFalcon winglogic): the highest-scoring
    /// visible hostile fighter that is NOT `lead_engaged_id` — the "free"
    /// bandit. When the ONLY visible hostile is the lead's target, return
    /// it (support the kill — a doubled-up 2v1 beats an idle wingman).
    /// `lead_engaged_id` == 0 (lead not fighting / not a wingman)
    /// degenerates to threat_target(). Engine-agnostic: the host reads the
    /// lead's engagement and passes the id; nothing here touches the world.
    [[nodiscard]] const TargetInfo* sorted_threat_target(
        std::uint64_t lead_engaged_id) const noexcept;

    /// Nearest visible HOSTILE incoming missile. Returns nullptr if none
    /// (or if the only missiles in flight are same-team — your own shot
    /// must never read as a threat to defend against; launch_missile
    /// copies the shooter's team onto the missile entity).
    [[nodiscard]] const TargetInfo* missile_threat() const noexcept;

    /// Time until next scheduled target list refresh (seconds).
    [[nodiscard]] double time_until_refresh() const noexcept { return update_timer_; }

    // --- Per-target detection source queries ---
    // These are static because they only inspect the TargetInfo's flags —
    // no SensorFusion state is involved. Useful for testing and for
    // downstream modules that want to ask "how did I see this target?".
    [[nodiscard]] static bool detected_by_radar(const TargetInfo& t) noexcept {
        return t.detected_by_radar;
    }
    [[nodiscard]] static bool detected_by_rwr(const TargetInfo& t) noexcept {
        return t.detected_by_rwr;
    }
    [[nodiscard]] static bool detected_by_visual(const TargetInfo& t) noexcept {
        return t.detected_by_visual;
    }
    [[nodiscard]] static bool detected_by_gci(const TargetInfo& t) noexcept {
        return t.detected_by_gci;
    }

    /// "Can we see this target at all?" — any one detection source is enough.
    /// Matches FreeFalcon's canSee rule in sfusion.cpp.
    [[nodiscard]] static bool can_see(const TargetInfo& t) noexcept {
        return t.detected_by_radar || t.detected_by_rwr ||
               t.detected_by_visual || t.detected_by_gci;
    }

    // --- Detection policy (M2 hook; see DetectionPolicy above) --------------
    /// Set (or clear, with nullptr) the detection-source override. Non-owning.
    void set_detection_policy(DetectionPolicy* policy) noexcept { policy_ = policy; }
    [[nodiscard]] DetectionPolicy* detection_policy() noexcept { return policy_; }

    // --- Skill parameters (per AI_IMPLEMENTATION_PLAN §9) ---

    [[nodiscard]] static double update_interval_sec(SkillLevel s) noexcept;
    [[nodiscard]] static double reaction_delay_sec(SkillLevel s) noexcept;

    [[nodiscard]] SkillLevel skill() const noexcept { return skill_; }
    [[nodiscard]] std::uint64_t ownship_id() const noexcept { return ownship_id_; }

private:
    void rebuild_target_list();
    void compute_geometry(TargetInfo& t,
                          const f4::geo::WorldPosition& ownship_pos,
                          const f4::geo::WorldPosition& ownship_vel,
                          const f4::entities::TransformComponent& target_tf);
    void compute_threat_score(TargetInfo& t);
    void update_ewma(TargetInfo& t, const TargetInfo* prev, double dt);

    std::uint64_t ownship_id_{0};
    entities::EntityWorld* world_{nullptr};
    messaging::MessageBus* bus_{nullptr};
    SkillLevel skill_{SkillLevel::Rookie};
    Config cfg_{};
    DetectionPolicy* policy_{nullptr};

    /// Ownship's TEAM tag, resolved at each rebuild. Own-relative
    /// hostility (M3 tactics): target.is_hostile = (target team !=
    /// own_team_). Empty = ownship carries no team tag => the legacy
    /// blue-perspective rule ("red" => hostile) applies.
    std::string own_team_{};

    double update_timer_{0.0};
    std::vector<TargetInfo> targets_;
    std::vector<TargetInfo> prev_targets_;  // snapshot for EWMA rate computation
};

} // namespace f4::ai
