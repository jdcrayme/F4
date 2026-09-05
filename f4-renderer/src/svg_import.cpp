// f4-renderer/src/svg_import.cpp
//
// SVG import/export for the SymbolLibrary — see svg_import.hpp for the
// supported-subset contract. Import parses (pugixml), flattens curves,
// applies transforms, classifies evenodd subpaths into outer rings +
// holes, and normalizes everything into the model's [-1, +1] space in
// ONE pass at load time. Export emits the same subset so definitions
// round-trip through external editors (and back) losslessly at render
// fidelity.

#include <f4/renderer/svg_import.hpp>

#include <f4/xml/f4_xml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace f4::renderer {
namespace {

// Curve-flattening resolution (points per segment).
constexpr int kCurveSegments = 16;
constexpr int kCircleSegments = 32;
constexpr int kCornerSegments = 4;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("f4::renderer::import_symbol_from_svg: " + what);
}

// ===========================================================================
// Affine transform — SVG's 2x3 matrix: x' = a*x + c*y + e; y' = b*x + d*y + f
// ===========================================================================

struct Xform {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    // Compose so that `inner` applies FIRST, then `outer`.
    static Xform compose(const Xform& o, const Xform& i) {
        return Xform{
            o.a * i.a + o.c * i.b,
            o.b * i.a + o.d * i.b,
            o.a * i.c + o.c * i.d,
            o.b * i.c + o.d * i.d,
            o.a * i.e + o.c * i.f + o.e,
            o.b * i.e + o.d * i.f + o.f,
        };
    }

    [[nodiscard]] SymbolPoint apply(double x, double y) const {
        const double dx = x, dy = y;
        return SymbolPoint{
            static_cast<float>(a * dx + c * dy + e),
            static_cast<float>(b * dx + d * dy + f),
        };
    }
};

// ===========================================================================
// Lexing helpers (shared by path data, transform lists, viewBox, points)
// ===========================================================================

const char* const kSeps = " \t\r\n\f\v,";

void skip_sep(const char*& p) {
    while (*p && std::strchr(kSeps, *p)) ++p;
}

[[nodiscard]] bool number_starts(char c) {
    return c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9');
}

// Try to read an SVG number at p (skipping separators first). Advances p
// on success. Rejects non-finite values ("1e999").
[[nodiscard]] bool read_number(const char*& p, double& out) {
    skip_sep(p);
    if (!number_starts(*p)) return false;
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p || !std::isfinite(v)) return false;
    p = end;
    out = v;
    return true;
}

double require_number(const char*& p, const char* context) {
    double v = 0.0;
    if (!read_number(p, v)) {
        fail(std::string("expected a number ") + context);
    }
    return v;
}

// ===========================================================================
// Transform lists — "translate(10,20) scale(2) rotate(45, 5, 5)"
// Left-to-right composition per SVG: the rightmost applies first.
// ===========================================================================

Xform parse_transform(const char* s) {
    Xform total;
    const char* p = s;
    for (;;) {
        skip_sep(p);
        if (!*p) break;
        const char* name_start = p;
        while (*p && *p != '(' && (std::isalpha(static_cast<unsigned char>(*p)) || *p == '-')) ++p;
        if (*p != '(') {
            fail(std::string("malformed transform list '") + s + "'");
        }
        const std::string name(name_start, p);
        ++p;  // consume '('
        std::vector<double> args;
        double v = 0.0;
        while (read_number(p, v)) args.push_back(v);
        skip_sep(p);
        if (*p != ')') {
            fail(std::string("malformed transform '") + name + "' in '" + s + "'");
        }
        ++p;  // consume ')'

        Xform m;
        if (name == "translate") {
            if (args.empty() || args.size() > 2) fail("translate() takes 1-2 arguments");
            m.e = static_cast<float>(args[0]);
            m.f = static_cast<float>(args.size() > 1 ? args[1] : 0.0);
        } else if (name == "scale") {
            if (args.empty() || args.size() > 2) fail("scale() takes 1-2 arguments");
            const float sx = static_cast<float>(args[0]);
            const float sy = static_cast<float>(args.size() > 1 ? args[1] : args[0]);
            if (sx == 0.0f || sy == 0.0f) fail("scale(0) collapses the symbol");
            m.a = sx;
            m.d = sy;
        } else if (name == "rotate") {
            if (args.empty() || args.size() == 2 || args.size() > 3) fail("rotate() takes 1 or 3 arguments");
            const double rad = args[0] * 3.14159265358979323846 / 180.0;
            const float cs = static_cast<float>(std::cos(rad));
            const float sn = static_cast<float>(std::sin(rad));
            m.a = cs; m.b = sn; m.c = -sn; m.d = cs;
            if (args.size() == 3) {
                const float cx = static_cast<float>(args[1]);
                const float cy = static_cast<float>(args[2]);
                Xform to_center, from_center;
                to_center.e = cx; to_center.f = cy;
                from_center.e = -cx; from_center.f = -cy;
                m = Xform::compose(to_center, Xform::compose(m, from_center));
            }
        } else if (name == "matrix") {
            if (args.size() != 6) fail("matrix() takes 6 arguments");
            m.a = static_cast<float>(args[0]);
            m.b = static_cast<float>(args[1]);
            m.c = static_cast<float>(args[2]);
            m.d = static_cast<float>(args[3]);
            m.e = static_cast<float>(args[4]);
            m.f = static_cast<float>(args[5]);
        } else {
            fail("unsupported transform '" + name +
                 "(...)' — supported: translate, scale, rotate, matrix");
        }
        total = Xform::compose(total, m);
    }
    return total;
}

// ===========================================================================
// Paint / style inheritance
// ===========================================================================

enum class Paint { None, Current, Black, White };

Paint parse_paint(const char* value, const char* attr) {
    if (std::strcmp(value, "none") == 0) return Paint::None;
    if (std::strcmp(value, "currentColor") == 0) return Paint::Current;
    if (std::strcmp(value, "black") == 0 || std::strcmp(value, "#000000") == 0 ||
        std::strcmp(value, "#000") == 0) return Paint::Black;
    if (std::strcmp(value, "white") == 0 || std::strcmp(value, "#ffffff") == 0 ||
        std::strcmp(value, "#fff") == 0) return Paint::White;
    fail(std::string(attr) + "=\"" + value +
         "\": only currentColor / none / black / white paints are supported");
}

struct Style {
    Paint fill = Paint::Current;    // absent fill -> Fill role (documented deviation)
    Paint stroke = Paint::None;
    float stroke_width = 1.0f;      // viewBox units
    bool evenodd = false;
    std::optional<SymbolColorRole> role_override;  // data-color-role
};

// Style inheritance down the tree. "inherit" keeps the parent value.
Style inherit_style(const Style& parent, const f4::xml::xml_node& n) {
    Style s = parent;
    for (f4::xml::xml_attribute a : n.attributes()) {
        const char* name = a.name();
        const char* value = a.value();
        if (std::strcmp(name, "fill") == 0) {
            if (std::strcmp(value, "inherit") != 0) s.fill = parse_paint(value, "fill");
        } else if (std::strcmp(name, "stroke") == 0) {
            if (std::strcmp(value, "inherit") != 0) s.stroke = parse_paint(value, "stroke");
        } else if (std::strcmp(name, "stroke-width") == 0) {
            const char* p = value;
            const double w = require_number(p, "for stroke-width");
            if (w < 0.0) fail("stroke-width must be >= 0");
            s.stroke_width = static_cast<float>(w);
        } else if (std::strcmp(name, "fill-rule") == 0) {
            if (std::strcmp(value, "evenodd") == 0) s.evenodd = true;
            else if (std::strcmp(value, "nonzero") == 0) s.evenodd = false;
            else fail(std::string("fill-rule=\"") + value + "\": only nonzero/evenodd");
        } else if (std::strcmp(name, "data-color-role") == 0) {
            if (std::strcmp(value, "fill") == 0) s.role_override = SymbolColorRole::Fill;
            else if (std::strcmp(value, "fill_blend") == 0) s.role_override = SymbolColorRole::FillBlend;
            else if (std::strcmp(value, "outline") == 0) s.role_override = SymbolColorRole::Outline;
            else fail(std::string("data-color-role=\"") + value + "\": fill/fill_blend/outline");
        }
    }
    return s;
}

// Two-tone placeholder mapping: an icon is authored black-on-white (the
// MIL-STD-2525 convention) and both placeholder paints are replaced at
// draw time by the owning team's palette — white -> Fill (the team's
// primary), black -> Outline (the team's secondary). data-color-role
// overrides the mapping when an author wants an explicit role.
SymbolColorRole paint_role(Paint p, const Style& st) {
    if (st.role_override) return *st.role_override;
    return (p == Paint::Current || p == Paint::White)
               ? SymbolColorRole::Fill
               : SymbolColorRole::Outline;
}

// ===========================================================================
// Attribute validation — allowed geometry/presentation per element;
// rendering-changing attributes fail loudly, inert ones are ignored.
// ===========================================================================

bool name_in(const char* n, std::initializer_list<const char*> set) {
    for (const char* s : set) {
        if (std::strcmp(n, s) == 0) return true;
    }
    return false;
}

// Attributes whose IDENTITY value editors write by default (Inkscape
// stamps fill-opacity="1" etc. on every shape). The identity value is
// ignored; anything else changes rendering and fails.
bool tolerated_identity(const char* n, const char* v) {
    struct Entry { const char* name; const char* value; };
    static constexpr Entry kTolerated[] = {
        {"opacity", "1"}, {"fill-opacity", "1"}, {"stroke-opacity", "1"},
        {"stroke-dasharray", "none"}, {"display", "inline"},
    };
    for (const auto& e : kTolerated) {
        if (std::strcmp(n, e.name) == 0) return std::strcmp(v, e.value) == 0;
    }
    return false;
}

constexpr std::initializer_list<const char*> kPresentationAttrs = {
    "fill", "stroke", "stroke-width", "fill-rule", "data-color-role", "transform",
};

// Attributes that change rendering outside the subset. Unknown-but-inert
// attributes (id, data-*, editor metadata like Inkscape's sodipodi:*) are
// ignored silently; these fail by name.
bool is_dangerous_attr(const char* n) {
    return name_in(n, {
        "filter", "mask", "clip-path", "class", "style",
        "marker-start", "marker-mid", "marker-end",
        "xlink:href", "href", "vector-effect",
        "display", "opacity", "fill-opacity", "stroke-opacity",
        "stroke-dasharray",
    });
}

void check_attributes(const f4::xml::xml_node& n,
                      std::initializer_list<const char*> geometry_attrs) {
    for (f4::xml::xml_attribute a : n.attributes()) {
        const char* name = a.name();
        if (name_in(name, geometry_attrs) || name_in(name, kPresentationAttrs)) continue;
        if (tolerated_identity(name, a.value())) continue;
        if (is_dangerous_attr(name)) {
            fail(std::string("attribute '") + name + "' on <" + n.name() +
                 "> changes rendering and is outside the SVG symbol subset "
                 "(filters/CSS/opacity/dashes are not supported)");
        }
        // Inert unknown attribute — ignore.
    }
}

// ===========================================================================
// Fill-ring classification — contained subpaths become holes.
// ===========================================================================

[[nodiscard]] double ring_area_abs(const std::vector<SymbolPoint>& ring) {
    double s = 0.0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const SymbolPoint& a = ring[i];
        const SymbolPoint& b = ring[(i + 1) % ring.size()];
        s += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    return std::fabs(s) * 0.5;
}

[[nodiscard]] bool point_in_ring(const SymbolPoint& pt,
                                 const std::vector<SymbolPoint>& ring) {
    bool inside = false;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const SymbolPoint& a = ring[i];
        const SymbolPoint& b = ring[j];
        if ((a.y > pt.y) != (b.y > pt.y) &&
            pt.x < (b.x - a.x) * (pt.y - a.y) / (b.y - a.y) + a.x) {
            inside = !inside;
        }
    }
    return inside;
}

struct FillPoly {
    std::vector<SymbolPoint> outer;
    std::vector<std::vector<SymbolPoint>> holes;
};

// Sort rings by |area| descending; each ring becomes a hole in the
// smallest ring that contains it, or a new outer. Containment is tested
// with the ring's first vertex — adequate for well-formed symbols (and
// for this exporter's output, where holes are strictly interior).
std::vector<FillPoly> classify_fill_rings(std::vector<std::vector<SymbolPoint>> rings) {
    std::vector<std::size_t> order(rings.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
        return ring_area_abs(rings[x]) > ring_area_abs(rings[y]);
    });

    std::vector<FillPoly> outers;
    for (std::size_t idx : order) {
        auto& ring = rings[idx];
        if (ring.size() < 3) continue;
        FillPoly* best = nullptr;
        for (auto& fp : outers) {
            if (point_in_ring(ring.front(), fp.outer) &&
                (!best || ring_area_abs(fp.outer) < ring_area_abs(best->outer))) {
                best = &fp;
            }
        }
        if (best) {
            best->holes.push_back(std::move(ring));
        } else {
            outers.push_back(FillPoly{std::move(ring), {}});
        }
    }
    return outers;
}

// ===========================================================================
// Sink — geometry accumulator writing into a SymbolDefinition
// ===========================================================================

struct Sink {
    SymbolDefinition def;
    float vb_scale = 1.0f;  // user units -> model units (for stroke widths)

    void add_fill(std::vector<std::vector<SymbolPoint>> rings, const Style& st) {
        for (auto& fp : classify_fill_rings(std::move(rings))) {
            SymbolPolygon pg;
            pg.points = std::move(fp.outer);
            pg.holes = std::move(fp.holes);
            pg.filled = true;
            pg.color_role = paint_role(st.fill, st);
            def.polygons.push_back(std::move(pg));
        }
    }

    void add_stroke(const std::vector<SymbolPoint>& pts, bool closed, const Style& st) {
        if (pts.size() < 2 || st.stroke == Paint::None) return;
        SymbolPolyline pl;
        pl.points = pts;
        pl.closed = closed && pts.size() >= 3;
        pl.width = st.stroke_width * vb_scale * (kSymbolReferenceSizePx * 0.5f);
        if (pl.width <= 0.0f) pl.width = 1.0f;
        pl.color_role = paint_role(st.stroke, st);
        def.polylines.push_back(std::move(pl));
    }
};

// ===========================================================================
// Path data — full command set, flattening, implicit repetition
// ===========================================================================

struct Loop {
    std::vector<SymbolPoint> pts;  // model coords
    bool closed = false;
};

struct PathState {
    Xform xf;
    Sink* sink = nullptr;

    // User-space current point / subpath start + reflection state.
    double cx = 0, cy = 0, sx = 0, sy = 0;
    bool has_cur = false;
    std::optional<std::pair<double, double>> last_c2;  // last cubic 2nd ctrl
    std::optional<std::pair<double, double>> last_q1;  // last quad ctrl
    char prev_cmd = 0;

    Loop cur;
    std::vector<Loop> loops;

    void emit(double x, double y) {
        cur.pts.push_back(xf.apply(x, y));
    }

    // After a Z the next non-M command continues at the subpath start.
    void ensure_started() {
        if (!cur.pts.empty()) return;
        if (!has_cur) fail("path data must start with M/m");
        cur.pts.push_back(xf.apply(cx, cy));
    }

    void flush(bool closed) {
        if (cur.pts.size() >= 2) {
            cur.closed = closed && cur.pts.size() >= 3;
            loops.push_back(std::move(cur));
        }
        cur = Loop{};
    }
};

void emit_cubic(PathState& st, double x1, double y1,
                double x2, double y2, double x3, double y3,
                double x4, double y4) {
    for (int i = 1; i <= kCurveSegments; ++i) {
        const double t = static_cast<double>(i) / kCurveSegments;
        const double u = 1.0 - t;
        const double w0 = u * u * u, w1 = 3 * u * u * t,
                     w2 = 3 * u * t * t, w3 = t * t * t;
        st.emit(w0 * x1 + w1 * x2 + w2 * x3 + w3 * x4,
                w0 * y1 + w1 * y2 + w2 * y3 + w3 * y4);
    }
}

void emit_quad(PathState& st, double x1, double y1,
               double x2, double y2, double x3, double y3) {
    for (int i = 1; i <= kCurveSegments; ++i) {
        const double t = static_cast<double>(i) / kCurveSegments;
        const double u = 1.0 - t;
        st.emit(u * u * x1 + 2 * u * t * x2 + t * t * x3,
                u * u * y1 + 2 * u * t * y2 + t * t * y3);
    }
}

// W3C SVG spec F.6.5 endpoint-to-center arc conversion, then sampled.
// Returns points EXCLUDING the start, INCLUDING the end (user coords).
std::vector<std::pair<double, double>> arc_points(
    double x1, double y1, double x2, double y2,
    double rx, double ry, double rot_deg, bool large, bool sweep) {
    std::vector<std::pair<double, double>> out;
    if (rx == 0.0 || ry == 0.0 || (x1 == x2 && y1 == y2)) {
        out.emplace_back(x2, y2);
        return out;
    }
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    const double phi = rot_deg * 3.14159265358979323846 / 180.0;
    const double cosp = std::cos(phi), sinp = std::sin(phi);
    const double dx = (x1 - x2) / 2.0, dy = (y1 - y2) / 2.0;
    const double x1p = cosp * dx + sinp * dy;
    const double y1p = -sinp * dx + cosp * dy;

    const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        const double s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }
    const double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    const double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double coef = 0.0;
    if (den > 0.0) coef = std::sqrt(std::max(0.0, num / den));
    const double sign = (large != sweep) ? 1.0 : -1.0;
    const double cxp = coef * sign * (rx * y1p) / ry;
    const double cyp = coef * sign * -(ry * x1p) / rx;
    const double ccx = cosp * cxp - sinp * cyp + (x1 + x2) / 2.0;
    const double ccy = sinp * cxp + cosp * cyp + (y1 + y2) / 2.0;

    auto angle_of = [](double ux, double uy, double vx, double vy) {
        double dot = ux * vx + uy * vy;
        const double len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        dot = std::max(-1.0, std::min(1.0, dot / (len > 0.0 ? len : 1.0)));
        double a = std::acos(dot);
        if (ux * vy - uy * vx < 0.0) a = -a;
        return a;
    };

    const double theta1 = angle_of(1.0, 0.0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    double dtheta = angle_of((x1p - cxp) / rx, (y1p - cyp) / ry,
                             (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && dtheta > 0.0) dtheta -= 2.0 * 3.14159265358979323846;
    if (sweep && dtheta < 0.0) dtheta += 2.0 * 3.14159265358979323846;

    for (int i = 1; i <= kCurveSegments; ++i) {
        const double th = theta1 + dtheta * static_cast<double>(i) / kCurveSegments;
        out.emplace_back(ccx + rx * cosp * std::cos(th) - ry * sinp * std::sin(th),
                         ccy + rx * sinp * std::cos(th) + ry * cosp * std::sin(th));
    }
    // Land exactly on the endpoint regardless of rounding.
    out.back() = {x2, y2};
    return out;
}

void parse_path_data(const char* d, const Xform& xf, const Style& stl, Sink& sink) {
    PathState st;
    st.xf = xf;
    const char* p = d;

    while (true) {
        skip_sep(p);
        if (!*p) break;
        const char cmd = *p;
        if (!std::isalpha(static_cast<unsigned char>(cmd))) {
            // Implicit repetition of the previous command.
            if (st.prev_cmd == 0) fail("path data must start with a command");
        } else {
            ++p;
            if (cmd == 'M' || cmd == 'm') {
                // An M after coordinates closes the previous subpath as open.
                st.flush(false);
            }
        }
        const bool implicit = !std::isalpha(static_cast<unsigned char>(cmd));
        char c = implicit ? st.prev_cmd : cmd;
        if (implicit && (c == 'M' || c == 'm')) {
            // Per spec, implicit repetition of M/m is a line-to:
            // "M 100 100 200 200" means moveto + lineto.
            c = (c == 'M') ? 'L' : 'l';
        }
        if (implicit && (c == 'Z' || c == 'z')) {
            // Would loop forever: Z consumes no arguments.
            fail("coordinates after Z require a new M/m command");
        }
        const char eff = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool rel = std::islower(static_cast<unsigned char>(c)) != 0;

        switch (eff) {
            case 'm': {
                double x = require_number(p, "after M/m");
                double y = require_number(p, "after M/m");
                if (rel) { x += st.cx; y += st.cy; }
                st.cx = x; st.cy = y; st.sx = x; st.sy = y; st.has_cur = true;
                st.cur.pts.push_back(xf.apply(x, y));
                break;
            }
            case 'l': {
                st.ensure_started();
                double x = require_number(p, "after L/l");
                double y = require_number(p, "after L/l");
                if (rel) { x += st.cx; y += st.cy; }
                st.emit(x, y);
                st.cx = x; st.cy = y;
                break;
            }
            case 'h': {
                st.ensure_started();
                double x = require_number(p, "after H/h");
                if (rel) x += st.cx;
                st.emit(x, st.cy);
                st.cx = x;
                break;
            }
            case 'v': {
                st.ensure_started();
                double y = require_number(p, "after V/v");
                if (rel) y += st.cy;
                st.emit(st.cx, y);
                st.cy = y;
                break;
            }
            case 'c': {
                st.ensure_started();
                double a[6];
                for (double& v : a) v = require_number(p, "after C/c");
                if (rel) {
                    a[0] += st.cx; a[1] += st.cy; a[2] += st.cx; a[3] += st.cy;
                    a[4] += st.cx; a[5] += st.cy;
                }
                emit_cubic(st, st.cx, st.cy, a[0], a[1], a[2], a[3], a[4], a[5]);
                st.last_c2 = {a[2], a[3]};
                st.cx = a[4]; st.cy = a[5];
                break;
            }
            case 's': {
                st.ensure_started();
                double a[4];
                for (double& v : a) v = require_number(p, "after S/s");
                if (rel) {
                    a[0] += st.cx; a[1] += st.cy; a[2] += st.cx; a[3] += st.cy;
                }
                double c1x = st.cx, c1y = st.cy;
                if (st.last_c2) {
                    c1x = 2 * st.cx - st.last_c2->first;
                    c1y = 2 * st.cy - st.last_c2->second;
                }
                emit_cubic(st, st.cx, st.cy, c1x, c1y, a[0], a[1], a[2], a[3]);
                st.last_c2 = {a[0], a[1]};
                st.cx = a[2]; st.cy = a[3];
                break;
            }
            case 'q': {
                st.ensure_started();
                double a[4];
                for (double& v : a) v = require_number(p, "after Q/q");
                if (rel) {
                    a[0] += st.cx; a[1] += st.cy; a[2] += st.cx; a[3] += st.cy;
                }
                emit_quad(st, st.cx, st.cy, a[0], a[1], a[2], a[3]);
                st.last_q1 = {a[0], a[1]};
                st.cx = a[2]; st.cy = a[3];
                break;
            }
            case 't': {
                st.ensure_started();
                double a[2];
                for (double& v : a) v = require_number(p, "after T/t");
                if (rel) { a[0] += st.cx; a[1] += st.cy; }
                double c1x = st.cx, c1y = st.cy;
                if (st.last_q1) {
                    c1x = 2 * st.cx - st.last_q1->first;
                    c1y = 2 * st.cy - st.last_q1->second;
                }
                emit_quad(st, st.cx, st.cy, c1x, c1y, a[0], a[1]);
                st.last_q1 = {c1x, c1y};
                st.cx = a[0]; st.cy = a[1];
                break;
            }
            case 'a': {
                st.ensure_started();
                double rx = require_number(p, "arc rx");
                double ry = require_number(p, "arc ry");
                double rot = require_number(p, "arc rotation");
                skip_sep(p);
                if (*p != '0' && *p != '1') fail("arc large-arc flag must be 0 or 1");
                const bool large = *p++ == '1';
                skip_sep(p);
                if (*p != '0' && *p != '1') fail("arc sweep flag must be 0 or 1");
                const bool sweep = *p++ == '1';
                double x = require_number(p, "arc x");
                double y = require_number(p, "arc y");
                if (rel) { x += st.cx; y += st.cy; }
                for (auto& q : arc_points(st.cx, st.cy, x, y, rx, ry, rot, large, sweep)) {
                    st.emit(q.first, q.second);
                }
                st.cx = x; st.cy = y;
                break;
            }
            case 'z': {
                st.flush(true);
                st.cx = st.sx; st.cy = st.sy;  // Z returns to the subpath start
                break;
            }
            default:
                fail(std::string("unsupported path command '") + c + "'");
        }

        if (std::isalpha(static_cast<unsigned char>(cmd))) {
            st.prev_cmd = cmd;
        }
        // Reflection state only survives across its own command family.
        if (eff != 'c' && eff != 's') st.last_c2.reset();
        if (eff != 'q' && eff != 't') st.last_q1.reset();
    }
    st.flush(false);

    // Emit: filled shapes take ALL loops (open ones close, per SVG);
    // otherwise stroked shapes emit one polyline per loop.
    if (stl.fill != Paint::None) {
        std::vector<std::vector<SymbolPoint>> rings;
        for (auto& lp : st.loops) rings.push_back(std::move(lp.pts));
        sink.add_fill(std::move(rings), stl);
    } else {
        for (auto& lp : st.loops) {
            sink.add_stroke(lp.pts, lp.closed, stl);
        }
    }
}

// ===========================================================================
// Shape elements
// ===========================================================================

std::vector<std::pair<double, double>> parse_points_attr(const char* s) {
    std::vector<std::pair<double, double>> pts;
    const char* p = s;
    double v = 0.0;
    while (read_number(p, v)) {
        const double x = v;
        const double y = require_number(p, "in points list (y coordinate)");
        pts.emplace_back(x, y);
    }
    skip_sep(p);
    if (*p) fail(std::string("trailing junk in points list '") + s + "'");
    return pts;
}

Xform node_xform(const Xform& base, const f4::xml::xml_node& n) {
    if (f4::xml::xml_attribute t = n.attribute("transform")) {
        return Xform::compose(base, parse_transform(t.value()));
    }
    return base;
}

double attr_number(const f4::xml::xml_node& n, const char* name, double def) {
    if (f4::xml::xml_attribute a = n.attribute(name)) {
        const char* p = a.value();
        return require_number(p, (std::string("for attribute '") + name + "'").c_str());
    }
    return def;
}

std::vector<SymbolPoint> ring_to_model(const std::vector<std::pair<double, double>>& ring,
                                       const Xform& xf) {
    std::vector<SymbolPoint> out;
    out.reserve(ring.size());
    for (const auto& q : ring) out.push_back(xf.apply(q.first, q.second));
    return out;
}

void emit_shape(Sink& sink, const Style& st,
                std::vector<std::vector<std::pair<double, double>>> rings_user,
                const Xform& xf, bool stroke_only) {
    if (rings_user.empty()) return;
    if (!stroke_only && st.fill != Paint::None) {
        std::vector<std::vector<SymbolPoint>> rings;
        rings.reserve(rings_user.size());
        for (auto& r : rings_user) rings.push_back(ring_to_model(r, xf));
        sink.add_fill(std::move(rings), st);
    } else {
        for (const auto& r : rings_user) {
            sink.add_stroke(ring_to_model(r, xf), true, st);
        }
    }
}

void handle_rect(const f4::xml::xml_node& n, Style st, const Xform& xf, Sink& sink) {
    const double x = attr_number(n, "x", 0.0);
    const double y = attr_number(n, "y", 0.0);
    const double w = attr_number(n, "width", 0.0);
    const double h = attr_number(n, "height", 0.0);
    if (w < 0.0 || h < 0.0) fail("rect width/height must be >= 0");
    if (w == 0.0 || h == 0.0) return;
    double rx = n.attribute("rx") ? attr_number(n, "rx", 0.0) : 0.0;
    double ry = n.attribute("ry") ? attr_number(n, "ry", rx) : rx;
    if (rx < 0.0 || ry < 0.0) fail("rect rx/ry must be >= 0");
    rx = std::min(rx, w / 2.0);
    ry = std::min(ry, h / 2.0);

    std::vector<std::pair<double, double>> ring;
    if (rx <= 0.0 || ry <= 0.0) {
        ring = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    } else {
        // Rounded corners: quarter ellipses, walk clockwise (SVG y-down).
        struct Corner { double cx, cy, a0; };
        const Corner corners[] = {
            {x + w - rx, y + ry, -1.5707963267948966},  // top-right: -90° -> 0°
            {x + w - rx, y + h - ry, 0.0},               // bottom-right
            {x + rx, y + h - ry, 1.5707963267948966},    // bottom-left
            {x + rx, y + ry, 3.1415926535897932},        // top-left
        };
        ring.emplace_back(x + rx, y);
        for (const Corner& c : corners) {
            for (int i = 1; i <= kCornerSegments; ++i) {
                const double th =
                    c.a0 + (1.5707963267948966 * i) / kCornerSegments;
                ring.emplace_back(c.cx + rx * std::cos(th), c.cy + ry * std::sin(th));
            }
        }
    }
    emit_shape(sink, st, {std::move(ring)}, node_xform(xf, n), false);
}

void handle_ellipse_like(const f4::xml::xml_node& n, Style st, const Xform& xf,
                         Sink& sink, double rx, double ry) {
    const double cx = attr_number(n, "cx", 0.0);
    const double cy = attr_number(n, "cy", 0.0);
    if (rx <= 0.0 || ry <= 0.0) return;
    std::vector<std::pair<double, double>> ring;
    for (int i = 0; i < kCircleSegments; ++i) {
        const double th = 2.0 * 3.1415926535897932 * i / kCircleSegments;
        ring.emplace_back(cx + rx * std::cos(th), cy + ry * std::sin(th));
    }
    emit_shape(sink, st, {std::move(ring)}, node_xform(xf, n), false);
}

void handle_poly_points(const f4::xml::xml_node& n, Style st, const Xform& xf,
                        Sink& sink, bool closed_shape) {
    if (!n.attribute("points")) fail(std::string("<") + n.name() + "> requires a points list");
    auto pts = parse_points_attr(n.attribute("points").value());
    if (pts.size() < 2) return;
    const Xform nx = node_xform(xf, n);
    if (closed_shape && st.fill != Paint::None) {
        emit_shape(sink, st, {std::move(pts)}, nx, false);
    } else {
        sink.add_stroke(ring_to_model(pts, nx), closed_shape, st);
    }
}

// ===========================================================================
// Element tree walk
// ===========================================================================

struct ImportMeta {
    std::string display_name;
    std::string description;
};

void walk(const f4::xml::xml_node& parent, const Style& style, const Xform& xf,
          Sink& sink, ImportMeta& meta) {
    for (f4::xml::xml_node n = parent.first_child(); n; n = n.next_sibling()) {
        if (n.type() != f4::xml::node_element) continue;
        const char* name = n.name();

        if (std::strcmp(name, "g") == 0) {
            check_attributes(n, {});
            walk(n, inherit_style(style, n), node_xform(xf, n), sink, meta);
        } else if (std::strcmp(name, "title") == 0) {
            if (meta.display_name.empty()) meta.display_name = n.child_value();
        } else if (std::strcmp(name, "desc") == 0) {
            if (meta.description.empty()) meta.description = n.child_value();
        } else if (std::strcmp(name, "metadata") == 0) {
            continue;  // editor metadata, ignored
        } else if (std::strcmp(name, "path") == 0) {
            check_attributes(n, {"d"});
            if (!n.attribute("d")) fail("<path> requires a d attribute");
            parse_path_data(n.attribute("d").value(), node_xform(xf, n),
                            inherit_style(style, n), sink);
        } else if (std::strcmp(name, "rect") == 0) {
            check_attributes(n, {"x", "y", "width", "height", "rx", "ry"});
            handle_rect(n, inherit_style(style, n), xf, sink);
        } else if (std::strcmp(name, "circle") == 0) {
            check_attributes(n, {"cx", "cy", "r"});
            handle_ellipse_like(n, inherit_style(style, n), xf, sink,
                                attr_number(n, "r", 0.0),
                                attr_number(n, "r", 0.0));
        } else if (std::strcmp(name, "ellipse") == 0) {
            check_attributes(n, {"cx", "cy", "rx", "ry"});
            handle_ellipse_like(n, inherit_style(style, n), xf, sink,
                                attr_number(n, "rx", 0.0),
                                attr_number(n, "ry", 0.0));
        } else if (std::strcmp(name, "line") == 0) {
            check_attributes(n, {"x1", "y1", "x2", "y2"});
            Style st = inherit_style(style, n);
            std::vector<std::pair<double, double>> pts = {
                {attr_number(n, "x1", 0.0), attr_number(n, "y1", 0.0)},
                {attr_number(n, "x2", 0.0), attr_number(n, "y2", 0.0)},
            };
            sink.add_stroke(ring_to_model(pts, node_xform(xf, n)), false, st);
        } else if (std::strcmp(name, "polyline") == 0) {
            check_attributes(n, {"points"});
            handle_poly_points(n, inherit_style(style, n), xf, sink, false);
        } else if (std::strcmp(name, "polygon") == 0) {
            check_attributes(n, {"points"});
            handle_poly_points(n, inherit_style(style, n), xf, sink, true);
        } else {
            fail(std::string("unsupported element <") + name +
                 ">; the SVG symbol subset supports svg, g, path, rect, "
                 "circle, ellipse, line, polyline, polygon, title, desc");
        }
    }
}

} // namespace

// ===========================================================================
// Public import API
// ===========================================================================

SymbolDefinition import_symbol_from_svg_string(const std::string& svg,
                                               const std::string& key) {
    f4::xml::xml_document doc;
    const f4::xml::xml_parse_result result = doc.load_string(svg.c_str());
    if (!result) {
        fail(std::string("XML parse error at offset ") +
             std::to_string(result.offset) + ": " + result.description());
    }

    const f4::xml::xml_node root = doc.document_element();
    if (!root || std::strcmp(root.name(), "svg") != 0) {
        fail("root element must be <svg>");
    }
    if (!root.attribute("viewBox")) {
        fail("root <svg> requires a viewBox (width/height alone don't define "
             "the symbol extent in this subset)");
    }

    // viewBox -> model space: uniform scale 1/max(halfW, halfH), centered.
    double vb_nums[4] = {0, 0, 0, 0};
    {
        const char* p = root.attribute("viewBox").value();
        for (double& v : vb_nums) v = require_number(p, "in viewBox (minx miny w h)");
        skip_sep(p);
        if (*p) fail("viewBox takes exactly 4 numbers");
    }
    if (vb_nums[2] <= 0.0 || vb_nums[3] <= 0.0) fail("viewBox width/height must be > 0");

    // Root attribute validation (same policy as child elements).
    for (f4::xml::xml_attribute a : root.attributes()) {
        const char* name = a.name();
        if (std::strncmp(name, "xmlns", 5) == 0) continue;
        if (name_in(name, {"viewBox", "width", "height", "version", "baseProfile"})) continue;
        if (name_in(name, kPresentationAttrs)) continue;
        if (tolerated_identity(name, a.value())) continue;
        if (is_dangerous_attr(name)) {
            fail(std::string("attribute '") + name +
                 "' on <svg> changes rendering and is outside the SVG symbol subset");
        }
    }

    const double scale = 1.0 / std::max(vb_nums[2] / 2.0, vb_nums[3] / 2.0);
    const double ccx = vb_nums[0] + vb_nums[2] / 2.0;
    const double ccy = vb_nums[1] + vb_nums[3] / 2.0;

    Xform vb;
    vb.a = static_cast<float>(scale);
    vb.d = static_cast<float>(scale);
    vb.e = static_cast<float>(-ccx * scale);
    vb.f = static_cast<float>(-ccy * scale);

    Sink sink;
    sink.vb_scale = static_cast<float>(scale);
    sink.def.key = key;

    ImportMeta meta;
    walk(root, inherit_style(Style{}, root), node_xform(vb, root), sink, meta);

    sink.def.display_name = meta.display_name.empty() ? key : meta.display_name;
    sink.def.description = meta.description;
    refresh_fill_caches(sink.def);
    return sink.def;
}

SymbolDefinition import_symbol_from_svg_file(const std::filesystem::path& path,
                                             const std::string& key) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("f4::renderer::import_symbol_from_svg_file: cannot open '" +
                                 path.string() + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return import_symbol_from_svg_string(ss.str(), key);
}

SymbolDefinition import_symbol_from_svg_file(const std::filesystem::path& path) {
    return import_symbol_from_svg_file(path, path.stem().string());
}

// ===========================================================================
// Export
// ===========================================================================

namespace {

std::string fmt_num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

void append_ring_d(std::string& d, const std::vector<SymbolPoint>& ring, bool close) {
    d += "M ";
    d += fmt_num(ring[0].x);
    d += ' ';
    d += fmt_num(ring[0].y);
    for (std::size_t i = 1; i < ring.size(); ++i) {
        d += " L ";
        d += fmt_num(ring[i].x);
        d += ' ';
        d += fmt_num(ring[i].y);
    }
    if (close) d += " Z";
}

} // namespace

std::string symbol_to_svg(const SymbolDefinition& def) {
    const float half_ref = kSymbolReferenceSizePx * 0.5f;

    f4::xml::xml_document doc;
    f4::xml::xml_node decl = doc.prepend_child(f4::xml::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    f4::xml::xml_node svg = doc.append_child("svg");
    svg.append_attribute("xmlns").set_value("http://www.w3.org/2000/svg");
    svg.append_attribute("viewBox").set_value("-1 -1 2 2");
    if (!def.display_name.empty()) {
        svg.append_child("title").append_child(f4::xml::node_pcdata)
           .set_value(def.display_name.c_str());
    }
    if (!def.description.empty()) {
        svg.append_child("desc").append_child(f4::xml::node_pcdata)
           .set_value(def.description.c_str());
    }

    for (const auto& pg : def.polygons) {
        if (pg.points.size() < 2) continue;
        std::string d;
        append_ring_d(d, pg.points, true);
        for (const auto& h : pg.holes) {
            d += ' ';
            append_ring_d(d, h, true);
        }
        f4::xml::xml_node path = svg.append_child("path");
        path.append_attribute("d").set_value(d.c_str());
        if (pg.filled) {
            path.append_attribute("fill").set_value(
                pg.color_role == SymbolColorRole::Outline ? "#000000" : "currentColor");
            if (pg.color_role == SymbolColorRole::FillBlend) {
                path.append_attribute("data-color-role").set_value("fill_blend");
            } else if (pg.color_role == SymbolColorRole::Outline) {
                path.append_attribute("data-color-role").set_value("outline");
            }
        } else {
            // The model renders unfilled polygons as a fixed 1px outline.
            path.append_attribute("fill").set_value("none");
            path.append_attribute("stroke").set_value("#000000");
            path.append_attribute("stroke-width").set_value(fmt_num(1.0 / half_ref).c_str());
        }
    }

    for (const auto& pl : def.polylines) {
        if (pl.points.size() < 2) continue;
        std::string d;
        append_ring_d(d, pl.points, pl.closed);
        f4::xml::xml_node path = svg.append_child("path");
        path.append_attribute("d").set_value(d.c_str());
        path.append_attribute("fill").set_value("none");
        path.append_attribute("stroke").set_value(
            pl.color_role == SymbolColorRole::Outline ? "#000000" : "currentColor");
        if (pl.color_role == SymbolColorRole::FillBlend) {
            path.append_attribute("data-color-role").set_value("fill_blend");
        }
        path.append_attribute("stroke-width").set_value(fmt_num(pl.width / half_ref).c_str());
    }

    std::ostringstream ss;
    doc.save(ss, "  ");
    return ss.str();
}

void save_symbol_as_svg(const SymbolDefinition& def,
                        const std::filesystem::path& path) {
    const std::string svg = symbol_to_svg(def);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("f4::renderer::save_symbol_as_svg: cannot open '" +
                                 path.string() + "' for writing");
    }
    out.write(svg.data(), static_cast<std::streamsize>(svg.size()));
    if (!out) {
        throw std::runtime_error("f4::renderer::save_symbol_as_svg: write failed for '" +
                                 path.string() + "'");
    }
}

} // namespace f4::renderer
