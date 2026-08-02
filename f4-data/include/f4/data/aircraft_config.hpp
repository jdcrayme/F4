// f4-data/aircraft_config.hpp
//
// Data-only configuration describing an aircraft. This is the in-memory
// representation that f4-convert produces from .dat files and that
// f4-flight-model (and other consumers) read.
//
// All quantities are in Imperial units (ft, slugs, lb, knots, degrees for
// alpha/beta limits) to match the original coefficient tables. This matches
// the FreeFalcon source-of-truth .dat format.
//
// Ported from F4Flight's aircraft_config.h, which itself is a direct port
// of FreeFalcon's AeroDataSet / AuxAeroData / AirframeFcsRead structures.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace f4::data {

// ---------------------------------------------------------------------------
// Aerodynamic data: 2-D Mach x alpha coefficient tables.
//   - Mach is the outer (slow) axis.
//   - Alpha is the inner (fast) axis and is in DEGREES.
// Direct port of AeroData (arfrmdat.h).
// ---------------------------------------------------------------------------
struct AeroTable {
    std::vector<double> mach;       // numMach breakpoints
    std::vector<double> alpha_deg;  // numAlpha breakpoints (degrees)
    std::vector<double> clift;      // numMach * numAlpha, [mach*numAlpha + alpha]
    std::vector<double> cdrag;      // same layout (NOTE: original .dat multiplies by 1.5 on read; we expose the post-scale value)
    std::vector<double> cy;         // same layout

    double clFactor{1.0};
    double cdFactor{1.0};
    double cyFactor{1.0};
};

// ---------------------------------------------------------------------------
// Engine thrust / fuel-flow tables.
//   - Altitude is the outer (slow) axis, in FEET positive upward.
//   - Mach is the inner (fast) axis.
//   - Three power settings: idle, mil, afterburner.
// Direct port of EngineData (arfrmdat.h).
// ---------------------------------------------------------------------------
struct EngineTable {
    double thrustFactor{1.0};       // global thrust scale
    double fuelFlowFactor{1.0};     // legacy fuel flow scale

    std::vector<double> alt_ft;     // numAlt breakpoints (feet, positive upward)
    std::vector<double> mach;       // numMach breakpoints

    // thrust[i][alt*numMach + mach]
    // i = 0 idle, 1 mil, 2 afterburner
    std::vector<double> thrust_idle;
    std::vector<double> thrust_mil;
    std::vector<double> thrust_ab;

    // Optional fuel flow tables (same layout). If empty, a fallback factor
    // model is used.
    std::vector<double> fuelflow_idle;
    std::vector<double> fuelflow_mil;
    std::vector<double> fuelflow_ab;

    bool hasAB() const noexcept;
    bool hasFuelFlow() const noexcept;
};

// ---------------------------------------------------------------------------
// Roll-rate command table: 2-D alpha x qbar.
// Direct port of AirframeFcsRead / rollCmd.
// ---------------------------------------------------------------------------
struct RollCommandTable {
    std::vector<double> alpha_deg;
    std::vector<double> qbar;       // lb/ft^2
    std::vector<double> rollRate;   // deg/s, [alpha*numQbar + qbar]
    double scale{1.0};
};

// ---------------------------------------------------------------------------
// FCS limiter (line, three-point, value, min/max, percent).
// Port of limiters.h / limiters.cpp.
// ---------------------------------------------------------------------------
enum class LimiterType : int {
    Line        = 0,  // y = m x + b clipped to [x1, x2]
    Value       = 1,  // constant output
    Percent     = 2,  // multiplier
    ThreePoint  = 3,  // 3-point piecewise linear
    MinMax      = 4,  // hard clamp
};

enum class LimiterKey : int {
    NegGLimiter = 0,
    PosGLimiter,
    RollRateLimiter,
    YawAlphaLimiter,
    YawRollRateLimiter,
    CatIIICommandType,
    CatIIIAOALimiter,
    CatIIIRollRateLimiter,
    CatIIIYawAlphaLimiter,
    CatIIIYawRollRateLimiter,
    PitchYawControlDamper,
    RollControlDamper,
    CommandType,
    LowSpeedOmega,
    StoresDrag,
    LowSpeedPitchDown,
    CatIIIMaxGs,
    AOALimiter,
    Count
};

constexpr int kLimiterCount = static_cast<int>(LimiterKey::Count);

struct Limiter {
    LimiterType type{LimiterType::Line};
    // Line: x1 y1 x2 y2 (two endpoints)
    // ThreePoint: x0 y0 x1 y1 x2 y2
    // Value: x1 = value (only first used)
    // MinMax: x1 = min, x2 = max
    // Percent: x1 = percent
    double x1{0.0}, y1{0.0}, x2{0.0}, y2{0.0};
    double x0{0.0}, y0{0.0};

    /// Evaluate the limiter at input x. Direct port of the legacy Limiter
    /// subclasses (limiters.cpp).
    double limit(double x) const noexcept;
};

// ---------------------------------------------------------------------------
// Gear point: position in body axes (ft) + extension range.
// ---------------------------------------------------------------------------
struct GearPoint {
    double x{0.0};       // body X (+forward), ft
    double y{0.0};       // body Y (+right),  ft
    double z{0.0};       // body Z (+down), strut extended length, ft
    double range{0.0};   // deg, max extension angle (legacy field, unused in modern model)
};

// ---------------------------------------------------------------------------
// Reference input parameters.
// Direct port of AeroDataSet::inputData[] (arfrmdat.h).
// ---------------------------------------------------------------------------
struct AircraftGeometry {
    double emptyWeight_lbs{0.0};      // empty weight
    double area_ft2{0.0};             // wing reference area
    double internalFuel_lbs{0.0};     // internal fuel
    double maxFuel_lbs{0.0};          // optional explicit max (else = internalFuel)

    double aoaMax_deg{25.0};
    double aoaMin_deg{-5.0};
    double betaMax_deg{30.0};
    double betaMin_deg{-30.0};
    double maxGs{9.0};
    double maxRoll_deg{80.0};
    double minVcas_kts{140.0};
    double maxVcas_kts{800.0};
    double cornerVcas_kts{330.0};
    double thetaMax_rad{1.4};

    std::vector<GearPoint> gear;

    double cgLoc_ft{0.0};             // CG location from nose, ft
    double length_ft{0.0};            // fuselage length, ft
    double span_ft{0.0};              // wingspan, ft
    double fusRadius_ft{0.0};         // fuselage radius, ft
    double tailHt_ft{0.0};            // tail height, ft
};

// ---------------------------------------------------------------------------
// Auxiliary aero / engine parameters.
// Subset of the original AuxAeroData that is actually used by the flight
// model. Every field here is also present in rawAuxAeroData (below) as the
// verbatim .dat key/value, so the typed struct is a convenience view over
// the verbatim map, not a separate source of truth.
// ---------------------------------------------------------------------------
struct AuxAero {
    // Engine spool dynamics
    double fuelFlowFactorNormal{0.25};
    double fuelFlowFactorAb{0.65};
    double minFuelFlow{1200.0};
    double normSpoolRate{0.7};
    double abSpoolRate{0.4};
    double jfsSpoolUpRate{10.0};
    double jfsSpoolUpLimit{0.7};
    double lightupSpoolRate{10.0};
    double flameoutSpoolRate{5.0};
    double jfsRechargeTime{60.0};
    double jfsMinRechargeRpm{0.12};
    double jfsSpinTime{240.0};
    double mainGenRpm{0.63};
    double stbyGenRpm{0.60};
    double epuBurnTime{600.0};

    // Surfaces
    bool   hasLef{false};
    bool   hasTef{false};
    double tefMaxAngle{20.0};
    double lefMaxAngle{20.0};
    double tefRate{1.0};
    double lefRate{1.0};
    double tefTakeOff{20.0};
    double lefGround{0.0};
    double lefMaxMach{1.0};

    double rudderMaxAngle{30.0};
    double aileronMaxAngle{20.0};
    double airbrakeMaxAngle{60.0};

    // Aero factor contributions
    double CLtefFactor{0.05};
    double CDtefFactor{0.05};
    double CDlefFactor{0.05};
    double CDSPDBFactor{0.08};
    double CDLDGFactor{0.06};
    double dragChuteCd{0.0};
    double area2Span{0.1066};

    // Inertia / damping multipliers
    double rollMomentum{1.0};
    double pitchMomentum{1.0};
    double yawMomentum{1.0};
    double pitchElasticity{1.0};

    // Misc
    double sinkRate{15.0};
    double gearPitchFactor{0.0};
    double rollGearGain{0.6};
    double yawGearGain{0.6};
    double pitchGearGain{0.8};
    double landingAOA{12.5};
    double rollCouple{0.0};
    bool   elevatorRolls{false};
    double criticalAOA{0.0};

    int    nEngines{1};
    int    typeEngine{2};              // 1 PW100, 2 PW220, 3 PW229, 4 GE110, 5 GE129
};

// ---------------------------------------------------------------------------
// Validation result returned by AircraftConfig::validate().
// Aggregates ALL problems found (does not stop at the first one) so a host
// loading an aircraft data file can present a complete diagnostic in one pass.
// ---------------------------------------------------------------------------
struct ConfigValidationReport {
    enum class Severity { Warning, Error };

    struct Issue {
        Severity  severity;
        std::string field;   // dotted path, e.g. "aero.clift"
        std::string message;
    };

    std::vector<Issue> issues;

    bool ok() const noexcept;
    bool hasWarnings() const noexcept;
    std::size_t errorCount()   const noexcept;
    std::size_t warningCount() const noexcept;

    /// Human-readable multi-line summary (one line per issue, prefixed "E:" / "W:").
    std::string format() const;
};

// ---------------------------------------------------------------------------
// Top-level aircraft configuration. Combines all the above.
// ---------------------------------------------------------------------------
struct AircraftConfig {
    std::string name;
    std::string description;

    AircraftGeometry geometry;
    AuxAero          aux;
    AeroTable        aero;
    EngineTable      engine;
    RollCommandTable rollCmd;
    std::array<Limiter, kLimiterCount> limiters;

    // -----------------------------------------------------------------------
    // Verbatim .dat capture (the "no data loss" channel).
    //
    // rawAuxAeroData  : every `key value...` line from the AuxAeroData section
    //                   of the .dat file, stored as key -> exact value string.
    // aeroOptions     : the literal option names from every `aeropt <name>`
    //                   line, in file order.
    // engineOptions   : the literal option names from every `engopt <name>`
    //                   line, in file order.
    // sourceTitle / sourceAuthor / sourceRevision / sourceFile
    //                 : metadata from the .dat file header comments and the
    //                   file name.
    // -----------------------------------------------------------------------
    std::map<std::string, std::string> rawAuxAeroData;
    std::vector<std::string> aeroOptions;
    std::vector<std::string> engineOptions;
    std::string sourceTitle;
    std::string sourceAuthor;
    std::string sourceRevision;
    std::string sourceFile;

    // Convenience accessors for typed limiter access.
    const Limiter& limiter(LimiterKey key) const {
        return limiters[static_cast<int>(key)];
    }
    Limiter& limiter(LimiterKey key) {
        return limiters[static_cast<int>(key)];
    }
    void setLimiter(LimiterKey key, const Limiter& l) {
        limiters[static_cast<int>(key)] = l;
    }

    /// Validate the configuration. Returns a report with every problem found.
    /// `ok()` is true iff there are no Error-severity issues (warnings are
    /// reported but do not fail validation).
    ConfigValidationReport validate() const;
};

} // namespace f4::data
