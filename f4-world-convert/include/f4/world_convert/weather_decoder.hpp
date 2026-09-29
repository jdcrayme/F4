// f4-world-convert/include/f4/world_convert/weather_decoder.hpp
//
// Decodes the .wth weather sub-file from a .cam archive (WTH-CODEC-1).
//
// Layout sources (FreeFalcon):
//   WeatherClass::CampLoad   campupd/weather.cpp:474
//   WeatherClass::Save       campupd/weather.cpp:665
//   COVersion = 0.0077f      campupd/weather.cpp:30 ("Cobra file version kludge")
//
// Three on-disk layouts (the file itself carries NO version field —
// branch on size + the COVersion marker, exactly as upstream does):
//
//   1. Legacy (original Falcon 4.0 — every committed fixture):
//      37-byte header + mapW*mapH 2-byte cloud cells = 32,805 bytes for
//      the standard 128x128 Korea grid. FreeFalcon's CampLoad reads ONLY
//      the header (the map is dead upstream — GetCloudCover is a FIXME
//      stub, weather.cpp:752); the original game wrote it, so every real
//      save still carries it and the decoder preserves it for the
//      runtime's future per-grid weather (WTH-SEED-1's consumer).
//
//   2. TaceditCompat (FreeFalcon's v<75 Save): 37 bytes, no map — the
//      fields reinterpret the legacy offsets and a float field carries
//      COVersion as the marker (offset 25 when FreeFalcon wrote it; the
//      loader also accepts offset 21 — "Tacedit reverses XOff and YOff
//      when TE is saved").
//
//   3. CobraFlat (v>=75 Save): 36 bytes (9 floats/ints, the v76+ form)
//      or 32 bytes (the v75 form without cumulusZ). No map.
//
//   The 0-byte .wth campinit writes (campaign_initializer.cpp writes
//   `stem + ".wth"` empty) decodes to nullopt — absent weather, the
//   honest reading the runtime already treats as "no save weather".
//
// Byte-identity contract: encode_wth(decode_wth(bytes)) == bytes for
// every layout (the encoder mirrors the decoder field-for-field; float
// values round-trip bit-exactly because both sides memcpy 4 bytes).
// Bytes beyond the modeled layout land in trailing_raw and ride the
// encode verbatim.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace f4::world_convert {

enum class WthLayout : uint8_t {
    Legacy,        // original F4: 37-byte header + cloud map
    TaceditCompat, // FreeFalcon v<75: 37 bytes, COVersion marker
    CobraFlat36,   // v>=76 Save: 9 fields
    CobraFlat32,   // v75 Save: 8 fields (no cumulusZ)
};

/// The decoded .wth face. Field names follow the upstream variables
/// (weather.cpp) so the layout comments in the encoder/decoder and the
/// JSON emitter read against the source. Units are the STORED units —
/// the runtime seed (WTH-SEED-1) owns any conversion.
struct DecodedWeather {
    WthLayout layout = WthLayout::Legacy;

    // --- Legacy header (offsets 0..36; Tacedit reuses 0..20) ------------
    float wind_heading = 0.0f;  // @0  rad (legacy; "Wind Direction")
    float wind_speed = 0.0f;    // @4  stored KPH (legacy); Tacedit reuses
                                //     @4 as cumulus base (feet, float)
    int32_t last_check = 0;     // @8  CampaignTime ms
    float temperature = 0.0f;   // @12 deg C (legacy); Tacedit: stratus base
    uint8_t todays_temp = 0;    // @16 (legacy)
    uint8_t todays_wind = 0;    // @17 (legacy; knots, u8)
    uint8_t cloud_base = 0;     // @18 legacy: cumulus base, 100s ft
                                //     Tacedit: weatherCondition (1..4)
    uint8_t con_layer_start = 0; // @19 1000s ft (legacy; Tacedit: contrail base)
    uint8_t con_layer_end = 0;   // @20 1000s ft (legacy; Tacedit: overcast depth)
    float x_off = 0.0f;         // @21 legacy XOff / Tacedit stratus2 base
    float y_off = 0.0f;         // @25 legacy YOff / Tacedit COVersion marker

    // --- TaceditCompat overlay (the reinterpretation, for consumers) ----
    // The @21/@25 float pair: one slot carries COVersion (the marker),
    // the other is stratus2 base (feet) — which is which depends on who
    // wrote the file (Tacedit swaps the slots). tacedit_stratus2_base_ft
    // is the non-marker value. The cumulus/stratus bases in feet ride
    // the reinterpreted wind_speed/temperature slots:
    float tacedit_cumulus_base_ft = 0.0f;   // = wind_speed slot (f32 @4)
    float tacedit_stratus_base_ft = 0.0f;   // = temperature slot (f32 @12)
    float tacedit_stratus2_base_ft = 0.0f;  // = the non-marker @21/@25 float

    // --- CobraFlat fields (Save order: condition, lastCheck, temperature,
    //     windSpeed, windHeading, [cumulusZ], stratusZ, contrailLow/High) --
    int32_t weather_condition = 0; // 1=Sunny 2=Fair 3=Poor 4=Inclement
    float wind_speed_flat = 0.0f;  // stored float (upstream internal unit)
    float wind_heading_flat = 0.0f; // rad
    float cumulus_z = 0.0f;        // v76+ only (negative, feet)
    float stratus_z = 0.0f;        // negative, feet
    float contrail_low = 0.0f;     // feet
    float contrail_high = 0.0f;    // feet

    // --- The cloud map (Legacy only) ------------------------------------
    uint32_t map_w = 0;            // grid width (128 on Korea)
    uint32_t map_h = 0;            // grid height
    std::vector<uint8_t> map_raw;  // w*h*2 bytes, interleaved
                                   // {level u8, cover u8} per cell
                                   // (FreeFalcon's CellState fwrite order)

    /// Bytes beyond the modeled layout (byte-identity ride-along). Empty
    /// for every fixture; the encoder appends verbatim.
    std::vector<uint8_t> trailing_raw;
};

/// Decode a .wth sub-file.
///   * empty input (campinit's 0-byte ride) → nullopt — weather absent;
///   * a layout the size/marker dispatch recognizes → the decoded face;
///   * anything else (implausible size, dims that don't match the
///     buffer, garbage marker) → std::runtime_error.
/// `camp_version` is the .cam's gCampDataVersion (the upstream reader
/// branches on it for the v75+ flat layout; kept for signature
/// stability + the v75/v76 cumulusZ gate).
[[nodiscard]] std::optional<DecodedWeather>
decode_wth(const uint8_t* data, std::size_t size, int camp_version);

} // namespace f4::world_convert
