// f4-convert/json_io.cpp
//
// JSON serialization + diff for AircraftConfig.
//
// Historically this file duplicated the JSON writer/reader from
// f4-data/src/config_loader.cpp — both copies had to be kept in sync
// manually, which was a maintenance hazard. As of the 2025 cleanup pass
// the duplication has been eliminated:
//
//   * writeJson()           delegates to f4::data::writeConfig()
//   * writeJsonFile()       delegates to f4::data::writeConfig(cfg, path)
//   * readJson()            delegates to f4::data::loadConfigFromString()
//   * readJsonFile()        delegates to f4::data::loadConfig()
//
// The IoResult → LoadResult adapter handles the structural difference
// (IoResult takes cfg by reference; LoadResult embeds it). The f4-convert
// public API is preserved for backward compatibility with existing callers
// (dat2json CLI, json_diff CLI, and the round-trip tests).
//
// What remains in this file is the diffConfigs() implementation, which has
// no equivalent in f4-data and is genuinely f4-convert-specific (used by
// the json_diff CLI tool and the round-trip test harness).

#include "f4/convert/json_io.hpp"
#include "f4/data/config_loader.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace f4::convert {

using f4::data::AircraftConfig;
using f4::data::AircraftGeometry;
using f4::data::AuxAero;
using f4::data::AeroTable;
using f4::data::EngineTable;
using f4::data::RollCommandTable;
using f4::data::Limiter;
using f4::data::LimiterType;
using f4::data::GearPoint;

// ============================================================================
// Writer / Reader — thin adapters over f4::data
// ============================================================================

std::string writeJson(const AircraftConfig& cfg) {
    return f4::data::writeConfig(cfg);
}

bool writeJsonFile(const AircraftConfig& cfg, const std::string& path) {
    return f4::data::writeConfig(cfg, path);
}

IoResult readJson(const std::string& jsonStr, AircraftConfig& cfg) {
    auto loaded = f4::data::loadConfigFromString(jsonStr);
    IoResult out;
    out.ok       = loaded.ok;
    out.errors   = std::move(loaded.errors);
    out.warnings = std::move(loaded.warnings);
    if (out.ok) {
        cfg = std::move(loaded.config);
    }
    return out;
}

IoResult readJsonFile(const std::string& path, AircraftConfig& cfg) {
    auto loaded = f4::data::loadConfig(path);
    IoResult out;
    out.ok       = loaded.ok;
    out.errors   = std::move(loaded.errors);
    out.warnings = std::move(loaded.warnings);
    if (out.ok) {
        cfg = std::move(loaded.config);
    }
    return out;
}

// ---------------------------------------------------------------------------
// diffConfigs — field-by-field comparison with floating-point tolerance.
// Used by the round-trip test harness and the json_diff CLI tool.
//
// This is the one piece of f4-convert that has NO equivalent in f4-data
// (f4-data only does I/O, not comparison). It stays here.
// ---------------------------------------------------------------------------

namespace {

bool eq(double a, double b, double tol) {
    if (a == b) return true;
    return std::fabs(a - b) <= tol;
}

void diffDouble(const std::string& name, double a, double b, double tol,
                std::vector<std::string>& diffs) {
    if (!eq(a, b, tol)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %-40s  %.12g  ->  %.12g", name.c_str(), a, b);
        diffs.push_back(buf);
    }
}

void diffInt(const std::string& name, int a, int b,
             std::vector<std::string>& diffs) {
    if (a != b) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %-40s  %d  ->  %d", name.c_str(), a, b);
        diffs.push_back(buf);
    }
}

void diffBool(const std::string& name, bool a, bool b,
              std::vector<std::string>& diffs) {
    if (a != b) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %-40s  %s  ->  %s",
                      name.c_str(), a ? "true" : "false", b ? "true" : "false");
        diffs.push_back(buf);
    }
}

void diffString(const std::string& name, const std::string& a, const std::string& b,
                std::vector<std::string>& diffs) {
    if (a != b) {
        diffs.push_back("  " + name);
        diffs.push_back("    - " + a);
        diffs.push_back("    + " + b);
    }
}

void diffDoubleArray(const std::string& name,
                     const std::vector<double>& a, const std::vector<double>& b,
                     double tol, std::vector<std::string>& diffs) {
    if (a.size() != b.size()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %-40s  size %zu  ->  %zu",
                      name.c_str(), a.size(), b.size());
        diffs.push_back(buf);
        return;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!eq(a[i], b[i], tol)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "  %s[%zu]  %.12g  ->  %.12g",
                          name.c_str(), i, a[i], b[i]);
            diffs.push_back(buf);
        }
    }
}

void diffStringVector(const std::string& name,
                      const std::vector<std::string>& a, const std::vector<std::string>& b,
                      std::vector<std::string>& diffs) {
    if (a.size() != b.size()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %-40s  size %zu  ->  %zu",
                      name.c_str(), a.size(), b.size());
        diffs.push_back(buf);
        return;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "  %s[%zu]  '%s'  ->  '%s'",
                          name.c_str(), i, a[i].c_str(), b[i].c_str());
            diffs.push_back(buf);
        }
    }
}

void diffStringMap(const std::string& name,
                   const std::map<std::string, std::string>& a,
                   const std::map<std::string, std::string>& b,
                   std::vector<std::string>& diffs) {
    if (a.size() != b.size()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %-40s  size %zu  ->  %zu",
                      name.c_str(), a.size(), b.size());
        diffs.push_back(buf);
    }
    for (const auto& [k, v] : a) {
        auto it = b.find(k);
        if (it == b.end()) {
            diffs.push_back("  " + name + "[" + k + "]  '" + v + "'  ->  (missing)");
        } else if (it->second != v) {
            diffs.push_back("  " + name + "[" + k + "]");
            diffs.push_back("    - " + v);
            diffs.push_back("    + " + it->second);
        }
    }
    for (const auto& [k, v] : b) {
        if (a.find(k) == a.end()) {
            diffs.push_back("  " + name + "[" + k + "]  (missing)  ->  '" + v + "'");
        }
    }
}

} // namespace

std::vector<std::string> diffConfigs(const AircraftConfig& a, const AircraftConfig& b,
                                      double tolerance) {
    std::vector<std::string> diffs;

    diffString("name",        a.name,        b.name,        diffs);
    diffString("description", a.description, b.description, diffs);
    diffString("sourceTitle",    a.sourceTitle,    b.sourceTitle,    diffs);
    diffString("sourceAuthor",   a.sourceAuthor,   b.sourceAuthor,   diffs);
    diffString("sourceRevision", a.sourceRevision, b.sourceRevision, diffs);
    diffString("sourceFile",     a.sourceFile,     b.sourceFile,     diffs);

    // Geometry
    diffDouble("geometry.emptyWeight_lbs",  a.geometry.emptyWeight.value(),  b.geometry.emptyWeight.value(),  tolerance, diffs);
    diffDouble("geometry.area_ft2",         a.geometry.area.value(),         b.geometry.area.value(),         tolerance, diffs);
    diffDouble("geometry.internalFuel_lbs", a.geometry.internalFuel.value(), b.geometry.internalFuel.value(), tolerance, diffs);
    diffDouble("geometry.maxFuel_lbs",      a.geometry.maxFuel.value(),      b.geometry.maxFuel.value(),      tolerance, diffs);
    diffDouble("geometry.aoaMax_deg",       a.geometry.aoaMax.to<f4::Degrees>().value(),       b.geometry.aoaMax.to<f4::Degrees>().value(),       tolerance, diffs);
    diffDouble("geometry.aoaMin_deg",       a.geometry.aoaMin.to<f4::Degrees>().value(),       b.geometry.aoaMin.to<f4::Degrees>().value(),       tolerance, diffs);
    diffDouble("geometry.betaMax_deg",      a.geometry.betaMax.to<f4::Degrees>().value(),      b.geometry.betaMax.to<f4::Degrees>().value(),      tolerance, diffs);
    diffDouble("geometry.betaMin_deg",      a.geometry.betaMin.to<f4::Degrees>().value(),      b.geometry.betaMin.to<f4::Degrees>().value(),      tolerance, diffs);
    diffDouble("geometry.maxGs",            a.geometry.maxGs,            b.geometry.maxGs,            tolerance, diffs);
    diffDouble("geometry.maxRoll_deg",      a.geometry.maxRoll.to<f4::Degrees>().value(),      b.geometry.maxRoll.to<f4::Degrees>().value(),      tolerance, diffs);
    diffDouble("geometry.minVcas_kts",      a.geometry.minVcas.value(),      b.geometry.minVcas.value(),      tolerance, diffs);
    diffDouble("geometry.maxVcas_kts",      a.geometry.maxVcas.value(),      b.geometry.maxVcas.value(),      tolerance, diffs);
    diffDouble("geometry.cornerVcas_kts",   a.geometry.cornerVcas.value(),   b.geometry.cornerVcas.value(),   tolerance, diffs);
    diffDouble("geometry.thetaMax_rad",     a.geometry.thetaMax.value(),     b.geometry.thetaMax.value(),     tolerance, diffs);
    diffDouble("geometry.cgLoc_ft",         a.geometry.cgLoc.value(),         b.geometry.cgLoc.value(),         tolerance, diffs);
    diffDouble("geometry.length_ft",        a.geometry.length.value(),        b.geometry.length.value(),        tolerance, diffs);
    diffDouble("geometry.span_ft",          a.geometry.span.value(),          b.geometry.span.value(),          tolerance, diffs);
    diffDouble("geometry.fusRadius_ft",     a.geometry.fusRadius.value(),     b.geometry.fusRadius.value(),     tolerance, diffs);
    diffDouble("geometry.tailHt_ft",        a.geometry.tailHt.value(),        b.geometry.tailHt.value(),        tolerance, diffs);

    if (a.geometry.gear.size() != b.geometry.gear.size()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  geometry.gear  size %zu  ->  %zu",
                      a.geometry.gear.size(), b.geometry.gear.size());
        diffs.push_back(buf);
    } else {
        for (std::size_t i = 0; i < a.geometry.gear.size(); ++i) {
            diffDouble("geometry.gear[" + std::to_string(i) + "].x",
                       a.geometry.gear[i].x.value(), b.geometry.gear[i].x.value(), tolerance, diffs);
            diffDouble("geometry.gear[" + std::to_string(i) + "].y",
                       a.geometry.gear[i].y.value(), b.geometry.gear[i].y.value(), tolerance, diffs);
            diffDouble("geometry.gear[" + std::to_string(i) + "].z",
                       a.geometry.gear[i].z.value(), b.geometry.gear[i].z.value(), tolerance, diffs);
            diffDouble("geometry.gear[" + std::to_string(i) + "].range",
                       a.geometry.gear[i].range.to<f4::Degrees>().value(), b.geometry.gear[i].range.to<f4::Degrees>().value(), tolerance, diffs);
        }
    }

    // Aux
    #define DIFF_AUX_DOUBLE(field) \
        diffDouble("aux." #field, a.aux.field, b.aux.field, tolerance, diffs)
    #define DIFF_AUX_ANGLE(field) \
        diffDouble("aux." #field, a.aux.field.to<f4::Degrees>().value(), b.aux.field.to<f4::Degrees>().value(), tolerance, diffs)
    #define DIFF_AUX_MACH(field) \
        diffDouble("aux." #field, a.aux.field.value(), b.aux.field.value(), tolerance, diffs)
    #define DIFF_AUX_INT(field) \
        diffInt("aux." #field, a.aux.field, b.aux.field, diffs)
    #define DIFF_AUX_BOOL(field) \
        diffBool("aux." #field, a.aux.field, b.aux.field, diffs)

    DIFF_AUX_DOUBLE(fuelFlowFactorNormal);
    DIFF_AUX_DOUBLE(fuelFlowFactorAb);
    DIFF_AUX_DOUBLE(minFuelFlow);
    DIFF_AUX_DOUBLE(normSpoolRate);
    DIFF_AUX_DOUBLE(abSpoolRate);
    DIFF_AUX_DOUBLE(jfsSpoolUpRate);
    DIFF_AUX_DOUBLE(jfsSpoolUpLimit);
    DIFF_AUX_DOUBLE(lightupSpoolRate);
    DIFF_AUX_DOUBLE(flameoutSpoolRate);
    DIFF_AUX_DOUBLE(jfsRechargeTime);
    DIFF_AUX_DOUBLE(jfsMinRechargeRpm);
    DIFF_AUX_DOUBLE(jfsSpinTime);
    DIFF_AUX_DOUBLE(mainGenRpm);
    DIFF_AUX_DOUBLE(stbyGenRpm);
    DIFF_AUX_DOUBLE(epuBurnTime);
    DIFF_AUX_BOOL(hasLef);
    DIFF_AUX_BOOL(hasTef);
    DIFF_AUX_ANGLE(tefMaxAngle);
    DIFF_AUX_ANGLE(lefMaxAngle);
    DIFF_AUX_DOUBLE(tefRate);
    DIFF_AUX_DOUBLE(lefRate);
    DIFF_AUX_DOUBLE(tefTakeOff);
    DIFF_AUX_DOUBLE(lefGround);
    DIFF_AUX_MACH(lefMaxMach);
    DIFF_AUX_ANGLE(rudderMaxAngle);
    DIFF_AUX_ANGLE(aileronMaxAngle);
    DIFF_AUX_ANGLE(airbrakeMaxAngle);
    DIFF_AUX_DOUBLE(CLtefFactor);
    DIFF_AUX_DOUBLE(CDtefFactor);
    DIFF_AUX_DOUBLE(CDlefFactor);
    DIFF_AUX_DOUBLE(CDSPDBFactor);
    DIFF_AUX_DOUBLE(CDLDGFactor);
    DIFF_AUX_DOUBLE(dragChuteCd);
    DIFF_AUX_DOUBLE(area2Span);
    DIFF_AUX_DOUBLE(rollMomentum);
    DIFF_AUX_DOUBLE(pitchMomentum);
    DIFF_AUX_DOUBLE(yawMomentum);
    DIFF_AUX_DOUBLE(pitchElasticity);
    DIFF_AUX_DOUBLE(sinkRate);
    DIFF_AUX_DOUBLE(gearPitchFactor);
    DIFF_AUX_DOUBLE(rollGearGain);
    DIFF_AUX_DOUBLE(yawGearGain);
    DIFF_AUX_DOUBLE(pitchGearGain);
    DIFF_AUX_ANGLE(landingAOA);
    DIFF_AUX_DOUBLE(rollCouple);
    DIFF_AUX_BOOL(elevatorRolls);
    DIFF_AUX_ANGLE(criticalAOA);
    DIFF_AUX_INT(nEngines);
    DIFF_AUX_INT(typeEngine);

    #undef DIFF_AUX_DOUBLE
    #undef DIFF_AUX_ANGLE
    #undef DIFF_AUX_MACH
    #undef DIFF_AUX_INT
    #undef DIFF_AUX_BOOL

    // Aero
    diffDoubleArray("aero.mach",     a.aero.mach,     b.aero.mach,     tolerance, diffs);
    diffDoubleArray("aero.alpha_deg",a.aero.alpha_deg,b.aero.alpha_deg,tolerance, diffs);
    diffDoubleArray("aero.clift",    a.aero.clift,    b.aero.clift,    tolerance, diffs);
    diffDoubleArray("aero.cdrag",    a.aero.cdrag,    b.aero.cdrag,    tolerance, diffs);
    diffDoubleArray("aero.cy",       a.aero.cy,       b.aero.cy,       tolerance, diffs);
    diffDouble("aero.clFactor", a.aero.clFactor, b.aero.clFactor, tolerance, diffs);
    diffDouble("aero.cdFactor", a.aero.cdFactor, b.aero.cdFactor, tolerance, diffs);
    diffDouble("aero.cyFactor", a.aero.cyFactor, b.aero.cyFactor, tolerance, diffs);

    // Engine
    diffDouble("engine.thrustFactor",   a.engine.thrustFactor,   b.engine.thrustFactor,   tolerance, diffs);
    diffDouble("engine.fuelFlowFactor", a.engine.fuelFlowFactor, b.engine.fuelFlowFactor, tolerance, diffs);
    diffDoubleArray("engine.alt_ft",      a.engine.alt_ft,      b.engine.alt_ft,      tolerance, diffs);
    diffDoubleArray("engine.mach",        a.engine.mach,        b.engine.mach,        tolerance, diffs);
    diffDoubleArray("engine.thrust_idle", a.engine.thrust_idle, b.engine.thrust_idle, tolerance, diffs);
    diffDoubleArray("engine.thrust_mil",  a.engine.thrust_mil,  b.engine.thrust_mil,  tolerance, diffs);
    diffDoubleArray("engine.thrust_ab",   a.engine.thrust_ab,   b.engine.thrust_ab,   tolerance, diffs);
    diffDoubleArray("engine.fuelflow_idle", a.engine.fuelflow_idle, b.engine.fuelflow_idle, tolerance, diffs);
    diffDoubleArray("engine.fuelflow_mil",  a.engine.fuelflow_mil,  b.engine.fuelflow_mil,  tolerance, diffs);
    diffDoubleArray("engine.fuelflow_ab",   a.engine.fuelflow_ab,   b.engine.fuelflow_ab,   tolerance, diffs);

    // Roll command
    diffDoubleArray("rollCmd.alpha_deg", a.rollCmd.alpha_deg, b.rollCmd.alpha_deg, tolerance, diffs);
    diffDoubleArray("rollCmd.qbar",      a.rollCmd.qbar,      b.rollCmd.qbar,      tolerance, diffs);
    diffDoubleArray("rollCmd.rollRate",  a.rollCmd.rollRate,  b.rollCmd.rollRate,  tolerance, diffs);
    diffDouble("rollCmd.scale", a.rollCmd.scale, b.rollCmd.scale, tolerance, diffs);

    // Limiters
    for (std::size_t i = 0; i < a.limiters.size() && i < b.limiters.size(); ++i) {
        std::string prefix = "limiters[" + std::to_string(i) + "]";
        diffInt(prefix + ".type", static_cast<int>(a.limiters[i].type), static_cast<int>(b.limiters[i].type), diffs);
        diffDouble(prefix + ".x1", a.limiters[i].x1, b.limiters[i].x1, tolerance, diffs);
        diffDouble(prefix + ".y1", a.limiters[i].y1, b.limiters[i].y1, tolerance, diffs);
        diffDouble(prefix + ".x2", a.limiters[i].x2, b.limiters[i].x2, tolerance, diffs);
        diffDouble(prefix + ".y2", a.limiters[i].y2, b.limiters[i].y2, tolerance, diffs);
        diffDouble(prefix + ".x0", a.limiters[i].x0, b.limiters[i].x0, tolerance, diffs);
        diffDouble(prefix + ".y0", a.limiters[i].y0, b.limiters[i].y0, tolerance, diffs);
    }

    // Verbatim capture
    diffStringMap("rawAuxAeroData", a.rawAuxAeroData, b.rawAuxAeroData, diffs);
    diffStringVector("aeroOptions",   a.aeroOptions,   b.aeroOptions,   diffs);
    diffStringVector("engineOptions", a.engineOptions, b.engineOptions, diffs);

    return diffs;
}

} // namespace f4::convert
