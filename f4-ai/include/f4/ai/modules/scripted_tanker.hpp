// f4-ai/include/f4/ai/modules/scripted_tanker.hpp
//
// ScriptedTanker — a trivial "AI" that flies straight and level.
//
// This is NOT a real tanker AI. It exists so that the RefuelModule has
// something to refuel from — a tanker that maintains a constant heading,
// altitude, and speed. The entity's TransformComponent is updated each
// tick by the ScenarioRunner (or by the ScriptedTanker's own update).
//
// When a real tanker AI is built (part of WingmanModule / formation
// logic), this can be replaced. But for AR demos and integration tests,
// a scripted tanker is sufficient.
//
// Dependencies: f4-entities, f4-geo. C++20.

#pragma once

#include <cstdint>

#include <f4/entities/entity.hpp>
#include <f4/geo/position.hpp>

namespace f4::ai::modules {

class ScriptedTanker {
public:
    ScriptedTanker(
        std::uint64_t entity_id,
        const geo::WorldPosition& initial_position,
        double heading_rad,
        double altitude_msl_ft,
        double speed_kts)
        : entity_id_(entity_id)
        , position_(initial_position)
        , heading_rad_(heading_rad)
        , altitude_msl_ft_(altitude_msl_ft)
        , speed_kts_(speed_kts)
    {}

    // Advance the tanker along its track by one timestep.
    void update(double dt) {
        // Compute velocity in ENU frame:
        //   x = east  = speed * sin(heading)
        //   y = north = speed * cos(heading)
        const double speed_fps = speed_kts_ * 6076.12 / 3600.0;  // knots -> ft/s
        const double vx = speed_fps * std::sin(heading_rad_);
        const double vy = speed_fps * std::cos(heading_rad_);

        position_.x += vx * dt;
        position_.y += vy * dt;
        // z stays constant (level flight)
    }

    // Update the entity's TransformComponent in EntityWorld.
    void update_entity(entities::EntityWorld& world) const {
        entities::EntityHandle handle(
            entities::EntityId::make(
                static_cast<uint32_t>(entity_id_), 1u), &world);
        if (handle.valid()) {
            auto* transform = handle.get<entities::TransformComponent>();
            if (transform) {
                transform->position = position_;
            }
        }
    }

    // --- Accessors ---
    [[nodiscard]] std::uint64_t entity_id() const noexcept { return entity_id_; }
    [[nodiscard]] const geo::WorldPosition& position() const noexcept { return position_; }
    [[nodiscard]] double heading_rad() const noexcept { return heading_rad_; }
    [[nodiscard]] double altitude_msl_ft() const noexcept { return altitude_msl_ft_; }
    [[nodiscard]] double speed_kts() const noexcept { return speed_kts_; }

private:
    std::uint64_t entity_id_{0};
    geo::WorldPosition position_;
    double heading_rad_{0.0};
    double altitude_msl_ft_{0.0};
    double speed_kts_{250.0};
};

} // namespace f4::ai::modules
