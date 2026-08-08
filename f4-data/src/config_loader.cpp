// f4-data/config_loader.cpp
//
// JSON serialization for AircraftConfig using nlohmann/json.
//
// This is the CANONICAL implementation of the AircraftConfig JSON format.
// f4-convert's json_io.cpp delegates to these functions (writeConfig /
// loadConfig / loadConfigFromString) rather than maintaining a duplicate
// copy — the previous "both copies MUST stay in sync" hazard was
// eliminated in the 2025 cleanup pass. f4-convert retains its own
// diffConfigs() helper because f4-data does no comparison, only I/O.
//
// Format: 1:1 mapping of AircraftConfig fields to JSON keys. Arrays of
// doubles are JSON arrays of numbers. The rawAuxAeroData map is a JSON
// object (key -> string) preserving verbatim .dat capture for lossless
// round-trip.

#include "f4/data/config_loader.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace f4::data {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Reader (deserialization)
// ---------------------------------------------------------------------------

namespace {

// Helper: read a double field with missing-key warning.
// j.value() silently defaults missing fields to the struct's default (often 0.0),
// which can hide config errors (e.g. a missing emptyWeight_lbs silently becomes
// 0 lbs). This wrapper emits a warning so the caller knows a field was defaulted.
template<typename T>
T value_with_warning(const json& j, const std::string& key, T default_val,
                     const std::string& section, std::vector<std::string>& warnings) {
    if (!j.contains(key)) {
        warnings.push_back(section + "." + key + " missing, defaulted to " +
                           std::to_string(default_val));
        return default_val;
    }
    return j.value(key, default_val);
}

// ---------------------------------------------------------------------------
// Field-list macros for X-macro read/write generation.
// Each entry is (json_key, struct_field, default_value).
// Adding a new scalar field requires editing only the corresponding list;
// fields with special handling (nested objects, arrays, validation) are
// kept outside the macro expansion.
// ---------------------------------------------------------------------------

// Fields that are plain double (use simple read/write)
#define F4_AUX_AERO_DOUBLE_FIELDS(X) \
    X("fuelFlowFactorNormal", fuelFlowFactorNormal, 0.25) \
    X("fuelFlowFactorAb",     fuelFlowFactorAb,     0.65) \
    X("minFuelFlow",          minFuelFlow,          1200.0) \
    X("normSpoolRate",        normSpoolRate,        0.7) \
    X("abSpoolRate",          abSpoolRate,          0.4) \
    X("jfsSpoolUpRate",       jfsSpoolUpRate,       10.0) \
    X("jfsSpoolUpLimit",      jfsSpoolUpLimit,      0.7) \
    X("lightupSpoolRate",     lightupSpoolRate,     10.0) \
    X("flameoutSpoolRate",    flameoutSpoolRate,    5.0) \
    X("jfsRechargeTime",      jfsRechargeTime,      60.0) \
    X("jfsMinRechargeRpm",    jfsMinRechargeRpm,    0.12) \
    X("jfsSpinTime",          jfsSpinTime,          240.0) \
    X("mainGenRpm",           mainGenRpm,           0.63) \
    X("stbyGenRpm",           stbyGenRpm,           0.60) \
    X("epuBurnTime",          epuBurnTime,          600.0) \
    X("tefRate",              tefRate,              1.0) \
    X("lefRate",              lefRate,              1.0) \
    X("tefTakeOff",           tefTakeOff,           20.0) \
    X("lefGround",            lefGround,            0.0) \
    X("CLtefFactor",          CLtefFactor,          0.05) \
    X("CDtefFactor",          CDtefFactor,          0.05) \
    X("CDlefFactor",          CDlefFactor,          0.05) \
    X("CDSPDBFactor",         CDSPDBFactor,         0.08) \
    X("CDLDGFactor",          CDLDGFactor,          0.06) \
    X("dragChuteCd",          dragChuteCd,          0.0) \
    X("area2Span",            area2Span,            0.1066) \
    X("rollMomentum",         rollMomentum,         1.0) \
    X("pitchMomentum",        pitchMomentum,        1.0) \
    X("yawMomentum",          yawMomentum,          1.0) \
    X("pitchElasticity",      pitchElasticity,      1.0) \
    X("sinkRate",             sinkRate,             15.0) \
    X("gearPitchFactor",      gearPitchFactor,      0.0) \
    X("rollGearGain",         rollGearGain,         0.6) \
    X("yawGearGain",          yawGearGain,          0.6) \
    X("pitchGearGain",        pitchGearGain,        0.8) \
    X("rollCouple",           rollCouple,           0.0)

// Fields that are bool
#define F4_AUX_AERO_BOOL_FIELDS(X) \
    X("hasLef",               hasLef,               false) \
    X("hasTef",               hasTef,               false) \
    X("elevatorRolls",        elevatorRolls,        false)

// Fields that are int
#define F4_AUX_AERO_INT_FIELDS(X) \
    X("nEngines",             nEngines,             1) \
    X("typeEngine",           typeEngine,           2)

// Angle fields stored as radians, JSON uses degrees (tefMaxAngle, lefMaxAngle, etc.)
#define F4_AUX_AERO_ANGLE_FIELDS(X) \
    X("tefMaxAngle",          tefMaxAngle,          20.0) \
    X("lefMaxAngle",          lefMaxAngle,          20.0) \
    X("rudderMaxAngle",       rudderMaxAngle,       30.0) \
    X("aileronMaxAngle",      aileronMaxAngle,      20.0) \
    X("airbrakeMaxAngle",     airbrakeMaxAngle,     60.0) \
    X("landingAOA",           landingAOA,           12.5) \
    X("criticalAOA",          criticalAOA,          0.0)

// Mach phantom-dimension field
#define F4_AUX_AERO_MACH_FIELDS(X) \
    X("lefMaxMach",           lefMaxMach,           1.0)

// Geometry double fields (maxGs is the only plain double)
#define F4_GEOMETRY_DOUBLE_FIELDS(X) \
    X("maxGs",            maxGs,            9.0)

// Geometry angle fields — JSON uses degrees, stored as radians
#define F4_GEOMETRY_ANGLE_FIELDS(X) \
    X("aoaMax_deg",       aoaMax,       25.0) \
    X("aoaMin_deg",       aoaMin,       -5.0) \
    X("betaMax_deg",      betaMax,      30.0) \
    X("betaMin_deg",      betaMin,      -30.0) \
    X("maxRoll_deg",      maxRoll,      80.0)

// Geometry CAS fields — JSON uses knots
#define F4_GEOMETRY_CAS_FIELDS(X) \
    X("minVcas_kts",      minVcas,      140.0) \
    X("maxVcas_kts",      maxVcas,      800.0) \
    X("cornerVcas_kts",   cornerVcas,   330.0)

// Geometry radian field — JSON uses radians
#define F4_GEOMETRY_RADIAN_FIELDS(X) \
    X("thetaMax_rad",     thetaMax,     1.4)

// Geometry feet fields — JSON uses feet
#define F4_GEOMETRY_FEET_FIELDS(X) \
    X("cgLoc_ft",         cgLoc,         0.0) \
    X("length_ft",        length,        0.0) \
    X("span_ft",          span,          0.0) \
    X("fusRadius_ft",     fusRadius,     0.0) \
    X("tailHt_ft",        tailHt,        0.0)

// Engine scalar fields only — the table arrays require .get<vector<double>>()
// handling, so they stay outside.
#define F4_ENGINE_SCALAR_FIELDS(X) \
    X("thrustFactor",   thrustFactor,   1.0) \
    X("fuelFlowFactor", fuelFlowFactor, 1.0)

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

void readGear(const json& j, GearPoint& g) {
    g.x     = f4::Quantity<f4::Feet>(j.at(0).get<double>());
    g.y     = f4::Quantity<f4::Feet>(j.at(1).get<double>());
    g.z     = f4::Quantity<f4::Feet>(j.at(2).get<double>());
    g.range = f4::Quantity<f4::Degrees>(j.at(3).get<double>()).to<f4::Radians>();
}

void readGeometry(const json& j, AircraftGeometry& g,
                  std::vector<std::string>& warnings) {
    // Warning-gated fields (kept outside the macro)
    g.emptyWeight  = f4::Quantity<f4::Pounds>(value_with_warning(j, "emptyWeight_lbs",  g.emptyWeight.value(),  "geometry", warnings));
    g.area         = f4::Quantity<f4::SquareFeet>(value_with_warning(j, "area_ft2",         g.area.value(),         "geometry", warnings));
    g.internalFuel = f4::Quantity<f4::Pounds>(value_with_warning(j, "internalFuel_lbs", g.internalFuel.value(), "geometry", warnings));
    g.maxFuel      = f4::Quantity<f4::Pounds>(value_with_warning(j, "maxFuel_lbs",      g.maxFuel.value(),      "geometry", warnings));

    // Plain double fields
#define F4_READ_FIELD(key, field, def) g.field = j.value(key, def);
    F4_GEOMETRY_DOUBLE_FIELDS(F4_READ_FIELD)
#undef F4_READ_FIELD

    // Angle fields — JSON is in degrees, stored as radians
#define F4_READ_ANGLE(key, field, def) g.field = f4::Quantity<f4::Degrees>(j.value(key, def)).to<f4::Radians>();
    F4_GEOMETRY_ANGLE_FIELDS(F4_READ_ANGLE)
#undef F4_READ_ANGLE

    // CAS fields — JSON is in knots
#define F4_READ_CAS(key, field, def) g.field = f4::Quantity<f4::CASKnots>(j.value(key, def));
    F4_GEOMETRY_CAS_FIELDS(F4_READ_CAS)
#undef F4_READ_CAS

    // Radian field — JSON is in radians
#define F4_READ_RAD(key, field, def) g.field = f4::Quantity<f4::Radians>(j.value(key, def));
    F4_GEOMETRY_RADIAN_FIELDS(F4_READ_RAD)
#undef F4_READ_RAD

    // Feet fields — JSON is in feet
#define F4_READ_FT(key, field, def) g.field = f4::Quantity<f4::Feet>(j.value(key, def));
    F4_GEOMETRY_FEET_FIELDS(F4_READ_FT)
#undef F4_READ_FT

    // Nested gear array
    if (j.contains("gear")) {
        g.gear.clear();
        for (const auto& gj : j.at("gear")) {
            GearPoint gp;
            readGear(gj, gp);
            g.gear.push_back(gp);
        }
    }
}

void readAux(const json& j, AuxAero& a) {
    // Plain double fields
#define F4_READ_FIELD(key, field, def) a.field = j.value(key, def);
    F4_AUX_AERO_DOUBLE_FIELDS(F4_READ_FIELD)
#undef F4_READ_FIELD

    // Bool fields
#define F4_READ_BOOL(key, field, def) a.field = j.value(key, def);
    F4_AUX_AERO_BOOL_FIELDS(F4_READ_BOOL)
#undef F4_READ_BOOL

    // Int fields
#define F4_READ_INT(key, field, def) a.field = j.value(key, def);
    F4_AUX_AERO_INT_FIELDS(F4_READ_INT)
#undef F4_READ_INT

    // Angle fields — JSON is in degrees, stored as radians
#define F4_READ_ANGLE(key, field, def) a.field = f4::Quantity<f4::Degrees>(j.value(key, def)).to<f4::Radians>();
    F4_AUX_AERO_ANGLE_FIELDS(F4_READ_ANGLE)
#undef F4_READ_ANGLE

    // Mach fields — JSON is a plain Mach number
#define F4_READ_MACH(key, field, def) a.field = f4::Quantity<f4::MachUnit>(j.value(key, def));
    F4_AUX_AERO_MACH_FIELDS(F4_READ_MACH)
#undef F4_READ_MACH
}

void readAero(const json& j, AeroTable& a) {
    if (j.contains("mach"))      a.mach      = j.at("mach").get<std::vector<double>>();
    if (j.contains("alpha_deg")) a.alpha_deg = j.at("alpha_deg").get<std::vector<double>>();
    if (j.contains("clift"))     a.clift     = j.at("clift").get<std::vector<double>>();
    if (j.contains("cdrag"))     a.cdrag     = j.at("cdrag").get<std::vector<double>>();
    if (j.contains("cy"))        a.cy        = j.at("cy").get<std::vector<double>>();
    a.clFactor = j.value("clFactor", a.clFactor);
    a.cdFactor = j.value("cdFactor", a.cdFactor);
    a.cyFactor = j.value("cyFactor", a.cyFactor);
}

void readEngine(const json& j, EngineTable& e) {
    // Scalar fields via field-list macro
#define F4_READ_FIELD(key, field, def) e.field = j.value(key, static_cast<decltype(e.field)>(def));
    F4_ENGINE_SCALAR_FIELDS(F4_READ_FIELD)
#undef F4_READ_FIELD

    // Table arrays (kept outside the macro)
    if (j.contains("alt_ft"))      e.alt_ft      = j.at("alt_ft").get<std::vector<double>>();
    if (j.contains("mach"))        e.mach        = j.at("mach").get<std::vector<double>>();
    if (j.contains("thrust_idle")) e.thrust_idle = j.at("thrust_idle").get<std::vector<double>>();
    if (j.contains("thrust_mil"))  e.thrust_mil  = j.at("thrust_mil").get<std::vector<double>>();
    if (j.contains("thrust_ab"))   e.thrust_ab   = j.at("thrust_ab").get<std::vector<double>>();
    if (j.contains("fuelflow_idle")) e.fuelflow_idle = j.at("fuelflow_idle").get<std::vector<double>>();
    if (j.contains("fuelflow_mil"))  e.fuelflow_mil  = j.at("fuelflow_mil").get<std::vector<double>>();
    if (j.contains("fuelflow_ab"))   e.fuelflow_ab   = j.at("fuelflow_ab").get<std::vector<double>>();
}

void readRollCmd(const json& j, RollCommandTable& r) {
    if (j.contains("alpha_deg")) r.alpha_deg = j.at("alpha_deg").get<std::vector<double>>();
    if (j.contains("qbar"))      r.qbar      = j.at("qbar").get<std::vector<double>>();
    if (j.contains("rollRate"))  r.rollRate  = j.at("rollRate").get<std::vector<double>>();
    r.scale = j.value("scale", r.scale);
}

void readLimiter(const json& j, Limiter& l) {
    l.type = static_cast<LimiterType>(j.value("type", static_cast<int>(l.type)));
    l.x1 = j.value("x1", l.x1);
    l.y1 = j.value("y1", l.y1);
    l.x2 = j.value("x2", l.x2);
    l.y2 = j.value("y2", l.y2);
    l.x0 = j.value("x0", l.x0);
    l.y0 = j.value("y0", l.y0);
}

} // namespace

LoadResult loadConfigFromString(const std::string& jsonStr) {
    LoadResult result;
    try {
        json j = json::parse(jsonStr);
        AircraftConfig& cfg = result.config;

        cfg.name        = j.value("name",        cfg.name);
        cfg.description = j.value("description", cfg.description);
        if (j.contains("geometry")) readGeometry(j.at("geometry"), cfg.geometry, result.warnings);
        if (j.contains("aux"))      readAux(j.at("aux"),           cfg.aux);
        if (j.contains("aero"))     readAero(j.at("aero"),         cfg.aero);
        if (j.contains("engine"))   readEngine(j.at("engine"),     cfg.engine);
        if (j.contains("rollCmd"))  readRollCmd(j.at("rollCmd"),   cfg.rollCmd);
        if (j.contains("limiters")) {
            const auto& lims = j.at("limiters");
            for (std::size_t i = 0; i < lims.size() && i < cfg.limiters.size(); ++i) {
                readLimiter(lims.at(i), cfg.limiters[i]);
            }
        }
        if (j.contains("rawAuxAeroData")) {
            cfg.rawAuxAeroData = j.at("rawAuxAeroData").get<std::map<std::string, std::string>>();
        }
        if (j.contains("aeroOptions"))   cfg.aeroOptions   = j.at("aeroOptions").get<std::vector<std::string>>();
        if (j.contains("engineOptions")) cfg.engineOptions = j.at("engineOptions").get<std::vector<std::string>>();
        cfg.sourceTitle    = j.value("sourceTitle",    cfg.sourceTitle);
        cfg.sourceAuthor   = j.value("sourceAuthor",   cfg.sourceAuthor);
        cfg.sourceRevision = j.value("sourceRevision", cfg.sourceRevision);
        cfg.sourceFile     = j.value("sourceFile",     cfg.sourceFile);

        result.ok = true;
    } catch (const std::exception& e) {
        result.ok = false;
        result.errors.push_back(e.what());
    }
    return result;
}

LoadResult loadConfig(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        LoadResult r;
        r.ok = false;
        r.errors.push_back("Could not open file: " + path);
        return r;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return loadConfigFromString(ss.str());
}

// ---------------------------------------------------------------------------
// Writer (serialization) — kept in sync with f4-convert's writer.
// ---------------------------------------------------------------------------

namespace {

json gearToJson(const GearPoint& g) {
    return json::array({g.x.value(), g.y.value(), g.z.value(), g.range.to<f4::Degrees>().value()});
}

json geometryToJson(const AircraftGeometry& g) {
    json j;
    // Warning-gated fields (kept outside the macro)
    j["emptyWeight_lbs"]    = g.emptyWeight.value();
    j["area_ft2"]           = g.area.value();
    j["internalFuel_lbs"]   = g.internalFuel.value();
    j["maxFuel_lbs"]        = g.maxFuel.value();

    // Plain double fields
#define F4_WRITE_FIELD(key, field, _def) j[key] = g.field;
    F4_GEOMETRY_DOUBLE_FIELDS(F4_WRITE_FIELD)
#undef F4_WRITE_FIELD

    // Angle fields — write degrees (JSON convention)
#define F4_WRITE_ANGLE(key, field, _def) j[key] = g.field.to<f4::Degrees>().value();
    F4_GEOMETRY_ANGLE_FIELDS(F4_WRITE_ANGLE)
#undef F4_WRITE_ANGLE

    // CAS fields — write knots
#define F4_WRITE_CAS(key, field, _def) j[key] = g.field.value();
    F4_GEOMETRY_CAS_FIELDS(F4_WRITE_CAS)
#undef F4_WRITE_CAS

    // Radian fields — write radians
#define F4_WRITE_RAD(key, field, _def) j[key] = g.field.value();
    F4_GEOMETRY_RADIAN_FIELDS(F4_WRITE_RAD)
#undef F4_WRITE_RAD

    // Feet fields — write feet
#define F4_WRITE_FT(key, field, _def) j[key] = g.field.value();
    F4_GEOMETRY_FEET_FIELDS(F4_WRITE_FT)
#undef F4_WRITE_FT

    // Nested gear array
    json gearArr = json::array();
    for (const auto& gp : g.gear) gearArr.push_back(gearToJson(gp));
    j["gear"] = gearArr;
    return j;
}

json auxToJson(const AuxAero& a) {
    json j;
    // Plain double fields
#define F4_WRITE_FIELD(key, field, _def) j[key] = a.field;
    F4_AUX_AERO_DOUBLE_FIELDS(F4_WRITE_FIELD)
#undef F4_WRITE_FIELD

    // Bool fields
#define F4_WRITE_BOOL(key, field, _def) j[key] = a.field;
    F4_AUX_AERO_BOOL_FIELDS(F4_WRITE_BOOL)
#undef F4_WRITE_BOOL

    // Int fields
#define F4_WRITE_INT(key, field, _def) j[key] = a.field;
    F4_AUX_AERO_INT_FIELDS(F4_WRITE_INT)
#undef F4_WRITE_INT

    // Angle fields — write degrees (JSON convention)
#define F4_WRITE_ANGLE(key, field, _def) j[key] = a.field.to<f4::Degrees>().value();
    F4_AUX_AERO_ANGLE_FIELDS(F4_WRITE_ANGLE)
#undef F4_WRITE_ANGLE

    // Mach fields — write plain Mach number
#define F4_WRITE_MACH(key, field, _def) j[key] = a.field.value();
    F4_AUX_AERO_MACH_FIELDS(F4_WRITE_MACH)
#undef F4_WRITE_MACH

    return j;
}

json aeroToJson(const AeroTable& a) {
    json j;
    j["mach"]     = a.mach;
    j["alpha_deg"] = a.alpha_deg;
    j["clift"]    = a.clift;
    j["cdrag"]    = a.cdrag;
    j["cy"]       = a.cy;
    j["clFactor"] = a.clFactor;
    j["cdFactor"] = a.cdFactor;
    j["cyFactor"] = a.cyFactor;
    return j;
}

json engineToJson(const EngineTable& e) {
    json j;
    // Scalar fields via field-list macro
#define F4_WRITE_FIELD(key, field, _def) j[key] = e.field;
    F4_ENGINE_SCALAR_FIELDS(F4_WRITE_FIELD)
#undef F4_WRITE_FIELD

    // Table arrays (kept outside the macro)
    j["alt_ft"]          = e.alt_ft;
    j["mach"]            = e.mach;
    j["thrust_idle"]     = e.thrust_idle;
    j["thrust_mil"]      = e.thrust_mil;
    j["thrust_ab"]       = e.thrust_ab;
    j["fuelflow_idle"]   = e.fuelflow_idle;
    j["fuelflow_mil"]    = e.fuelflow_mil;
    j["fuelflow_ab"]     = e.fuelflow_ab;
    return j;
}

json rollCmdToJson(const RollCommandTable& r) {
    json j;
    j["alpha_deg"] = r.alpha_deg;
    j["qbar"]      = r.qbar;
    j["rollRate"]  = r.rollRate;
    j["scale"]     = r.scale;
    return j;
}

json limiterToJson(const Limiter& l) {
    json j;
    j["type"] = static_cast<int>(l.type);
    j["x1"]   = l.x1;
    j["y1"]   = l.y1;
    j["x2"]   = l.x2;
    j["y2"]   = l.y2;
    j["x0"]   = l.x0;
    j["y0"]   = l.y0;
    return j;
}

} // namespace

std::string writeConfig(const AircraftConfig& cfg) {
    json j;
    j["name"]        = cfg.name;
    j["description"] = cfg.description;
    j["geometry"]    = geometryToJson(cfg.geometry);
    j["aux"]         = auxToJson(cfg.aux);
    j["aero"]        = aeroToJson(cfg.aero);
    j["engine"]      = engineToJson(cfg.engine);
    j["rollCmd"]     = rollCmdToJson(cfg.rollCmd);

    json limitersArr = json::array();
    for (const auto& l : cfg.limiters) limitersArr.push_back(limiterToJson(l));
    j["limiters"] = limitersArr;

    j["rawAuxAeroData"] = cfg.rawAuxAeroData;
    j["aeroOptions"]    = cfg.aeroOptions;
    j["engineOptions"]  = cfg.engineOptions;
    j["sourceTitle"]    = cfg.sourceTitle;
    j["sourceAuthor"]   = cfg.sourceAuthor;
    j["sourceRevision"] = cfg.sourceRevision;
    j["sourceFile"]     = cfg.sourceFile;
    return j.dump(2);
}

bool writeConfig(const AircraftConfig& cfg, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << writeConfig(cfg);
    return static_cast<bool>(f);
}

} // namespace f4::data
