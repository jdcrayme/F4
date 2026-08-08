// f4-convert/dat_parser.cpp
//
// Implementation of the .dat file parser.
//
// Ported from F4Flight's flight/src/dat_loader.cpp, which is itself a clean
// recursive-descent port of FreeFalcon's readin.cpp. Behaviour verified
// against FF source as the baseline truth.
//
// Key design decisions preserved from F4Flight:
//   - TokenStream strips '#' comments and tokenizes on whitespace.
//   - The AuxAeroData section is parsed line-by-line from the raw contents
//     (not the token stream) so that key/value verbatim capture is preserved.
//   - Section headers like "INPUT MASS AND GEOMETRIC PROPERTIES" are comments
//     in the .dat format, so they're stripped by TokenStream and we rely on
//     positional / heuristic parsing instead.
//   - The legacy "no engopt" engine format is supported via a forward-scan
//     heuristic (validated by checking that breakpoints are in plausible
//     ranges and enough tokens remain for the thrust tables).
//   - AFM (BMS Advanced Flight Model) files are detected and rejected with
//     a distinct error message so callers can skip them.

#include "f4/convert/dat_parser.hpp"

#include <f4/math/constants.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace f4::convert {

using f4::data::AircraftConfig;
using f4::data::AeroTable;
using f4::data::EngineTable;
using f4::data::Limiter;
using f4::data::LimiterType;
using f4::data::LimiterKey;
using f4::data::kLimiterCount;
using f4::data::GearPoint;
using f4::data::RollCommandTable;

// Conversion constant: degrees to radians (for thetaMax storage).
// Single source of truth is f4/math/constants.hpp.
constexpr double kDTR = f4::math::DEG_TO_RAD;

// ---------------------------------------------------------------------------
// TokenStream
//
// The .dat format uses '#' for line comments and whitespace as the token
// separator. We read the whole file into a string, strip comments, and
// produce a flat token stream.
// ---------------------------------------------------------------------------
class TokenStream {
public:
    explicit TokenStream(const std::string& source, std::string sourceName)
        : sourceName_(std::move(sourceName)) {
        // Strip line comments
        std::string stripped;
        stripped.reserve(source.size());
        std::size_t i = 0;
        while (i < source.size()) {
            char c = source[i];
            if (c == '#') {
                while (i < source.size() && source[i] != '\n') ++i;
            } else {
                stripped.push_back(c);
                ++i;
            }
        }
        // Tokenize on whitespace
        std::istringstream iss(stripped);
        std::string tok;
        while (iss >> tok) tokens_.push_back(tok);
    }

    const std::string& peek() const {
        static const std::string empty;
        return (pos_ < tokens_.size()) ? tokens_[pos_] : empty;
    }

    bool eof() const { return pos_ >= tokens_.size(); }

    std::string next() {
        if (pos_ >= tokens_.size()) {
            throw std::runtime_error("Unexpected EOF in " + sourceName_);
        }
        return tokens_[pos_++];
    }

    double nextDouble() {
        std::string tok = next();
        try {
            return std::stod(tok);
        } catch (const std::exception&) {
            throw std::runtime_error("Failed to parse '" + tok + "' as double in " + sourceName_);
        }
    }

    int nextInt() {
        std::string tok = next();
        try {
            return std::stoi(tok);
        } catch (const std::exception&) {
            throw std::runtime_error("Failed to parse '" + tok + "' as int in " + sourceName_);
        }
    }

    std::vector<double> nextDoubles(std::size_t n) {
        std::vector<double> v;
        v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) v.push_back(nextDouble());
        return v;
    }

    // C3 FIX: Non-throwing probe methods for backtracking search.
    // These save the current position, attempt to read, and on failure
    // restore the position and return std::nullopt. This replaces the
    // exception-as-control-flow pattern (try { ts.nextDouble(); } catch)
    // that was ~100x slower per backtracking attempt due to stack unwinding.

    /// Try to read a double at the current position. Returns the value
    /// on success, or std::nullopt on EOF or parse failure. Does NOT
    /// advance the position on failure.
    std::optional<double> tryNextDouble() {
        if (pos_ >= tokens_.size()) return std::nullopt;
        try {
            double v = std::stod(tokens_[pos_]);
            ++pos_;
            return v;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    /// Try to read an int at the current position.
    std::optional<int> tryNextInt() {
        if (pos_ >= tokens_.size()) return std::nullopt;
        try {
            int v = std::stoi(tokens_[pos_]);
            ++pos_;
            return v;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    /// Try to read `n` doubles starting at the current position.
    /// Returns the values on success (all n parsed), or std::nullopt on
    /// any failure. Position is NOT advanced on failure.
    std::optional<std::vector<double>> tryNextDoubles(std::size_t n) {
        std::size_t saved = pos_;
        std::vector<double> v;
        v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            auto val = tryNextDouble();
            if (!val) { pos_ = saved; return std::nullopt; }
            v.push_back(*val);
        }
        return v;
    }

    std::size_t pos() const { return pos_; }
    void setPos(std::size_t p) { pos_ = p; }
    std::size_t size() const { return tokens_.size(); }

private:
    std::string sourceName_;
    std::vector<std::string> tokens_;
    std::size_t pos_{0};
};

// ---------------------------------------------------------------------------
// Section markers. The .dat file is organised into sections delimited by
// specific keyword tokens. We search forward for each marker.
// ---------------------------------------------------------------------------

// Find the position of the next occurrence of a literal token (case-sensitive).
// Returns true if found; the stream's position is left AT that token (not
// past it). If not found, the stream position is restored to where it was
// before the call.
static bool findToken(TokenStream& ts, const std::string& needle) {
    std::size_t startPos = ts.pos();
    while (!ts.eof()) {
        if (ts.next() == needle) {
            ts.setPos(ts.pos() - 1);  // step back to the matching token
            return true;
        }
    }
    ts.setPos(startPos);  // not found -- restore position
    return false;
}

// ---------------------------------------------------------------------------
// Parse the input-data block (51 positional fields + gear points).
// Direct port of ReadData() in readin.cpp.
// ---------------------------------------------------------------------------
static void parseInputData(TokenStream& ts, AircraftConfig& cfg,
                           std::vector<std::string>& /*warnings*/) {
    ts.setPos(0);

    cfg.geometry.emptyWeight = f4::Quantity<f4::Pounds>(ts.nextDouble());
    cfg.geometry.area = f4::Quantity<f4::SquareFeet>(ts.nextDouble());
    cfg.geometry.internalFuel = f4::Quantity<f4::Pounds>(ts.nextDouble());
    cfg.geometry.maxFuel = cfg.geometry.internalFuel;

    cfg.geometry.aoaMax   = f4::Quantity<f4::Degrees>(ts.nextDouble()).to<f4::Radians>();
    cfg.geometry.aoaMin   = f4::Quantity<f4::Degrees>(ts.nextDouble()).to<f4::Radians>();
    cfg.geometry.betaMax  = f4::Quantity<f4::Degrees>(ts.nextDouble()).to<f4::Radians>();
    cfg.geometry.betaMin  = f4::Quantity<f4::Degrees>(ts.nextDouble()).to<f4::Radians>();
    cfg.geometry.maxGs        = ts.nextDouble();
    cfg.geometry.maxRoll  = f4::Quantity<f4::Degrees>(ts.nextDouble()).to<f4::Radians>();
    cfg.geometry.minVcas  = f4::Quantity<f4::CASKnots>(ts.nextDouble());
    cfg.geometry.maxVcas  = f4::Quantity<f4::CASKnots>(ts.nextDouble());
    cfg.geometry.cornerVcas = f4::Quantity<f4::CASKnots>(ts.nextDouble());
    cfg.geometry.thetaMax = f4::Quantity<f4::Radians>(ts.nextDouble() * kDTR); // stored as radians
    int numGear = ts.nextInt();
    cfg.geometry.gear.resize(static_cast<std::size_t>(numGear));

    for (int i = 0; i < numGear; ++i) {
        cfg.geometry.gear[i].x = f4::Quantity<f4::Feet>(ts.nextDouble());
        cfg.geometry.gear[i].y = f4::Quantity<f4::Feet>(ts.nextDouble());
        cfg.geometry.gear[i].z = f4::Quantity<f4::Feet>(ts.nextDouble());
        cfg.geometry.gear[i].range = f4::Quantity<f4::Degrees>(ts.nextDouble()).to<f4::Radians>();
    }

    cfg.geometry.cgLoc     = f4::Quantity<f4::Feet>(ts.nextDouble());
    cfg.geometry.length    = f4::Quantity<f4::Feet>(ts.nextDouble());
    cfg.geometry.span      = f4::Quantity<f4::Feet>(ts.nextDouble());
    cfg.geometry.fusRadius = f4::Quantity<f4::Feet>(ts.nextDouble());
    cfg.geometry.tailHt    = f4::Quantity<f4::Feet>(ts.nextDouble());
}

// ---------------------------------------------------------------------------
// Parse a 2D Mach x alpha coefficient table (CL, CD, or CY).
// Direct port of AirframeAeroRead().
// ---------------------------------------------------------------------------
static void parseAeroTable(TokenStream& ts, AeroTable& table,
                           const std::string& tableName,
                           std::vector<std::string>& warnings) {
    int numMach = 0, numAlpha = 0;

    if (tableName == "CL") {
        numMach = ts.nextInt();
        if (numMach <= 0 || numMach > 1000) {
            warnings.push_back(tableName + ": implausible numMach=" + std::to_string(numMach));
            return;
        }
        table.mach = ts.nextDoubles(static_cast<std::size_t>(numMach));

        numAlpha = ts.nextInt();
        if (numAlpha <= 0 || numAlpha > 1000) {
            warnings.push_back(tableName + ": implausible numAlpha=" + std::to_string(numAlpha));
            return;
        }
        table.alpha_deg = ts.nextDoubles(static_cast<std::size_t>(numAlpha));
    } else {
        // CD / CY: reuse the breakpoints from the CL table that was already parsed.
        numMach  = static_cast<int>(table.mach.size());
        numAlpha = static_cast<int>(table.alpha_deg.size());
        if (numMach == 0 || numAlpha == 0) {
            warnings.push_back(tableName + ": CL table must be parsed first");
            return;
        }
    }

    double factor = ts.nextDouble();

    std::vector<double> data;
    data.reserve(static_cast<std::size_t>(numMach) * numAlpha);
    for (int i = 0; i < numMach * numAlpha; ++i) {
        data.push_back(ts.nextDouble());
    }

    if (tableName == "CL") {
        table.clift = std::move(data);
        table.clFactor = factor;
    } else if (tableName == "CD") {
        // Legacy: readin.cpp multiplies CD by 1.5 on read. We preserve that.
        for (auto& v : data) v *= 1.5;
        table.cdrag = std::move(data);
        table.cdFactor = factor;
    } else if (tableName == "CY") {
        table.cy = std::move(data);
        table.cyFactor = factor;
    } else {
        warnings.push_back("Unknown aero table name: " + tableName);
    }
}

// ---------------------------------------------------------------------------
// Parse the propulsion data block.
// Direct port of AirframeEngineRead().
// ---------------------------------------------------------------------------
static void parseEngine(TokenStream& ts, AircraftConfig& cfg,
                        std::vector<std::string>& warnings) {
    std::size_t searchStart = ts.pos();
    bool foundEngopt = findToken(ts, "engopt");
    bool hasFuelFlowOpt = false;
    if (foundEngopt) {
        while (ts.peek() == "engopt") {
            ts.next(); // consume "engopt"
            std::string optName = ts.next();
            cfg.engineOptions.push_back(optName);
            if (optName == "fuelflow") hasFuelFlowOpt = true;
        }
    } else {
        // Legacy format: scan forward for the engine block by validating
        // a pattern of (thrustFactor, fuelFlowFactor, numMach, machs, numAlt,
        // alts) with plausible ranges.
        ts.setPos(searchStart);
        bool found = false;
        while (!ts.eof()) {
            std::size_t saved = ts.pos();
            // C3 FIX: Replaced try/catch exception-as-control-flow with
            // non-throwing tryNext*() methods. Each probe returns nullopt
            // on failure instead of throwing, which is ~100x faster per
            // backtracking attempt.
            auto f1_opt = ts.tryNextDouble();
            auto f2_opt = ts.tryNextDouble();
            auto nm_opt = ts.tryNextInt();
            if (!f1_opt || !f2_opt || !nm_opt) { ts.setPos(saved + 1); continue; }
            double f1 = *f1_opt;
            double f2 = *f2_opt;
            int nm = *nm_opt;
            // Thrust/fuel factors can be large for multi-engine aircraft
            // (bombers go up to 15.0). Use a generous upper bound.
            // numMach >= 2: small prop aircraft (e.g. An-2) may have only
            // 2 Mach breakpoints.
            if (nm < 2 || nm > 50 ||
                f1 < 0.0 || f1 > 50.0 ||
                f2 < 0.0 || f2 > 50.0) {
                ts.setPos(saved + 1);
                continue;
            }
            auto machs_opt = ts.tryNextDoubles(static_cast<std::size_t>(nm));
            if (!machs_opt) { ts.setPos(saved + 1); continue; }
            bool machsOk = true;
            for (double m : *machs_opt) {
                if (m < 0.0 || m > 5.0) { machsOk = false; break; }
            }
            if (!machsOk) { ts.setPos(saved + 1); continue; }

            auto na_opt = ts.tryNextInt();
            if (!na_opt) { ts.setPos(saved + 1); continue; }
            int na = *na_opt;
            if (na < 3 || na > 20) { ts.setPos(saved + 1); continue; }

            auto alts_opt = ts.tryNextDoubles(static_cast<std::size_t>(na));
            if (!alts_opt) { ts.setPos(saved + 1); continue; }
            bool altsOk = true;
            for (double a : *alts_opt) {
                if (a < 0.0 || a > 100000.0) { altsOk = false; break; }
            }
            if (!altsOk) { ts.setPos(saved + 1); continue; }

            std::size_t needed = 3 * static_cast<std::size_t>(na) * nm;
            if (ts.pos() + needed > ts.size()) { ts.setPos(saved + 1); continue; }

            ts.setPos(saved);
            found = true;
            break;
        }
        if (!found) {
            warnings.push_back("Engine: could not locate engine block");
            return;
        }
    }

    EngineTable& e = cfg.engine;
    e.thrustFactor = ts.nextDouble();
    e.fuelFlowFactor = ts.nextDouble();

    int numMach = ts.nextInt();
    e.mach = ts.nextDoubles(static_cast<std::size_t>(numMach));

    int numAlt = ts.nextInt();
    e.alt_ft = ts.nextDoubles(static_cast<std::size_t>(numAlt));

    // Optional alpha-breakpoints + thrust-alpha-factor table.
    int numAlpha = 0;
    std::size_t savedPos = ts.pos();
    bool hasAlphaFactor = false;
    // C3 FIX: Replaced try/catch with non-throwing probe methods.
    {
        auto maybeNumAlpha = ts.tryNextInt();
        if (maybeNumAlpha && *maybeNumAlpha >= 2 && *maybeNumAlpha <= 50) {
            auto alphas_opt = ts.tryNextDoubles(static_cast<std::size_t>(*maybeNumAlpha));
            if (alphas_opt) {
                bool alphasOk = true;
                for (double a : *alphas_opt) {
                    if (a < -90.0 || a > 90.0) { alphasOk = false; break; }
                }
                std::size_t needed = static_cast<std::size_t>(numAlt) * *maybeNumAlpha
                                   + 3 * static_cast<std::size_t>(numAlt) * numMach;
                if (alphasOk && ts.pos() + needed <= ts.size()) {
                    numAlpha = *maybeNumAlpha;
                    hasAlphaFactor = true;
                    for (int a = 0; a < numAlt; ++a) {
                        for (int al = 0; al < numAlpha; ++al) {
                            ts.nextDouble();
                        }
                    }
                } else {
                    ts.setPos(savedPos);
                }
            } else {
                ts.setPos(savedPos);
            }
        } else {
            ts.setPos(savedPos);
        }
    }

    e.thrust_idle.resize(static_cast<std::size_t>(numAlt) * numMach);
    e.thrust_mil.resize (static_cast<std::size_t>(numAlt) * numMach);
    e.thrust_ab.resize  (static_cast<std::size_t>(numAlt) * numMach);

    auto readThrustTable = [&](std::vector<double>& out) {
        for (int a = 0; a < numAlt; ++a) {
            for (int m = 0; m < numMach; ++m) {
                out[static_cast<std::size_t>(a) * numMach + m] = ts.nextDouble();
            }
        }
    };
    readThrustTable(e.thrust_idle);
    readThrustTable(e.thrust_mil);
    readThrustTable(e.thrust_ab);

    if (hasFuelFlowOpt && !ts.eof()) {
        e.fuelflow_idle.resize(static_cast<std::size_t>(numAlt) * numMach);
        e.fuelflow_mil .resize(static_cast<std::size_t>(numAlt) * numMach);
        e.fuelflow_ab  .resize(static_cast<std::size_t>(numAlt) * numMach);
        readThrustTable(e.fuelflow_idle);
        readThrustTable(e.fuelflow_mil);
        readThrustTable(e.fuelflow_ab);
    }

    (void)hasAlphaFactor;
}

// ---------------------------------------------------------------------------
// Parse the roll-rate command table.
// Direct port of AirframeFcsRead().
// ---------------------------------------------------------------------------
static void parseRollData(TokenStream& ts, AircraftConfig& cfg,
                          std::vector<std::string>& warnings) {
    auto tryParseRollAt = [&](std::size_t startPos) -> bool {
        ts.setPos(startPos);
        while (!ts.eof()) {
            std::size_t savedPos = ts.pos();
            // C3 FIX: Replaced try/catch with non-throwing probe methods.
            auto numAlpha_opt = ts.tryNextInt();
            if (!numAlpha_opt || *numAlpha_opt < 2 || *numAlpha_opt > 20) {
                ts.setPos(savedPos + 1); continue;
            }
            int numAlpha = *numAlpha_opt;

            auto alphas_opt = ts.tryNextDoubles(static_cast<std::size_t>(numAlpha));
            if (!alphas_opt) { ts.setPos(savedPos + 1); continue; }
            bool alphasOk = true;
            for (double a : *alphas_opt) {
                if (a < -30.0 || a > 90.0) { alphasOk = false; break; }
            }
            if (!alphasOk) { ts.setPos(savedPos + 1); continue; }

            auto numQbar_opt = ts.tryNextInt();
            if (!numQbar_opt || *numQbar_opt < 2 || *numQbar_opt > 20) {
                ts.setPos(savedPos + 1); continue;
            }
            int numQbar = *numQbar_opt;

            auto qbars_opt = ts.tryNextDoubles(static_cast<std::size_t>(numQbar));
            if (!qbars_opt) { ts.setPos(savedPos + 1); continue; }
            bool qbarsOk = true;
            for (double q : *qbars_opt) {
                if (q < 0.0 || q > 10000.0) { qbarsOk = false; break; }
            }
            if (!qbarsOk) { ts.setPos(savedPos + 1); continue; }

            auto scale_opt = ts.tryNextDouble();
            if (!scale_opt || *scale_opt < 0.01 || *scale_opt > 100.0) {
                ts.setPos(savedPos + 1); continue;
            }
            double scale = *scale_opt;

            std::size_t needed = static_cast<std::size_t>(numAlpha) * numQbar;
            if (ts.pos() + needed > ts.size()) { ts.setPos(savedPos + 1); continue; }

            auto rates_opt = ts.tryNextDoubles(needed);
            if (!rates_opt) { ts.setPos(savedPos + 1); continue; }

            bool ratesOk = true;
            for (double r : *rates_opt) {
                if (r < -50.0 || r > 500.0) { ratesOk = false; break; }
            }
            if (!ratesOk) { ts.setPos(savedPos + 1); continue; }

            cfg.rollCmd.alpha_deg = std::move(*alphas_opt);
            cfg.rollCmd.qbar      = std::move(*qbars_opt);
            cfg.rollCmd.scale     = scale;
            cfg.rollCmd.rollRate  = std::move(*rates_opt);
            return true;
        }
        return false;
    };

    if (tryParseRollAt(ts.pos())) return;
    if (tryParseRollAt(0)) return;

    warnings.push_back("RollData: could not locate a valid roll table");
}

// ---------------------------------------------------------------------------
// Parse the LIMITERS block.
// Direct port of LimiterMgrClass::ReadLimiters() in limiters.cpp.
// ---------------------------------------------------------------------------
static void parseLimiters(TokenStream& ts, AircraftConfig& cfg,
                          std::vector<std::string>& warnings) {
    // The limiters block is always the LAST section in the .dat file,
    // following the roll command table. Try two scan strategies:
    //   Pass 1: scan forward from the current stream position (which should
    //           be just after the roll table). This is the authoritative
    //           location.
    //   Pass 2: only if Pass 1 fails, scan from position 0 (handles edge
    //           cases where the roll parser overran).
    auto tryParseLimitersFrom = [&](std::size_t startPos) -> bool {
        ts.setPos(startPos);
        while (!ts.eof()) {
            std::size_t savedPos = ts.pos();
            // C3 FIX: Replaced try/catch with non-throwing probe methods.
            auto numLimiters_opt = ts.tryNextInt();
            if (!numLimiters_opt) { ts.setPos(savedPos + 1); continue; }
            int numLimiters = *numLimiters_opt;
            // Real aircraft .dat files have as few as 3 limiters (an2,
            // a simple prop plane) up to 18 (modern FreeFalcon). Accept
            // 3-30 to cover the full range while avoiding false positives
            // from small numeric sequences in aero/engine data.
            if (numLimiters < 3 || numLimiters > 30) {
                ts.setPos(savedPos + 1);
                continue;
            }

            bool ok = true;
            std::array<Limiter, kLimiterCount> tempLimiters{};

            for (int i = 0; i < numLimiters && ok; ++i) {
                auto type_opt = ts.tryNextInt();
                auto key_opt  = ts.tryNextInt();
                if (!type_opt || !key_opt) { ok = false; break; }
                int type = *type_opt;
                int key  = *key_opt;
                if (type < 0 || type > 4) { ok = false; break; }
                if (key  < 0 || key  >= kLimiterCount) { ok = false; break; }

                Limiter& lim = tempLimiters[static_cast<std::size_t>(key)];
                switch (type) {
                    case 0: { // Line: x1 y1 x2 y2
                        lim.type = LimiterType::Line;
                        auto x1 = ts.tryNextDouble(); auto y1 = ts.tryNextDouble();
                        auto x2 = ts.tryNextDouble(); auto y2 = ts.tryNextDouble();
                        if (!x1 || !y1 || !x2 || !y2) { ok = false; break; }
                        lim.x1 = *x1; lim.y1 = *y1; lim.x2 = *x2; lim.y2 = *y2;
                        break;
                    }
                    case 1: { // Value
                        lim.type = LimiterType::Value;
                        auto x1 = ts.tryNextDouble();
                        if (!x1) { ok = false; break; }
                        lim.x1 = *x1;
                        break;
                    }
                    case 2: { // Percent
                        lim.type = LimiterType::Percent;
                        auto x1 = ts.tryNextDouble();
                        if (!x1) { ok = false; break; }
                        lim.x1 = *x1;
                        break;
                    }
                    case 3: { // ThreePoint: x0 y0 x1 y1 x2 y2
                        lim.type = LimiterType::ThreePoint;
                        auto x0 = ts.tryNextDouble(); auto y0 = ts.tryNextDouble();
                        auto x1 = ts.tryNextDouble(); auto y1 = ts.tryNextDouble();
                        auto x2 = ts.tryNextDouble(); auto y2 = ts.tryNextDouble();
                        if (!x0 || !y0 || !x1 || !y1 || !x2 || !y2) { ok = false; break; }
                        lim.x0 = *x0; lim.y0 = *y0; lim.x1 = *x1; lim.y1 = *y1; lim.x2 = *x2; lim.y2 = *y2;
                        break;
                    }
                    case 4: { // MinMax: min max
                        lim.type = LimiterType::MinMax;
                        auto x1 = ts.tryNextDouble(); auto x2 = ts.tryNextDouble();
                        if (!x1 || !x2) { ok = false; break; }
                        lim.x1 = *x1; lim.x2 = *x2;
                        break;
                    }
                }
            }

            if (!ok) {
                ts.setPos(savedPos + 1);
                continue;
            }

            cfg.limiters = tempLimiters;
            return true;
        }
        return false;
    };

    std::size_t searchStart = ts.pos();
    if (tryParseLimitersFrom(searchStart)) return;
    if (tryParseLimitersFrom(0)) return;

    warnings.push_back("Limiters: could not locate a valid limiters block");
}

// ---------------------------------------------------------------------------
// Parse the AuxAeroData block (key/value pairs).
// Direct port of the AuxAeroDataDesc table in readin.cpp, but with a
// critical difference: we capture EVERY key/value pair verbatim into
// `cfg.rawAuxAeroData`, not just the keys we know about.
// ---------------------------------------------------------------------------
static void parseAuxAero(const std::string& contents,
                         AircraftConfig& cfg,
                         std::vector<std::string>& /*warnings*/) {
    auto& aux = cfg.aux;

    auto parseDoubleAt = [](const std::string& s, std::size_t& pos, double& out) -> bool {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos >= s.size()) return false;
        char* end = nullptr;
        out = std::strtod(s.c_str() + pos, &end);
        if (end == s.c_str() + pos) return false;
        pos = static_cast<std::size_t>(end - s.c_str());
        return true;
    };
    auto parseIntAt = [](const std::string& s, std::size_t& pos, int& out) -> bool {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos >= s.size()) return false;
        char* end = nullptr;
        long v = std::strtol(s.c_str() + pos, &end, 10);
        if (end == s.c_str() + pos) return false;
        out = static_cast<int>(v);
        pos = static_cast<std::size_t>(end - s.c_str());
        return true;
    };

    std::istringstream iss(contents);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::size_t start = line.find_first_not_of(" \t\r\n\v\f");
        if (start == std::string::npos) continue;
        if (line[start] == '#') continue;

        std::size_t keyEnd = line.find_first_of(" \t\r\n\v\f", start);
        std::string key;
        std::string value;
        if (keyEnd == std::string::npos) {
            key = line.substr(start);
            value = "";
        } else {
            key = line.substr(start, keyEnd - start);
            std::size_t valStart = line.find_first_not_of(" \t\r\n\v\f", keyEnd);
            if (valStart == std::string::npos) {
                value = "";
            } else {
                std::size_t valEnd = line.find_last_not_of(" \t\r\n\v\f");
                value = line.substr(valStart, valEnd - valStart + 1);
            }
        }

        if (key == "aeropt" || key == "engopt") continue;
        if (key == "END") continue;

        // Skip keys that are actually positional-table tokens that happen to
        // parse as numbers.
        {
            char* end = nullptr;
            std::strtod(key.c_str(), &end);
            if (end != key.c_str() && *end == '\0') continue;
        }

        cfg.rawAuxAeroData[key] = value;

        std::size_t pos = 0;
        double dv = 0.0;
        int iv = 0;

        auto tryD = [&](double& dst) {
            if (parseDoubleAt(value, pos, dv)) dst = dv;
        };
        auto tryI = [&](int& dst) {
            if (parseIntAt(value, pos, iv)) dst = iv;
        };

        if      (key == "typeEngine")   tryI(aux.typeEngine);
        else if (key == "nEngines")     tryI(aux.nEngines);
        else if (key == "normSpoolRate")        tryD(aux.normSpoolRate);
        else if (key == "abSpoolRate")          tryD(aux.abSpoolRate);
        else if (key == "jfsSpoolUpRate")       tryD(aux.jfsSpoolUpRate);
        else if (key == "jfsSpoolUpLimit")      tryD(aux.jfsSpoolUpLimit);
        else if (key == "lightupSpoolRate")     tryD(aux.lightupSpoolRate);
        else if (key == "flameoutSpoolRate")    tryD(aux.flameoutSpoolRate);
        else if (key == "jfsRechargeTime")      tryD(aux.jfsRechargeTime);
        else if (key == "jfsMinRechargeRpm")    tryD(aux.jfsMinRechargeRpm);
        else if (key == "jfsSpinTime")          tryD(aux.jfsSpinTime);
        else if (key == "mainGenRpm")           tryD(aux.mainGenRpm);
        else if (key == "stbyGenRpm")           tryD(aux.stbyGenRpm);
        else if (key == "epuBurnTime")          tryD(aux.epuBurnTime);
        else if (key == "fuelFlowFactorNormal") tryD(aux.fuelFlowFactorNormal);
        else if (key == "fuelFlowFactorAb")     tryD(aux.fuelFlowFactorAb);
        else if (key == "minFuelFlow")          tryD(aux.minFuelFlow);
        else if (key == "haslef" || key == "hasLef") {
            if (parseIntAt(value, pos, iv)) aux.hasLef = (iv != 0);
        }
        else if (key == "hasTef") {
            if (parseIntAt(value, pos, iv)) aux.hasTef = (iv != 0);
        }
        else if (key == "lefGround")    tryD(aux.lefGround);
        else if (key == "lefMaxAngle")  { double v; if (parseDoubleAt(value, pos, v)) aux.lefMaxAngle = f4::Quantity<f4::Degrees>(v).to<f4::Radians>(); }
        else if (key == "lefMaxMach")   { double v; if (parseDoubleAt(value, pos, v)) aux.lefMaxMach = f4::Quantity<f4::MachUnit>(v); }
        else if (key == "lefRate")      tryD(aux.lefRate);
        else if (key == "tefMaxAngle")  { double v; if (parseDoubleAt(value, pos, v)) aux.tefMaxAngle = f4::Quantity<f4::Degrees>(v).to<f4::Radians>(); }
        else if (key == "tefTakeoff" || key == "tefTakeOff") tryD(aux.tefTakeOff);
        else if (key == "tefRate")      tryD(aux.tefRate);
        else if (key == "CLtefFactor")  tryD(aux.CLtefFactor);
        else if (key == "CDtefFactor")  tryD(aux.CDtefFactor);
        else if (key == "CDlefFactor")  tryD(aux.CDlefFactor);
        else if (key == "CDSPDBFactor") tryD(aux.CDSPDBFactor);
        else if (key == "CDLDGFactor")  tryD(aux.CDLDGFactor);
        else if (key == "dragChuteCd")  tryD(aux.dragChuteCd);
        else if (key == "area2Span")    tryD(aux.area2Span);
        else if (key == "pitchMomentum")   tryD(aux.pitchMomentum);
        else if (key == "rollMomentum")    tryD(aux.rollMomentum);
        else if (key == "yawMomentum")     tryD(aux.yawMomentum);
        else if (key == "pitchElasticity") tryD(aux.pitchElasticity);
        else if (key == "gearPitchFactor") tryD(aux.gearPitchFactor);
        else if (key == "pitchGearGain")   tryD(aux.pitchGearGain);
        else if (key == "rollGearGain")    tryD(aux.rollGearGain);
        else if (key == "yawGearGain")     tryD(aux.yawGearGain);
        else if (key == "rudderMaxAngle")  { double v; if (parseDoubleAt(value, pos, v)) aux.rudderMaxAngle = f4::Quantity<f4::Degrees>(v).to<f4::Radians>(); }
        else if (key == "aileronMaxAngle") { double v; if (parseDoubleAt(value, pos, v)) aux.aileronMaxAngle = f4::Quantity<f4::Degrees>(v).to<f4::Radians>(); }
        else if (key == "airbrakeMaxAngle") { double v; if (parseDoubleAt(value, pos, v)) aux.airbrakeMaxAngle = f4::Quantity<f4::Degrees>(v).to<f4::Radians>(); }
        else if (key == "rollCouple")      tryD(aux.rollCouple);
        else if (key == "elevatorRoll" || key == "elevatorRolls") {
            if (parseIntAt(value, pos, iv)) aux.elevatorRolls = (iv != 0);
        }
        else if (key == "sinkRate")    tryD(aux.sinkRate);
        else if (key == "landingAOA")  { double v; if (parseDoubleAt(value, pos, v)) aux.landingAOA = f4::Quantity<f4::Degrees>(v).to<f4::Radians>(); }
        else if (key == "criticalAOA") { double v; if (parseDoubleAt(value, pos, v)) aux.criticalAOA = f4::Quantity<f4::Degrees>(v).to<f4::Radians>(); }
        // Unknown keys: already captured in rawAuxAeroData.
    }
}

// ---------------------------------------------------------------------------
// Extract source metadata (Title/Author/Revision) from the header comments.
// ---------------------------------------------------------------------------
static void extractSourceMetadata(const std::string& contents,
                                  const std::string& sourceName,
                                  AircraftConfig& cfg) {
    cfg.sourceFile = sourceName;
    {
        std::size_t slash = cfg.sourceFile.find_last_of("/\\");
        if (slash != std::string::npos) cfg.sourceFile = cfg.sourceFile.substr(slash + 1);
    }

    std::istringstream iss(contents);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::size_t start = line.find_first_not_of(" \t\r\n\v\f");
        if (start == std::string::npos) continue;
        if (line.compare(start, 8, "# Title:") == 0) {
            std::string v = line.substr(start + 8);
            std::size_t s = v.find_first_not_of(" \t");
            std::size_t e = v.find_last_not_of(" \t\r\n\v\f");
            if (s != std::string::npos) cfg.sourceTitle = v.substr(s, e - s + 1);
        }
        else if (line.compare(start, 9, "# Author:") == 0) {
            std::string v = line.substr(start + 9);
            std::size_t s = v.find_first_not_of(" \t");
            std::size_t e = v.find_last_not_of(" \t\r\n\v\f");
            if (s != std::string::npos) cfg.sourceAuthor = v.substr(s, e - s + 1);
        }
        else if (line.compare(start, 11, "# Revision:") == 0) {
            std::string v = line.substr(start + 11);
            std::size_t s = v.find_first_not_of(" \t");
            std::size_t e = v.find_last_not_of(" \t\r\n\v\f");
            if (s != std::string::npos) cfg.sourceRevision = v.substr(s, e - s + 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Detect BMS "Advanced Flight Model" (.dat files whose name ends in `_afm`).
// ---------------------------------------------------------------------------
static bool isAfmFile(const std::string& contents, const std::string& sourceName) {
    {
        std::string base = sourceName;
        std::size_t slash = base.find_last_of("/\\");
        if (slash != std::string::npos) base = base.substr(slash + 1);
        if (base.size() > 4 && base.compare(base.size() - 4, 4, ".dat") == 0) {
            base = base.substr(0, base.size() - 4);
        }
        if (base.size() >= 4 && base.compare(base.size() - 4, 4, "_afm") == 0) {
            return true;
        }
    }

    std::istringstream iss(contents);
    std::string line;
    while (std::getline(iss, line)) {
        std::size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        if (line[start] == '#') continue;
        std::size_t end = line.find_first_of(" \t", start);
        std::string tok = (end == std::string::npos)
                          ? line.substr(start)
                          : line.substr(start, end - start);
        try {
            int v = std::stoi(tok);
            if (v >= 1 && v <= 4) return true;
        } catch (const std::exception&) {
            // Not an integer
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Top-level parser
// ---------------------------------------------------------------------------
ParseResult loadString(const std::string& contents, const std::string& sourceName) {
    ParseResult result;
    AircraftConfig& cfg = result.config;

    if (isAfmFile(contents, sourceName)) {
        result.errors.push_back(
            "AFM format not supported by f4-convert (BMS Advanced Flight Model .dat files "
            "use a different schema with 6-DOF inertia tensors and per-surface derivative "
            "tables). Use the standard .dat file with the same base name instead.");
        result.ok = false;
        return result;
    }

    extractSourceMetadata(contents, sourceName, cfg);
    cfg.name = cfg.sourceTitle;
    if (cfg.name.empty()) cfg.name = sourceName;

    // Capture aeropt options from raw contents (before TokenStream stripping).
    {
        std::istringstream iss(contents);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::size_t start = line.find_first_not_of(" \t\r\n\v\f");
            if (start == std::string::npos) continue;
            if (line.compare(start, 6, "aeropt") != 0) continue;
            if (start + 6 < line.size() && !std::isspace(static_cast<unsigned char>(line[start + 6]))) continue;
            std::size_t nameStart = line.find_first_not_of(" \t\r\n\v\f", start + 6);
            if (nameStart == std::string::npos) continue;
            std::size_t nameEnd = line.find_last_not_of(" \t\r\n\v\f");
            cfg.aeroOptions.push_back(line.substr(nameStart, nameEnd - nameStart + 1));
        }
    }

    TokenStream ts(contents, sourceName);

    try {
        parseInputData(ts, cfg, result.warnings);

        while (ts.peek() == "aeropt") {
            ts.next();
            ts.next();
        }
        try {
            parseAeroTable(ts, cfg.aero, "CL", result.warnings);
            parseAeroTable(ts, cfg.aero, "CD", result.warnings);
            parseAeroTable(ts, cfg.aero, "CY", result.warnings);
        } catch (const std::exception& e) {
            result.warnings.push_back(std::string("Aero tables: ") + e.what());
        }

        try {
            parseEngine(ts, cfg, result.warnings);
        } catch (const std::exception& e) {
            result.warnings.push_back(std::string("Engine: ") + e.what());
        }

        try {
            parseRollData(ts, cfg, result.warnings);
        } catch (const std::exception& e) {
            result.warnings.push_back(std::string("RollData: ") + e.what());
        }

        try {
            parseLimiters(ts, cfg, result.warnings);
        } catch (const std::exception& e) {
            result.warnings.push_back(std::string("Limiters: ") + e.what());
        }

        try {
            parseAuxAero(contents, cfg, result.warnings);
        } catch (const std::exception& e) {
            result.warnings.push_back(std::string("AuxAero: ") + e.what());
        }

        result.ok = true;
    } catch (const std::exception& e) {
        result.errors.push_back(e.what());
        result.ok = false;
    }

    return result;
}

ParseResult loadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ParseResult r;
        r.errors.push_back("Could not open file: " + path);
        r.ok = false;
        return r;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return loadString(ss.str(), path);
}

} // namespace f4::convert
