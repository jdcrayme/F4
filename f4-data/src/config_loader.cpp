// f4-data/config_loader.cpp
//
// JSON serialization for AircraftConfig using nlohmann/json.
//
// This is the SAME serialization format that f4-convert produces (they share
// the AircraftConfig struct). f4-convert has its own copy of the JSON I/O
// code (in f4-convert/src/json_io.cpp) because f4-convert must not depend on
// f4-data at build time — that would create a circular dependency
// (f4-convert generates the JSONs that f4-data loads, so f4-data can't
// depend on f4-convert; and f4-convert uses the AircraftConfig struct that
// f4-data owns, so f4-convert depends on f4-data).
//
// Both copies MUST stay in sync. If the JSON format changes, update both.
// The round-trip tests in both libraries verify this.

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

void readGear(const json& j, GearPoint& g) {
    g.x     = j.at(0).get<double>();
    g.y     = j.at(1).get<double>();
    g.z     = j.at(2).get<double>();
    g.range = j.at(3).get<double>();
}

void readGeometry(const json& j, AircraftGeometry& g) {
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

void readAux(const json& j, AuxAero& a) {
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

json auxToJson(const AuxAero& a) {
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
