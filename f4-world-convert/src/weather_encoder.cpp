// f4-world-convert/src/weather_encoder.cpp
//
// .wth encoder — the inverse of weather_decoder.cpp's decode_wth().
// Field order, widths, layout dispatch, and the COVersion marker mirror
// the decoder exactly (that file is the ground truth; this one
// reproduces its byte sequence). See weather_encoder.hpp for the
// byte-identity scope note.

#include <f4/world_convert/weather_encoder.hpp>
#include "byte_writer.hpp"

#include <cstring>

namespace f4::world_convert {

namespace {

using Writer = ByteWriter;

void write_f32(Writer& w, float v) {
    w.f32(v);   // memcpy write: bit-exact against the decode read
}

} // namespace

std::vector<uint8_t> encode_wth(const DecodedWeather& w) {
    Writer out;

    switch (w.layout) {
    case WthLayout::CobraFlat36:
    case WthLayout::CobraFlat32: {
        // Upstream v>=75 Save order: condition, lastCheck, temperature,
        // windSpeed, windHeading, cumulusZ (v76+), stratusZ,
        // contrailLow, contrailHigh.
        out.i32(w.weather_condition);
        out.i32(w.last_check);
        write_f32(out, w.temperature);
        write_f32(out, w.wind_speed_flat);
        write_f32(out, w.wind_heading_flat);
        if (w.layout == WthLayout::CobraFlat36) write_f32(out, w.cumulus_z);
        write_f32(out, w.stratus_z);
        write_f32(out, w.contrail_low);
        write_f32(out, w.contrail_high);
        break;
    }
    case WthLayout::TaceditCompat:
    case WthLayout::Legacy: {
        // Shared 37-byte header. The Tacedit marker ride: FreeFalcon's
        // Save writes stratus2Z at @21 and COVersion at @25 — reproduce
        // that exactly (the decoder accepted either slot; the encoder
        // always emits the FreeFalcon form, and a Tacedit-written file
        // byte-identically re-encodes through the x_off/y_off capture
        // below because those floats decode bit-exactly).
        write_f32(out, w.wind_heading);      // @0
        write_f32(out, w.wind_speed);        // @4 (legacy KPH / tacedit cumulus ft)
        out.i32(w.last_check);               // @8
        write_f32(out, w.temperature);       // @12 (legacy °C / tacedit stratus ft)
        out.u8(w.todays_temp);               // @16
        out.u8(w.todays_wind);               // @17
        out.u8(w.cloud_base);                // @18
        out.u8(w.con_layer_start);           // @19
        out.u8(w.con_layer_end);             // @20
        write_f32(out, w.x_off);             // @21 XOff / tacedit slot value
        write_f32(out, w.y_off);             // @25 YOff / tacedit slot value
        if (w.layout == WthLayout::Legacy) {
            out.u32(w.map_w);                // @29
            out.u32(w.map_h);                // @33
            out.bytes(w.map_raw);            // w*h*2 {level, cover} cells
        }
        break;
    }
    }

    out.bytes(w.trailing_raw);
    return out.buf;
}

} // namespace f4::world_convert
