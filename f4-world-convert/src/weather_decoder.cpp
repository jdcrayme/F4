// f4-world-convert/src/weather_decoder.cpp
//
// .wth decoder — see weather_decoder.hpp for the three-layout contract
// and the upstream references (campupd/weather.cpp:474 CampLoad /
// :665 Save / :30 COVersion).
//
// Golden fixtures (both verified against the committed archives):
//   save1.cam (v63):  32,805 bytes — clear 09:00 map, every cloud cell
//                     {0xFF, 0x00} (the level "none" sentinel), wind
//                     10.0 KPH, 20.0 °C, lastCheck 32,400,000 ms.
//   TestCamp.cam (v71): 32,805 bytes — real cloud map (levels 46..179,
//                     covers 0..8), wind 18.65 KPH @ 1.1252 rad,
//                     lastCheck 38,574,360 ms.

#include <f4/world_convert/weather_decoder.hpp>
#include <f4/io/cursor.hpp>

#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {

using f4::io::Cursor;

// Upstream constants.
constexpr std::size_t LEGACY_HEADER = 37;   // wind(8)+time(4)+temp(4)+6×u8
                                            // +xoff/yoff(8)+dims(8) = 36? no:
                                            // 4+4+4+4+1+1+1+1+1+4+4+4+4 = 37
constexpr float CO_VERSION = 0.0077f;       // "Cobra file version kludge"

float read_f32(Cursor& c) {
    float v = 0.0f;
    const uint8_t* p = c.p;   // memcpy read: bit-exact capture
    c.skip(4);
    if (!c.error) std::memcpy(&v, p, 4);
    return v;
}

} // namespace

std::optional<DecodedWeather> decode_wth(const uint8_t* data, std::size_t size,
                                         int /*camp_version*/) {
    if (size == 0) return std::nullopt;   // campinit's empty ride — absent

    // --- CobraFlat (v>=75 Save): 36 bytes (9 fields) / 32 bytes (v75,
    //     8 fields — no cumulusZ). No map. Field order per upstream Save:
    //     condition, lastCheck, temperature, windSpeed, windHeading,
    //     cumulusZ, stratusZ, contrailLow, contrailHigh.
    if (size == 36 || size == 32) {
        DecodedWeather w;
        w.layout = (size == 36) ? WthLayout::CobraFlat36 : WthLayout::CobraFlat32;
        Cursor c{data, data + size};
        w.weather_condition = c.i32();
        w.last_check = c.i32();
        w.temperature = read_f32(c);
        w.wind_speed_flat = read_f32(c);
        w.wind_heading_flat = read_f32(c);
        if (size == 36) w.cumulus_z = read_f32(c);
        w.stratus_z = read_f32(c);
        w.contrail_low = read_f32(c);
        w.contrail_high = read_f32(c);
        if (c.error) throw std::runtime_error("wth: truncated cobra-flat body");
        return w;
    }

    // --- The 37-byte-header family (Legacy + TaceditCompat) -------------
    if (size < LEGACY_HEADER)
        throw std::runtime_error("wth: too small for the 37-byte header");

    // Marker check FIRST (upstream CampLoad :547): a float at offset 21
    // or 25 equal to COVersion marks the Tacedit-compat reinterpretation
    // (Tacedit swaps the two marker slots when TE saves).
    float at21, at25;
    std::memcpy(&at21, data + 21, 4);
    std::memcpy(&at25, data + 25, 4);
    const bool tacedit = (at21 == CO_VERSION) || (at25 == CO_VERSION);

    DecodedWeather w;
    Cursor c{data, data + size};
    w.wind_heading = read_f32(c);       // @0
    w.wind_speed = read_f32(c);         // @4  (legacy KPH; tacedit cumulus ft)
    w.last_check = c.i32();             // @8
    w.temperature = read_f32(c);        // @12 (legacy °C; tacedit stratus ft)
    w.todays_temp = c.u8();             // @16
    w.todays_wind = c.u8();             // @17
    w.cloud_base = c.u8();              // @18 (legacy 100s ft; tacedit condition)
    w.con_layer_start = c.u8();         // @19
    w.con_layer_end = c.u8();           // @20
    w.x_off = read_f32(c);              // @21
    w.y_off = read_f32(c);              // @25

    if (tacedit) {
        w.layout = WthLayout::TaceditCompat;
        // The non-marker float of the @21/@25 pair is stratus2 base
        // (feet); wind_speed's slot is the cumulus base (feet) and
        // temperature's slot the stratus base (feet).
        w.tacedit_cumulus_base_ft = w.wind_speed;
        w.tacedit_stratus_base_ft = w.temperature;
        w.tacedit_stratus2_base_ft = (at21 == CO_VERSION) ? at25 : at21;
    } else {
        w.layout = WthLayout::Legacy;
        w.map_w = c.u32();              // @29
        w.map_h = c.u32();              // @33
        if (c.error) throw std::runtime_error("wth: truncated header dims");
        // Implausible dims guard (the map is 2 bytes per cell; a real
        // theater grid is 128x128; reject anything that doesn't fit the
        // buffer we were handed).
        const uint64_t map_bytes =
            static_cast<uint64_t>(w.map_w) * w.map_h * 2u;
        if (map_bytes > (1ull << 26))
            throw std::runtime_error("wth: implausible map dims");
        const std::size_t have = size - LEGACY_HEADER;
        if (have < map_bytes)
            throw std::runtime_error("wth: cloud map truncated");
        w.map_raw.resize(static_cast<std::size_t>(map_bytes));
        c.read(w.map_raw.data(), static_cast<std::size_t>(map_bytes));
        if (c.error) throw std::runtime_error("wth: cloud map truncated");
    }

    // Trailing bytes beyond the modeled layout (empty for every known
    // producer; captured so encode_wth stays byte-faithful regardless).
    if (!c.error && c.p < c.end) {
        w.trailing_raw.assign(c.p, c.end);
    }

    return w;
}

} // namespace f4::world_convert
