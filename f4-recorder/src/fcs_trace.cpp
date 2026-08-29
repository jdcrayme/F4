// f4-recorder/src/fcs_trace.cpp
//
// FcsTraceWriter implementation. Writes one CSV header row followed by one
// row per FcsTraceSample. Numerical formatting uses max precision needed
// for offline plotting: 6 decimals for doubles (sufficient to resolve 0.1 ft
// at 10000 ft, 0.01 deg at 90 deg, 0.1 fpm at 6000 fpm).

#include "f4/recorder/fcs_trace.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace f4::recorder {

namespace {

// Format a double with 6 decimals. Using std::setprecision(6) + std::fixed
// gives "12.345600" for 12.3456 — consistent column widths for plotting tools.
void write_double(std::ostream& os, double v) {
    os << std::fixed << std::setprecision(6) << v;
}

void write_string(std::ostream& os, const std::string& s) {
    // CSV-escape: if the string contains a comma, quote, or newline, wrap
    // in double quotes and double any embedded quotes. AI mode/state names
    // are simple identifiers today (e.g. "OnFinal"), but defensive escaping
    // avoids silent corruption if that ever changes.
    const bool needs_quote = (s.find(',') != std::string::npos ||
                              s.find('"') != std::string::npos ||
                              s.find('\n') != std::string::npos);
    if (!needs_quote) {
        os << s;
        return;
    }
    os << '"';
    for (char c : s) {
        if (c == '"') os << '"';
        os << c;
    }
    os << '"';
}

void write_bool(std::ostream& os, bool b) {
    os << (b ? '1' : '0');
}

} // namespace

void FcsTraceWriter::write_csv(std::ostream& os) const {
    // Header row. Keep this in lock-step with the body below — if you add
    // a field to FcsTraceSample, add it here AND in the body.
    os << "tick,sim_time_s,time_scale,ai_mode,ai_state,"
       << "pitch_cmd,roll_cmd,yaw_cmd,throttle_cmd,speed_brake_cmd,"
       << "tef_cmd,lef_cmd,gear_down,wheel_brakes,parking_brake,"
       << "aoacmd_deg,pscmd,pstab,ptcmd,nzcgs,pitch_integral,betcmd_deg,"
       << "alpha_deg,beta_deg,yshape,pshape,rshape,"
       << "p_dps,q_dps,r_dps,"
       << "vcas_kts,vt_fps,alt_msl_ft,alt_agl_ft,vs_fpm,"
       << "heading_deg,pitch_deg,roll_deg,x_ft,y_ft,mach,"
       << "target_alt_ft,target_speed_kts,target_heading_deg,"
       << "course_lateral_ft,course_along_ft,localizer_heading_deg,"
       << "on_ground,gear_pos,engine_rpm,fuel_lbs,nz,nx\n";

    for (const auto& s : samples_) {
        // --- Timing ---
        os << s.tick << ',';
        write_double(os, s.sim_time_s);          os << ',';
        write_double(os, s.time_scale);           os << ',';

        // --- AI state ---
        write_string(os, s.ai_mode);              os << ',';
        write_string(os, s.ai_state);             os << ',';

        // --- AI commands ---
        write_double(os, s.pitch_cmd);             os << ',';
        write_double(os, s.roll_cmd);              os << ',';
        write_double(os, s.yaw_cmd);               os << ',';
        write_double(os, s.throttle_cmd);          os << ',';
        write_double(os, s.speed_brake_cmd);       os << ',';
        write_double(os, s.tef_cmd);               os << ',';
        write_double(os, s.lef_cmd);               os << ',';
        write_bool  (os, s.gear_down);             os << ',';
        write_bool  (os, s.wheel_brakes);          os << ',';
        write_bool  (os, s.parking_brake);         os << ',';

        // --- FCS intermediates ---
        write_double(os, s.aoacmd_deg);            os << ',';
        write_double(os, s.pscmd);                  os << ',';
        write_double(os, s.pstab);                  os << ',';
        write_double(os, s.ptcmd);                  os << ',';
        write_double(os, s.nzcgs);                  os << ',';
        write_double(os, s.pitch_integral);         os << ',';
        write_double(os, s.betcmd_deg);             os << ',';
        write_double(os, s.alpha_deg);              os << ',';
        write_double(os, s.beta_deg);               os << ',';
        write_double(os, s.yshape);                 os << ',';
        write_double(os, s.pshape);                 os << ',';
        write_double(os, s.rshape);                 os << ',';

        // --- Body rates ---
        write_double(os, s.p_dps);                 os << ',';
        write_double(os, s.q_dps);                  os << ',';
        write_double(os, s.r_dps);                  os << ',';

        // --- Kinematics ---
        write_double(os, s.vcas_kts);               os << ',';
        write_double(os, s.vt_fps);                  os << ',';
        write_double(os, s.alt_msl_ft);              os << ',';
        write_double(os, s.alt_agl_ft);              os << ',';
        write_double(os, s.vs_fpm);                  os << ',';
        write_double(os, s.heading_deg);             os << ',';
        write_double(os, s.pitch_deg);               os << ',';
        write_double(os, s.roll_deg);                os << ',';
        write_double(os, s.x_ft);                    os << ',';
        write_double(os, s.y_ft);                    os << ',';
        write_double(os, s.mach);                    os << ',';

        // --- Navigation intent ---
        write_double(os, s.target_alt_ft);          os << ',';
        write_double(os, s.target_speed_kts);        os << ',';
        write_double(os, s.target_heading_deg);      os << ',';
        write_double(os, s.course_lateral_ft);       os << ',';
        write_double(os, s.course_along_ft);        os << ',';
        write_double(os, s.localizer_heading_deg);  os << ',';

        // --- Ground / engine ---
        write_bool  (os, s.on_ground);               os << ',';
        write_double(os, s.gear_pos);                os << ',';
        write_double(os, s.engine_rpm);               os << ',';
        write_double(os, s.fuel_lbs);                 os << ',';
        write_double(os, s.nz);                       os << ',';
        write_double(os, s.nx);

        os << '\n';
    }
}

void FcsTraceWriter::write_csv(const std::string& path) const {
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("FcsTraceWriter::write_csv: cannot open '" + path + "'");
    }
    write_csv(ofs);
    ofs.flush();
    if (!ofs) {
        throw std::runtime_error("FcsTraceWriter::write_csv: write failed for '" + path + "'");
    }
}

} // namespace f4::recorder
