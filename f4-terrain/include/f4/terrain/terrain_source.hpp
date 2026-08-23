// f4-terrain/include/f4/terrain/terrain_source.hpp
//
// TerrainSource — abstract interface for terrain elevation queries.
//
// The Simulation's tick loop calls elevation_at_ft(east_ft, north_ft) for
// each aircraft to set the flight model's ground plane. This decouples
// the sim from the terrain data format: the host provides a concrete
// TerrainSource (e.g. TerrainDataAdapter wrapping TerrainData, or a future
// procedural terrain generator, or a DIS network source) and the sim
// queries it without knowing how the heights are stored.
//
// The default implementation (FlatTerrainSource) returns a constant
// elevation — the pre-terrain behavior (ground at the parking spot's
// altitude). This keeps existing scenarios working unchanged.
//
// Path B1: the scenario player wraps f4::terrain::TerrainData in a
// TerrainDataAdapter and registers it via Simulation::set_terrain_source().
// The sim then updates each aircraft's ground Z every tick, so the F-16
// follows the real Korea elevation profile instead of sitting on a flat
// plane at the parking altitude.
//
// This header lives in f4-terrain (not f4-simulation) so the concrete
// TerrainDataAdapter can live alongside it without a dependency cycle
// (f4-simulation already depends on f4-terrain). The interface itself
// has zero dependencies — just raw doubles.
//
// C++20.

#pragma once

namespace f4::terrain {

/// Abstract terrain elevation source. The sim queries elevation_at_ft()
/// each tick for each aircraft to set the flight model's ground plane.
class TerrainSource {
public:
    virtual ~TerrainSource() = default;

    /// Return the terrain elevation (MSL, feet, positive up) at the
    /// given ENU position. The sim calls this every tick for every
    /// aircraft. Implementations should be O(1) — typically a grid
    /// lookup + bilinear interpolation.
    ///
    /// \param east_ft   ENU east coordinate (feet)
    /// \param north_ft  ENU north coordinate (feet)
    /// \return terrain elevation in feet MSL (positive up)
    [[nodiscard]] virtual double elevation_at_ft(double east_ft, double north_ft) const = 0;
};

/// Flat terrain — returns a constant elevation. The pre-terrain default
/// (ground at the parking spot's altitude). Used when no real terrain
/// is loaded.
class FlatTerrainSource final : public TerrainSource {
public:
    explicit FlatTerrainSource(double elevation_ft) : elevation_ft_(elevation_ft) {}

    [[nodiscard]] double elevation_at_ft(double /*east_ft*/, double /*north_ft*/) const override {
        return elevation_ft_;
    }

private:
    double elevation_ft_;
};

/// No terrain — elevation 0 everywhere. Used as the null default before
/// the host provides a real source.
class NullTerrainSource final : public TerrainSource {
public:
    [[nodiscard]] double elevation_at_ft(double, double) const override { return 0.0; }
};

} // namespace f4::terrain
