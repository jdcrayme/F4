// f4-data/formation_data.hpp
//
// Data-only representation of the AI formation geometry
// (sim/acdata/formdata/formdat.fil). This is the in-memory representation
// that f4-convert produces from the .FIL file and that f4-ai's
// WingmanModule (and other consumers) read.
//
// FILE FORMAT (FreeFalcon formdata.cpp:12-115 ACFormationData, verbatim
// token order):
//   numFormations
//   for each formation:
//     num4Slots num2Slots formNum <name>
//     num4Slots slot triples:   relAz_deg relEl_deg range_NM
//     if num2Slots: one more triple for the 2-ship slot
//     else:         2-ship slot DEFAULTS to slot[0]
//
// The reference converts units AT READ TIME (formdata.cpp:59-61):
//   relAz = atof * DTR; relEl = atof * DTR; range = atof * NM_TO_FT.
// This struct keeps the RAW file units (degrees / NM) and provides the
// converted accessors — the repo's parse-verbatim convention.
//
// STATION MATH (bvrengage.cpp:3330-3370, the live consumer):
//   rangeFactor = slot.range_ft * mFormLateralSpaceFactor   (1.0 default)
//   trackX = leadX + rangeFactor * cos(relAz * mFormSide + leadSigma)
//   trackY = leadY + rangeFactor * sin(relAz * mFormSide + leadSigma)
//   trackZ = leadZ + (relEl != 0 ? rangeFactor * sin(-relEl)
//                                 : flightIdx * -100.0F)
// with Falcon sim coords (X=north, Y=east, Z=DOWN positive, sigma =
// compass heading) and mFormSide = +1 (WMToggleSide mirrors it to -1;
// wingai.cpp:1842-1846). In this repo's ENU convention (x=east, y=north,
// z=MSL altitude UP positive, heading = atan2(east, north)) the same
// station is:
//   east  = range_ft * sin(heading + side * relAz)
//   north = range_ft * cos(heading + side * relAz)
//   alt   = lead.alt + range_ft * sin(relEl)   (el +90 = up, -90 = down)
//
// FORM NUMBERS: formNum is the WingManCmd enum value (wingmanmsg.h:20-29)
// — 0 spread, 1 wedge, 2 trail, 3 ladder, 4 stack, 5 rescell, 6 box,
// 7 arrowhead, 8 fluid-four — the key FindFormation(msgNum) searches for.
//
// SLOT SEMANTICS: positionData[] slots are the 4-ship wingman slots
// (#2..#4, indexed flightIdx-1); twoposData is the 2-ship #2 slot (this
// repo's WingmanModule station when a 2-ship flies).

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <f4/math/constants.hpp>

namespace f4::data {

/// NM → feet at the reference's conversion (phyconst.h NM_TO_FT 6076.211).
inline constexpr double kNmToFt = 6076.211;

// ---------------------------------------------------------------------------
// FormationSlot — one (relAz, relEl, range) station in the LEAD's
// heading frame. Raw file units preserved; converted accessors below.
// relAz: degrees from the lead's nose, positive = RIGHT (clockwise when
// viewed from above); 0 = ahead, 90 = right beam, 180 = directly behind.
// relEl: degrees, positive = above the lead's altitude, -90 = straight
// below (Falcon Z-down origin visible in the bvrengage.cpp math above).
// range: nautical miles from the lead.
// ---------------------------------------------------------------------------
struct FormationSlot {
    double rel_az_deg{0.0};
    double rel_el_deg{0.0};
    double range_nm{0.0};
    int    form_num{0};       ///< WingManCmd value this slot's formation belongs to

    [[nodiscard]] double az_rad() const noexcept {
        return rel_az_deg * f4::math::DEG_TO_RAD;
    }
    [[nodiscard]] double el_rad() const noexcept {
        return rel_el_deg * f4::math::DEG_TO_RAD;
    }
    [[nodiscard]] double range_ft() const noexcept {
        return range_nm * kNmToFt;
    }
};

// ---------------------------------------------------------------------------
// Formation — one named formation (num4Slots slots + the 2-ship slot).
// ---------------------------------------------------------------------------
struct Formation {
    std::string               name;      ///< verbatim file token ("spread", ...)
    int                       form_num{0};
    std::vector<FormationSlot> slots;    ///< 4-ship wingman slots (#2..#4)
    FormationSlot             two_ship;  ///< 2-ship #2 slot (slot[0] when num2Slots == 0)
    bool                      two_ship_explicit{false};  ///< file carried a dedicated 2-ship triple
};

// ---------------------------------------------------------------------------
// FormationLibrary — the whole file.
// ---------------------------------------------------------------------------
struct FormationLibrary {
    std::vector<Formation> formations;

    /// FindFormation(msgNum) (formdata.cpp:95-115): first formation whose
    /// slot[0].form_num matches; nullptr when absent.
    [[nodiscard]] const Formation* find_by_form_num(int formNum) const noexcept;

    /// Name lookup (case-insensitive; nullptr when absent).
    [[nodiscard]] const Formation* find_by_name(
        const std::string& name) const noexcept;
};

// ---------------------------------------------------------------------------
// JSON serialization (canonical format; f4-convert delegates here).
// ---------------------------------------------------------------------------
struct FormationLibraryResult {
    FormationLibrary data;
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Load from a JSON file on disk.
[[nodiscard]] FormationLibraryResult loadFormationLibrary(
    const std::string& path);

/// Load from a JSON string.
[[nodiscard]] FormationLibraryResult loadFormationLibraryFromString(
    const std::string& json);

/// Serialize to a pretty-printed JSON string.
[[nodiscard]] std::string writeFormationLibrary(const FormationLibrary& lib);

/// Write to a JSON file. Returns true on success.
bool writeFormationLibraryFile(const FormationLibrary& lib,
                               const std::string& path);

} // namespace f4::data
