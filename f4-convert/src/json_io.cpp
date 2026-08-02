// f4-convert/json_io.cpp
//
// JSON serialization for AircraftConfig using nlohmann/json.
//
// The serialization is bidirectional and lossless: a config written to JSON
// and re-read produces a config that compares equal via diffConfigs() (up
// to floating-point representation). The rawAuxAeroData verbatim map is the
// authoritative "no data loss" channel.

#include "f4/convert/json_io.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

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

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

static json gearToJson(const GearPoint& g) {
    return json::array({g.x, g.y, g.z, g.range});
}

static json geometryToJson(const AircraftGeometry& g) {
    json j;
    j["emptyWeight_lbs"]    = g.emptyWeight_lbs;
    j["area_ft2"]           = g.area_ft2;
    j["internalFuel_lbs"]   = g.internalFuel_lbs;
    j["maxFuel_lbs"]        = g.maxFuel_lbs;
    j["aoaMax_deg"]         = g.aoaMax_deg;
    j["aoaMin_deg"]         = g.aoaMin_deg;
    j["betaMax_deg"]        = g.betaMax_deg;
    j["betaMin_deg"]        = g.betaMin_deg;
    j["maxGs"]              = g.maxGs;
    j["maxRoll_deg"]        = g.maxRoll_deg;
    j["minVcas_kts"]        = g.minVcas_kts;
    j["maxVcas_kts"]        = g.maxVcas_kts;
    j["cornerVcas_kts"]     = g.cornerVcas_kts;
    j["thetaMax_rad"]       = g.thetaMax_rad;
    j["cgLoc_ft"]           = g.cgLoc_ft;
    j["length_ft"]          = g.length_ft;
    j["span_ft"]            = g.span_ft;
    j["fusRadius_ft"]       = g.fusRadius_ft;
    j["tailHt_ft"]          = g.tailHt_ft;
    json gearArr = json::array();
    for (const auto& gp : g.gear) gearArr.push_back(gearToJson(gp));
    j["gear"] = gearArr;
    return j;
}

static json auxToJson(const AuxAero& a) {
    json j;
    j["fuelFlowFactorNormal"] = a.fuelFlowFactorNormal;
    j["fuelFlowFactorAb"]     = a.fuelFlowFactorAb;
    j["minFuelFlow"]          = a.minFuelFlow;
    j["normSpoolRate"]        = a.normSpoolRate;
    j["abSpoolRate"]          = a.abSpoolRate;
    j["jfsSpoolUpRate"]       = a.jfsSpoolUpRate;
    j["jfsSpoolUpLimit"]      = a.jfsSpoolUpLimit;
    j["lightupSpoolRate"]     = a.lightupSpoolRate;
    j["flameoutSpoolRate"]    = a.flameoutSpoolRate;
    j["jfsRechargeTime"]      = a.jfsRechargeTime;
    j["jfsMinRechargeRpm"]    = a.jfsMinRechargeRpm;
    j["jfsSpinTime"]          = a.jfsSpinTime;
    j["mainGenRpm"]           = a.mainGenRpm;
    j["stbyGenRpm"]           = a.stbyGenRpm;
    j["epuBurnTime"]          = a.epuBurnTime;
    j["hasLef"]               = a.hasLef;
    j["hasTef"]               = a.hasTef;
    j["tefMaxAngle"]          = a.tefMaxAngle;
    j["lefMaxAngle"]          = a.lefMaxAngle;
    j["tefRate"]              = a.tefRate;
    j["lefRate"]              = a.lefRate;
    j["tefTakeOff"]           = a.tefTakeOff;
    j["lefGround"]            = a.lefGround;
    j["lefMaxMach"]           = a.lefMaxMach;
    j["rudderMaxAngle"]       = a.rudderMaxAngle;
    j["aileronMaxAngle"]      = a.aileronMaxAngle;
    j["airbrakeMaxAngle"]     = a.airbrakeMaxAngle;
    j["CLtefFactor"]          = a.CLtefFactor;
    j["CDtefFactor"]          = a.CDtefFactor;
    j["CDlefFactor"]          = a.CDlefFactor;
    j["CDSPDBFactor"]         = a.CDSPDBFactor;
    j["CDLDGFactor"]          = a.CDLDGFactor;
    j["dragChuteCd"]          = a.dragChuteCd;
    j["area2Span"]            = a.area2Span;
    j["rollMomentum"]         = a.rollMomentum;
    j["pitchMomentum"]        = a.pitchMomentum;
    j["yawMomentum"]          = a.yawMomentum;
    j["pitchElasticity"]      = a.pitchElasticity;
    j["sinkRate"]             = a.sinkRate;
    j["gearPitchFactor"]      = a.gearPitchFactor;
    j["rollGearGain"]         = a.rollGearGain;
    j["yawGearGain"]          = a.yawGearGain;
    j["pitchGearGain"]        = a.pitchGearGain;
    j["landingAOA"]           = a.landingAOA;
    j["rollCouple"]           = a.rollCouple;
    j["elevatorRolls"]        = a.elevatorRolls;
    j["criticalAOA"]          = a.criticalAOA;
    j["nEngines"]             = a.nEngines;
    j["typeEngine"]           = a.typeEngine;
    return j;
}

static json aeroToJson(const AeroTable& a) {
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

static json engineToJson(const EngineTable& e) {
    json j;
    j["thrustFactor"]    = e.thrustFactor;
    j["fuelFlowFactor"]  = e.fuelFlowFactor;
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

static json rollCmdToJson(const RollCommandTable& r) {
    json j;
    j["alpha_deg"] = r.alpha_deg;
    j["qbar"]      = r.qbar;
    j["rollRate"]  = r.rollRate;
    j["scale"]     = r.scale;
    return j;
}

static json limiterToJson(const Limiter& l) {
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

std::string writeJson(const AircraftConfig& cfg) {
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

bool writeJsonFile(const AircraftConfig& cfg, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << writeJson(cfg);
    return static_cast<bool>(f);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

static void readGear(const json& j, GearPoint& g) {
    g.x     = j.at(0).get<double>();
    g.y     = j.at(1).get<double>();
    g.z     = j.at(2).get<double>();
    g.range = j.at(3).get<double>();
}

static void readGeometry(const json& j, AircraftGeometry& g) {
    g.emptyWeight_lbs  = j.value("emptyWeight_lbs",  g.emptyWeight_lbs);
    g.area_ft2         = j.value("area_ft2",         g.area_ft2);
    g.internalFuel_lbs = j.value("internalFuel_lbs", g.internalFuel_lbs);
    g.maxFuel_lbs      = j.value("maxFuel_lbs",      g.maxFuel_lbs);
    g.aoaMax_deg       = j.value("aoaMax_deg",       g.aoaMax_deg);
    g.aoaMin_deg       = j.value("aoaMin_deg",       g.aoaMin_deg);
    g.betaMax_deg      = j.value("betaMax_deg",      g.betaMax_deg);
    g.betaMin_deg      = j.value("betaMin_deg",      g.betaMin_deg);
    g.maxGs            = j.value("maxGs",            g.maxGs);
    g.maxRoll_deg      = j.value("maxRoll_deg",      g.maxRoll_deg);
    g.minVcas_kts      = j.value("minVcas_kts",      g.minVcas_kts);
    g.maxVcas_kts      = j.value("maxVcas_kts",      g.maxVcas_kts);
    g.cornerVcas_kts   = j.value("cornerVcas_kts",   g.cornerVcas_kts);
    g.thetaMax_rad     = j.value("thetaMax_rad",     g.thetaMax_rad);
    g.cgLoc_ft         = j.value("cgLoc_ft",         g.cgLoc_ft);
    g.length_ft        = j.value("length_ft",        g.length_ft);
    g.span_ft          = j.value("span_ft",          g.span_ft);
    g.fusRadius_ft     = j.value("fusRadius_ft",     g.fusRadius_ft);
    g.tailHt_ft        = j.value("tailHt_ft",        g.tailHt_ft);
    if (j.contains("gear")) {
        g.gear.clear();
        for (const auto& gj : j.at("gear")) {
            GearPoint gp;
            readGear(gj, gp);
            g.gear.push_back(gp);
        }
    }
}

static void readAux(const json& j, AuxAero& a) {
    a.fuelFlowFactorNormal = j.value("fuelFlowFactorNormal", a.fuelFlowFactorNormal);
    a.fuelFlowFactorAb     = j.value("fuelFlowFactorAb",     a.fuelFlowFactorAb);
    a.minFuelFlow          = j.value("minFuelFlow",          a.minFuelFlow);
    a.normSpoolRate        = j.value("normSpoolRate",        a.normSpoolRate);
    a.abSpoolRate          = j.value("abSpoolRate",          a.abSpoolRate);
    a.jfsSpoolUpRate       = j.value("jfsSpoolUpRate",       a.jfsSpoolUpRate);
    a.jfsSpoolUpLimit      = j.value("jfsSpoolUpLimit",      a.jfsSpoolUpLimit);
    a.lightupSpoolRate     = j.value("lightupSpoolRate",     a.lightupSpoolRate);
    a.flameoutSpoolRate    = j.value("flameoutSpoolRate",    a.flameoutSpoolRate);
    a.jfsRechargeTime      = j.value("jfsRechargeTime",      a.jfsRechargeTime);
    a.jfsMinRechargeRpm    = j.value("jfsMinRechargeRpm",    a.jfsMinRechargeRpm);
    a.jfsSpinTime          = j.value("jfsSpinTime",          a.jfsSpinTime);
    a.mainGenRpm           = j.value("mainGenRpm",           a.mainGenRpm);
    a.stbyGenRpm           = j.value("stbyGenRpm",           a.stbyGenRpm);
    a.epuBurnTime          = j.value("epuBurnTime",          a.epuBurnTime);
    a.hasLef               = j.value("hasLef",               a.hasLef);
    a.hasTef               = j.value("hasTef",               a.hasTef);
    a.tefMaxAngle          = j.value("tefMaxAngle",          a.tefMaxAngle);
    a.lefMaxAngle          = j.value("lefMaxAngle",          a.lefMaxAngle);
    a.tefRate              = j.value("tefRate",              a.tefRate);
    a.lefRate              = j.value("lefRate",              a.lefRate);
    a.tefTakeOff           = j.value("tefTakeOff",           a.tefTakeOff);
    a.lefGround             = j.value("lefGround",            a.lefGround);
    a.lefMaxMach           = j.value("lefMaxMach",           a.lefMaxMach);
    a.rudderMaxAngle       = j.value("rudderMaxAngle",       a.rudderMaxAngle);
    a.aileronMaxAngle      = j.value("aileronMaxAngle",      a.aileronMaxAngle);
    a.airbrakeMaxAngle     = j.value("airbrakeMaxAngle",     a.airbrakeMaxAngle);
    a.CLtefFactor          = j.value("CLtefFactor",          a.CLtefFactor);
    a.CDtefFactor          = j.value("CDtefFactor",          a.CDtefFactor);
    a.CDlefFactor          = j.value("CDlefFactor",          a.CDlefFactor);
    a.CDSPDBFactor         = j.value("CDSPDBFactor",         a.CDSPDBFactor);
    a.CDLDGFactor          = j.value("CDLDGFactor",          a.CDLDGFactor);
    a.dragChuteCd          = j.value("dragChuteCd",          a.dragChuteCd);
    a.area2Span            = j.value("area2Span",            a.area2Span);
    a.rollMomentum         = j.value("rollMomentum",         a.rollMomentum);
    a.pitchMomentum        = j.value("pitchMomentum",        a.pitchMomentum);
    a.yawMomentum          = j.value("yawMomentum",          a.yawMomentum);
    a.pitchElasticity      = j.value("pitchElasticity",      a.pitchElasticity);
    a.sinkRate             = j.value("sinkRate",             a.sinkRate);
    a.gearPitchFactor      = j.value("gearPitchFactor",      a.gearPitchFactor);
    a.rollGearGain         = j.value("rollGearGain",         a.rollGearGain);
    a.yawGearGain          = j.value("yawGearGain",          a.yawGearGain);
    a.pitchGearGain        = j.value("pitchGearGain",        a.pitchGearGain);
    a.landingAOA           = j.value("landingAOA",           a.landingAOA);
    a.rollCouple           = j.value("rollCouple",           a.rollCouple);
    a.elevatorRolls        = j.value("elevatorRolls",        a.elevatorRolls);
    a.criticalAOA          = j.value("criticalAOA",          a.criticalAOA);
    a.nEngines             = j.value("nEngines",             a.nEngines);
    a.typeEngine           = j.value("typeEngine",           a.typeEngine);
}

static void readAero(const json& j, AeroTable& a) {
    if (j.contains("mach"))      a.mach      = j.at("mach").get<std::vector<double>>();
    if (j.contains("alpha_deg")) a.alpha_deg = j.at("alpha_deg").get<std::vector<double>>();
    if (j.contains("clift"))     a.clift     = j.at("clift").get<std::vector<double>>();
    if (j.contains("cdrag"))     a.cdrag     = j.at("cdrag").get<std::vector<double>>();
    if (j.contains("cy"))        a.cy        = j.at("cy").get<std::vector<double>>();
    a.clFactor = j.value("clFactor", a.clFactor);
    a.cdFactor = j.value("cdFactor", a.cdFactor);
    a.cyFactor = j.value("cyFactor", a.cyFactor);
}

static void readEngine(const json& j, EngineTable& e) {
    e.thrustFactor   = j.value("thrustFactor",   e.thrustFactor);
    e.fuelFlowFactor = j.value("fuelFlowFactor", e.fuelFlowFactor);
    if (j.contains("alt_ft"))      e.alt_ft      = j.at("alt_ft").get<std::vector<double>>();
    if (j.contains("mach"))        e.mach        = j.at("mach").get<std::vector<double>>();
    if (j.contains("thrust_idle")) e.thrust_idle = j.at("thrust_idle").get<std::vector<double>>();
    if (j.contains("thrust_mil"))  e.thrust_mil  = j.at("thrust_mil").get<std::vector<double>>();
    if (j.contains("thrust_ab"))   e.thrust_ab   = j.at("thrust_ab").get<std::vector<double>>();
    if (j.contains("fuelflow_idle")) e.fuelflow_idle = j.at("fuelflow_idle").get<std::vector<double>>();
    if (j.contains("fuelflow_mil"))  e.fuelflow_mil  = j.at("fuelflow_mil").get<std::vector<double>>();
    if (j.contains("fuelflow_ab"))   e.fuelflow_ab   = j.at("fuelflow_ab").get<std::vector<double>>();
}

static void readRollCmd(const json& j, RollCommandTable& r) {
    if (j.contains("alpha_deg")) r.alpha_deg = j.at("alpha_deg").get<std::vector<double>>();
    if (j.contains("qbar"))      r.qbar      = j.at("qbar").get<std::vector<double>>();
    if (j.contains("rollRate"))  r.rollRate  = j.at("rollRate").get<std::vector<double>>();
    r.scale = j.value("scale", r.scale);
}

static void readLimiter(const json& j, Limiter& l) {
    l.type = static_cast<LimiterType>(j.value("type", static_cast<int>(l.type)));
    l.x1 = j.value("x1", l.x1);
    l.y1 = j.value("y1", l.y1);
    l.x2 = j.value("x2", l.x2);
    l.y2 = j.value("y2", l.y2);
    l.x0 = j.value("x0", l.x0);
    l.y0 = j.value("y0", l.y0);
}

IoResult readJson(const std::string& jsonStr, AircraftConfig& cfg) {
    IoResult result;
    try {
        json j = json::parse(jsonStr);

        cfg.name        = j.value("name",        cfg.name);
        cfg.description = j.value("description", cfg.description);
        if (j.contains("geometry")) readGeometry(j.at("geometry"), cfg.geometry);
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

IoResult readJsonFile(const std::string& path, AircraftConfig& cfg) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        IoResult r;
        r.ok = false;
        r.errors.push_back("Could not open file: " + path);
        return r;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return readJson(ss.str(), cfg);
}

// ---------------------------------------------------------------------------
// diffConfigs — field-by-field comparison with floating-point tolerance.
// Used by the round-trip test harness and the json_diff CLI tool.
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
    diffDouble("geometry.emptyWeight_lbs",  a.geometry.emptyWeight_lbs,  b.geometry.emptyWeight_lbs,  tolerance, diffs);
    diffDouble("geometry.area_ft2",         a.geometry.area_ft2,         b.geometry.area_ft2,         tolerance, diffs);
    diffDouble("geometry.internalFuel_lbs", a.geometry.internalFuel_lbs, b.geometry.internalFuel_lbs, tolerance, diffs);
    diffDouble("geometry.maxFuel_lbs",      a.geometry.maxFuel_lbs,      b.geometry.maxFuel_lbs,      tolerance, diffs);
    diffDouble("geometry.aoaMax_deg",       a.geometry.aoaMax_deg,       b.geometry.aoaMax_deg,       tolerance, diffs);
    diffDouble("geometry.aoaMin_deg",       a.geometry.aoaMin_deg,       b.geometry.aoaMin_deg,       tolerance, diffs);
    diffDouble("geometry.betaMax_deg",      a.geometry.betaMax_deg,      b.geometry.betaMax_deg,      tolerance, diffs);
    diffDouble("geometry.betaMin_deg",      a.geometry.betaMin_deg,      b.geometry.betaMin_deg,      tolerance, diffs);
    diffDouble("geometry.maxGs",            a.geometry.maxGs,            b.geometry.maxGs,            tolerance, diffs);
    diffDouble("geometry.maxRoll_deg",      a.geometry.maxRoll_deg,      b.geometry.maxRoll_deg,      tolerance, diffs);
    diffDouble("geometry.minVcas_kts",      a.geometry.minVcas_kts,      b.geometry.minVcas_kts,      tolerance, diffs);
    diffDouble("geometry.maxVcas_kts",      a.geometry.maxVcas_kts,      b.geometry.maxVcas_kts,      tolerance, diffs);
    diffDouble("geometry.cornerVcas_kts",   a.geometry.cornerVcas_kts,   b.geometry.cornerVcas_kts,   tolerance, diffs);
    diffDouble("geometry.thetaMax_rad",     a.geometry.thetaMax_rad,     b.geometry.thetaMax_rad,     tolerance, diffs);
    diffDouble("geometry.cgLoc_ft",         a.geometry.cgLoc_ft,         b.geometry.cgLoc_ft,         tolerance, diffs);
    diffDouble("geometry.length_ft",        a.geometry.length_ft,        b.geometry.length_ft,        tolerance, diffs);
    diffDouble("geometry.span_ft",          a.geometry.span_ft,          b.geometry.span_ft,          tolerance, diffs);
    diffDouble("geometry.fusRadius_ft",     a.geometry.fusRadius_ft,     b.geometry.fusRadius_ft,     tolerance, diffs);
    diffDouble("geometry.tailHt_ft",        a.geometry.tailHt_ft,        b.geometry.tailHt_ft,        tolerance, diffs);

    if (a.geometry.gear.size() != b.geometry.gear.size()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  geometry.gear  size %zu  ->  %zu",
                      a.geometry.gear.size(), b.geometry.gear.size());
        diffs.push_back(buf);
    } else {
        for (std::size_t i = 0; i < a.geometry.gear.size(); ++i) {
            diffDouble("geometry.gear[" + std::to_string(i) + "].x",
                       a.geometry.gear[i].x, b.geometry.gear[i].x, tolerance, diffs);
            diffDouble("geometry.gear[" + std::to_string(i) + "].y",
                       a.geometry.gear[i].y, b.geometry.gear[i].y, tolerance, diffs);
            diffDouble("geometry.gear[" + std::to_string(i) + "].z",
                       a.geometry.gear[i].z, b.geometry.gear[i].z, tolerance, diffs);
            diffDouble("geometry.gear[" + std::to_string(i) + "].range",
                       a.geometry.gear[i].range, b.geometry.gear[i].range, tolerance, diffs);
        }
    }

    // Aux
    #define DIFF_AUX_DOUBLE(field) \
        diffDouble("aux." #field, a.aux.field, b.aux.field, tolerance, diffs)
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
    DIFF_AUX_DOUBLE(tefMaxAngle);
    DIFF_AUX_DOUBLE(lefMaxAngle);
    DIFF_AUX_DOUBLE(tefRate);
    DIFF_AUX_DOUBLE(lefRate);
    DIFF_AUX_DOUBLE(tefTakeOff);
    DIFF_AUX_DOUBLE(lefGround);
    DIFF_AUX_DOUBLE(lefMaxMach);
    DIFF_AUX_DOUBLE(rudderMaxAngle);
    DIFF_AUX_DOUBLE(aileronMaxAngle);
    DIFF_AUX_DOUBLE(airbrakeMaxAngle);
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
    DIFF_AUX_DOUBLE(landingAOA);
    DIFF_AUX_DOUBLE(rollCouple);
    DIFF_AUX_BOOL(elevatorRolls);
    DIFF_AUX_DOUBLE(criticalAOA);
    DIFF_AUX_INT(nEngines);
    DIFF_AUX_INT(typeEngine);

    #undef DIFF_AUX_DOUBLE
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
