// f4-data/table_accessors.hpp
//
// Bridges AircraftConfig's raw vector<double> tables to f4-math's Table2D,
// which provides bilinear interpolation with cached-index optimization and
// three boundary modes.
//
// The raw vectors stay in AircraftConfig (they're the serialization format).
// These accessors build a Table2D view on demand. Callers should cache the
// returned Table2D rather than rebuilding it every frame — the construction
// copies the vectors (one-time cost) but the cached lookup is O(1).

#pragma once

#include "f4/data/aircraft_config.hpp"
#include "f4/math/table.hpp"

#include <optional>
#include <stdexcept>

namespace f4::data {

/// Build a Table2D view of the CL (lift coefficient) table.
/// Layout: rows = Mach breakpoints, cols = alpha (deg) breakpoints.
/// Bilinear interpolation with Clamp boundary mode (matches FF behaviour).
inline f4::math::Table2D<double, double, double> makeClTable(const AeroTable& a) {
    using FlatTag = typename f4::math::Table2D<double, double, double>::FlatDataTag;
    if (a.mach.empty() || a.alpha_deg.empty() || a.clift.empty()) {
        throw std::invalid_argument("makeClTable: aero table is empty");
    }
    if (a.clift.size() != a.mach.size() * a.alpha_deg.size()) {
        throw std::invalid_argument("makeClTable: clift size mismatch");
    }
    return f4::math::Table2D<double, double, double>(
        a.mach, a.alpha_deg, a.clift, FlatTag{}, f4::math::BoundaryMode::Clamp);
}

/// Build a Table2D view of the CD (drag coefficient) table.
inline f4::math::Table2D<double, double, double> makeCdTable(const AeroTable& a) {
    using FlatTag = typename f4::math::Table2D<double, double, double>::FlatDataTag;
    if (a.cdrag.empty()) {
        throw std::invalid_argument("makeCdTable: cdrag table is empty");
    }
    if (a.cdrag.size() != a.mach.size() * a.alpha_deg.size()) {
        throw std::invalid_argument("makeCdTable: cdrag size mismatch");
    }
    return f4::math::Table2D<double, double, double>(
        a.mach, a.alpha_deg, a.cdrag, FlatTag{}, f4::math::BoundaryMode::Clamp);
}

/// Build a Table2D view of the CY (side-force coefficient) table.
/// If the CY table is empty (some aircraft don't have one), returns an
/// empty optional — callers should treat missing CY as zero.
inline std::optional<f4::math::Table2D<double, double, double>>
makeCyTable(const AeroTable& a) {
    using FlatTag = typename f4::math::Table2D<double, double, double>::FlatDataTag;
    if (a.cy.empty()) return std::nullopt;
    if (a.cy.size() != a.mach.size() * a.alpha_deg.size()) {
        throw std::invalid_argument("makeCyTable: cy size mismatch");
    }
    return f4::math::Table2D<double, double, double>(
        a.mach, a.alpha_deg, a.cy, FlatTag{}, f4::math::BoundaryMode::Clamp);
}

/// Build a Table2D view of a thrust table (idle, mil, or ab).
/// Layout: rows = altitude (ft) breakpoints, cols = Mach breakpoints.
enum class ThrustTable { Idle, Mil, AB };
inline f4::math::Table2D<double, double, double>
makeThrustTable(const EngineTable& e, ThrustTable which) {
    using FlatTag = typename f4::math::Table2D<double, double, double>::FlatDataTag;
    const std::vector<double>* data = nullptr;
    switch (which) {
        case ThrustTable::Idle: data = &e.thrust_idle; break;
        case ThrustTable::Mil:  data = &e.thrust_mil;  break;
        case ThrustTable::AB:   data = &e.thrust_ab;   break;
    }
    if (data->empty()) {
        throw std::invalid_argument("makeThrustTable: requested table is empty");
    }
    if (e.alt_ft.empty() || e.mach.empty()) {
        throw std::invalid_argument("makeThrustTable: engine breakpoints are empty");
    }
    if (data->size() != e.alt_ft.size() * e.mach.size()) {
        throw std::invalid_argument("makeThrustTable: thrust size mismatch");
    }
    return f4::math::Table2D<double, double, double>(
        e.alt_ft, e.mach, *data, FlatTag{}, f4::math::BoundaryMode::Clamp);
}

/// Build a Table2D view of the roll-rate command table.
/// Layout: rows = alpha (deg) breakpoints, cols = qbar (lb/ft^2) breakpoints.
inline f4::math::Table2D<double, double, double>
makeRollRateTable(const RollCommandTable& r) {
    using FlatTag = typename f4::math::Table2D<double, double, double>::FlatDataTag;
    if (r.rollRate.empty()) {
        throw std::invalid_argument("makeRollRateTable: rollRate table is empty");
    }
    if (r.rollRate.size() != r.alpha_deg.size() * r.qbar.size()) {
        throw std::invalid_argument("makeRollRateTable: rollRate size mismatch");
    }
    return f4::math::Table2D<double, double, double>(
        r.alpha_deg, r.qbar, r.rollRate, FlatTag{}, f4::math::BoundaryMode::Clamp);
}

} // namespace f4::data
