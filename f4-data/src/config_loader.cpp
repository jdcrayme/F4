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

#define F4_AUX_AERO_FIELDS(X) \
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
    X("hasLef",               hasLef,               false) \
    X("hasTef",               hasTef,               false) \
    X("tefMaxAngle",          tefMaxAngle,          20.0) \
    X("lefMaxAngle",          lefMaxAngle,          20.0) \
    X("tefRate",              tefRate,              1.0) \
    X("lefRate",              lefRate,              1.0) \
    X("tefTakeOff",           tefTakeOff,           20.0) \
    X("lefGround",            lefGround,            0.0) \
    X("lefMaxMach",           lefMaxMach,           1.0) \
    X("rudderMaxAngle",       rudderMaxAngle,       30.0) \
    X("aileronMaxAngle",      aileronMaxAngle,      20.0) \
    X("airbrakeMaxAngle",     airbrakeMaxAngle,     60.0) \
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
    X("landingAOA",           landingAOA,           12.5) \
    X("rollCouple",           rollCouple,           0.0) \
    X("elevatorRolls",        elevatorRolls,        false) \
    X("criticalAOA",          criticalAOA,          0.0) \
    X("nEngines",             nEngines,             1) \
    X("typeEngine",           typeEngine,           2)

// Geometry scalar fields only — the first four fields use value_with_warning
// and the gear field requires nested-object handling, so they stay outside.
#define F4_GEOMETRY_SCALAR_FIELDS(X) \
    X("aoaMax_deg",       aoaMax_deg,       25.0) \
    X("aoaMin_deg",       aoaMin_deg,       -5.0) \
    X("betaMax_deg",      betaMax_deg,      30.0) \
    X("betaMin_deg",      betaMin_deg,      -30.0) \
    X("maxGs",            maxGs,            9.0) \
    X("maxRoll_deg",      maxRoll_deg,      80.0) \
    X("minVcas_kts",      minVcas_kts,      140.0) \
    X("maxVcas_kts",      maxVcas_kts,      800.0) \
    X("cornerVcas_kts",   cornerVcas_kts,   330.0) \
    X("thetaMax_rad",     thetaMax_rad,     1.4) \
    X("cgLoc_ft",         cgLoc_ft,         0.0) \
    X("length_ft",        length_ft,        0.0) \
    X("span_ft",          span_ft,          0.0) \
    X("fusRadius_ft",     fusRadius_ft,     0.0) \
    X("tailHt_ft",        tailHt_ft,        0.0)

// Engine scalar fields only — the table arrays require .get<vector<double>>()
// handling, so they stay outside.
#define F4_ENGINE_SCALAR_FIELDS(X) \
    X("thrustFactor",   thrustFactor,   1.0) \
    X("fuelFlowFactor", fuelFlowFactor, 1.0)

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

void readGear(const json& j, GearPoint& g) {
    g.x     = j.at(0).get<double>();
    g.y     = j.at(1).get<double>();
    g.z     = j.at(2).get<double>();
    g.range = j.at(3).get<double>();
}

void readGeometry(const json& j, AircraftGeometry& g,
                  std::vector<std::string>& warnings) {
    // Warning-gated fields (kept outside the macro)
    g.emptyWeight_lbs  = value_with_warning(j, "emptyWeight_lbs",  g.emptyWeight_lbs,  "geometry", warnings);
    g.area_ft2         = value_with_warning(j, "area_ft2",         g.area_ft2,         "geometry", warnings);
    g.internalFuel_lbs = value_with_warning(j, "internalFuel_lbs", g.internalFuel_lbs, "geometry", warnings);
    g.maxFuel_lbs      = value_with_warning(j, "maxFuel_lbs",      g.maxFuel_lbs,      "geometry", warnings);

    // Scalar fields via field-list macro
#define F4_READ_FIELD(key, field, def) g.field = j.value(key, static_cast<decltype(g.field)>(def));
    F4_GEOMETRY_SCALAR_FIELDS(F4_READ_FIELD)
#undef F4_READ_FIELD

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
#define F4_READ_FIELD(key, field, def) a.field = j.value(key, static_cast<decltype(a.field)>(def));
    F4_AUX_AERO_FIELDS(F4_READ_FIELD)
#undef F4_READ_FIELD
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
    return json::array({g.x, g.y, g.z, g.range});
}

json geometryToJson(const AircraftGeometry& g) {
    json j;
    // Warning-gated fields (kept outside the macro)
    j["emptyWeight_lbs"]    = g.emptyWeight_lbs;
    j["area_ft2"]           = g.area_ft2;
    j["internalFuel_lbs"]   = g.internalFuel_lbs;
    j["maxFuel_lbs"]        = g.maxFuel_lbs;

    // Scalar fields via field-list macro
#define F4_WRITE_FIELD(key, field, _def) j[key] = g.field;
    F4_GEOMETRY_SCALAR_FIELDS(F4_WRITE_FIELD)
#undef F4_WRITE_FIELD

    // Nested gear array
    json gearArr = json::array();
    for (const auto& gp : g.gear) gearArr.push_back(gearToJson(gp));
    j["gear"] = gearArr;
    return j;
}

json auxToJson(const AuxAero& a) {
    json j;
#define F4_WRITE_FIELD(key, field, _def) j[key] = a.field;
    F4_AUX_AERO_FIELDS(F4_WRITE_FIELD)
#undef F4_WRITE_FIELD
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
