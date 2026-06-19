#include "svg_import.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <thorvg.h>

#include "svg_path.h"  // figo::appendSvgPath — reused for accurate bbox

namespace figmaedit {
namespace {

using figo::Color;
using figo::GradientStop;
using figo::Mat23;
using figo::Node;
using figo::NodeType;
using figo::Paint;
using figo::PaintType;
using figo::StrokeAlign;

constexpr float kPi = 3.14159265358979323846f;

// ---- tiny string helpers ---------------------------------------------------

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Pull every signed float out of a string (commas/whitespace separated). Used
// for transform args, point lists, viewBox, etc.
std::vector<float> numbers(const std::string& s) {
    std::vector<float> out;
    const char* p = s.c_str();
    while (*p) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
        if (!*p) break;
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) { ++p; continue; }
        out.push_back(v);
        p = end;
    }
    return out;
}

// ---- color parsing ---------------------------------------------------------

const std::unordered_map<std::string, uint32_t>& namedColors() {
    static const std::unordered_map<std::string, uint32_t> m = {
        {"black", 0x000000},   {"white", 0xffffff},   {"red", 0xff0000},
        {"green", 0x008000},   {"blue", 0x0000ff},    {"yellow", 0xffff00},
        {"cyan", 0x00ffff},    {"magenta", 0xff00ff}, {"gray", 0x808080},
        {"grey", 0x808080},    {"silver", 0xc0c0c0},  {"maroon", 0x800000},
        {"olive", 0x808000},   {"lime", 0x00ff00},    {"aqua", 0x00ffff},
        {"teal", 0x008080},    {"navy", 0x000080},    {"fuchsia", 0xff00ff},
        {"purple", 0x800080},  {"orange", 0xffa500},  {"pink", 0xffc0cb},
        {"brown", 0xa52a2a},   {"gold", 0xffd700},    {"indigo", 0x4b0082},
        {"violet", 0xee82ee},  {"transparent", 0x000000}};
    return m;
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse a CSS color token. Returns false for "none"/empty/unsupported.
// "currentColor" resolves via `current`.
bool parseColor(const std::string& raw, const Color& current, Color& out) {
    std::string s = trim(raw);
    if (s.empty()) return false;
    const std::string ls = lower(s);
    if (ls == "none" || ls == "transparent") return false;
    if (ls == "currentcolor") { out = current; return true; }
    out.a = 1.0f;
    if (s[0] == '#') {
        const std::string h = s.substr(1);
        auto set = [&](int r, int g, int b) {
            out.r = r / 255.0f; out.g = g / 255.0f; out.b = b / 255.0f;
        };
        if (h.size() == 3) {
            const int r = hexNibble(h[0]), g = hexNibble(h[1]), b = hexNibble(h[2]);
            if (r < 0 || g < 0 || b < 0) return false;
            set(r * 17, g * 17, b * 17);
            return true;
        }
        if (h.size() == 6 || h.size() == 8) {
            int v[4] = {0, 0, 0, 255};
            for (size_t i = 0; i < h.size(); i += 2) {
                const int hi = hexNibble(h[i]), lo = hexNibble(h[i + 1]);
                if (hi < 0 || lo < 0) return false;
                v[i / 2] = hi * 16 + lo;
            }
            set(v[0], v[1], v[2]);
            out.a = v[3] / 255.0f;
            return true;
        }
        return false;
    }
    if (ls.rfind("rgb", 0) == 0) {
        const auto open = s.find('(');
        const auto close = s.find(')');
        if (open == std::string::npos || close == std::string::npos) return false;
        const auto n = numbers(s.substr(open + 1, close - open - 1));
        if (n.size() < 3) return false;
        auto chan = [](float v) { return std::clamp(v, 0.0f, 255.0f) / 255.0f; };
        // percentages aren't distinguished here; asset packs use 0..255.
        out.r = chan(n[0]); out.g = chan(n[1]); out.b = chan(n[2]);
        out.a = n.size() >= 4 ? std::clamp(n[3], 0.0f, 1.0f) : 1.0f;
        return true;
    }
    const auto it = namedColors().find(ls);
    if (it == namedColors().end()) return false;
    out.r = ((it->second >> 16) & 0xff) / 255.0f;
    out.g = ((it->second >> 8) & 0xff) / 255.0f;
    out.b = (it->second & 0xff) / 255.0f;
    return true;
}

// ---- transforms ------------------------------------------------------------

Mat23 translate(float x, float y) { Mat23 m; m.m02 = x; m.m12 = y; return m; }
Mat23 scale(float x, float y) { Mat23 m; m.m00 = x; m.m11 = y; return m; }

Mat23 rotate(float deg) {
    const float r = deg * kPi / 180.0f, c = std::cos(r), s = std::sin(r);
    Mat23 m;
    m.m00 = c; m.m01 = -s; m.m10 = s; m.m11 = c;
    return m;
}

// Parse an SVG transform list into a single matrix.
Mat23 parseTransform(const std::string& spec) {
    Mat23 acc = Mat23::identity();
    size_t i = 0;
    while (i < spec.size()) {
        while (i < spec.size() && (std::isspace(static_cast<unsigned char>(spec[i])) ||
                                   spec[i] == ',')) ++i;
        const size_t name0 = i;
        while (i < spec.size() && (std::isalpha(static_cast<unsigned char>(spec[i])))) ++i;
        const std::string fn = spec.substr(name0, i - name0);
        const auto open = spec.find('(', i);
        if (fn.empty() || open == std::string::npos) break;
        const auto close = spec.find(')', open);
        if (close == std::string::npos) break;
        const auto a = numbers(spec.substr(open + 1, close - open - 1));
        Mat23 m = Mat23::identity();
        if (fn == "translate") m = translate(a.size() > 0 ? a[0] : 0, a.size() > 1 ? a[1] : 0);
        else if (fn == "scale") m = scale(a.size() > 0 ? a[0] : 1, a.size() > 1 ? a[1] : (a.size() > 0 ? a[0] : 1));
        else if (fn == "rotate") {
            if (a.size() >= 3) m = translate(a[1], a[2]) * rotate(a[0]) * translate(-a[1], -a[2]);
            else if (!a.empty()) m = rotate(a[0]);
        } else if (fn == "matrix" && a.size() >= 6) {
            m.m00 = a[0]; m.m10 = a[1]; m.m01 = a[2]; m.m11 = a[3]; m.m02 = a[4]; m.m12 = a[5];
        } else if (fn == "skewX" && !a.empty()) {
            m.m01 = std::tan(a[0] * kPi / 180.0f);
        } else if (fn == "skewY" && !a.empty()) {
            m.m10 = std::tan(a[0] * kPi / 180.0f);
        }
        acc = acc * m;
        i = close + 1;
    }
    return acc;
}

// ---- minimal XML -----------------------------------------------------------

struct XmlNode {
    std::string tag;
    std::unordered_map<std::string, std::string> attr;
    std::vector<XmlNode> kids;
};

std::string decodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            const auto semi = s.find(';', i);
            if (semi != std::string::npos) {
                const std::string e = s.substr(i + 1, semi - i - 1);
                if (e == "amp") out.push_back('&');
                else if (e == "lt") out.push_back('<');
                else if (e == "gt") out.push_back('>');
                else if (e == "quot") out.push_back('"');
                else if (e == "apos") out.push_back('\'');
                else if (!e.empty() && e[0] == '#') out.push_back(static_cast<char>(std::atoi(e.c_str() + 1)));
                else { out.append(s, i, semi - i + 1); }
                i = semi + 1;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

struct XmlParser {
    const std::string& src;
    size_t i = 0;
    explicit XmlParser(const std::string& s) : src(s) {}

    void skipWs() {
        while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
    }

    // Skip <?...?>, <!-- -->, <!...> (doctype / CDATA, best-effort).
    bool skipMisc() {
        bool any = false;
        for (;;) {
            skipWs();
            if (i + 1 >= src.size() || src[i] != '<') break;
            if (src[i + 1] == '?') { const auto e = src.find("?>", i); i = (e == std::string::npos) ? src.size() : e + 2; any = true; }
            else if (src.compare(i, 4, "<!--") == 0) { const auto e = src.find("-->", i); i = (e == std::string::npos) ? src.size() : e + 3; any = true; }
            else if (src.compare(i, 9, "<![CDATA[") == 0) { const auto e = src.find("]]>", i); i = (e == std::string::npos) ? src.size() : e + 3; any = true; }
            else if (src[i + 1] == '!') { const auto e = src.find('>', i); i = (e == std::string::npos) ? src.size() : e + 1; any = true; }
            else break;
        }
        return any;
    }

    // Parse one element starting at '<tag'. Returns false on malformed input.
    bool parseElement(XmlNode& node) {
        if (i >= src.size() || src[i] != '<') return false;
        ++i;  // consume '<'
        const size_t t0 = i;
        while (i < src.size() && !std::isspace(static_cast<unsigned char>(src[i])) &&
               src[i] != '>' && src[i] != '/') ++i;
        node.tag = src.substr(t0, i - t0);
        // attributes
        for (;;) {
            skipWs();
            if (i >= src.size()) return false;
            if (src[i] == '/') {  // self-closing
                i = src.find('>', i);
                if (i == std::string::npos) return false;
                ++i;
                return true;
            }
            if (src[i] == '>') { ++i; break; }
            const size_t n0 = i;
            while (i < src.size() && src[i] != '=' && !std::isspace(static_cast<unsigned char>(src[i])) &&
                   src[i] != '>' && src[i] != '/') ++i;
            const std::string name = src.substr(n0, i - n0);
            skipWs();
            std::string value;
            if (i < src.size() && src[i] == '=') {
                ++i; skipWs();
                if (i < src.size() && (src[i] == '"' || src[i] == '\'')) {
                    const char q = src[i++];
                    const size_t v0 = i;
                    while (i < src.size() && src[i] != q) ++i;
                    value = decodeEntities(src.substr(v0, i - v0));
                    if (i < src.size()) ++i;
                }
            }
            if (!name.empty()) node.attr[name] = value;
        }
        // children until </tag>
        for (;;) {
            // text run
            const size_t txt = i;
            while (i < src.size() && src[i] != '<') ++i;
            (void)txt;
            if (i >= src.size()) return false;
            if (src.compare(i, 2, "</") == 0) {  // close tag
                const auto e = src.find('>', i);
                i = (e == std::string::npos) ? src.size() : e + 1;
                return true;
            }
            if (skipMisc()) continue;
            if (i < src.size() && src[i] == '<') {
                XmlNode kid;
                if (!parseElement(kid)) return false;
                node.kids.push_back(std::move(kid));
            }
        }
    }
};

const std::string* attr(const XmlNode& n, const char* k) {
    const auto it = n.attr.find(k);
    return it == n.attr.end() ? nullptr : &it->second;
}

// ---- graphics state (inherited presentation attributes) --------------------

struct GState {
    Mat23 ctm = Mat23::identity();
    std::string fill = "#000000";  // SVG default fill is black
    std::string stroke = "none";
    float strokeWidth = 1.0f;
    float fillOpacity = 1.0f;
    float strokeOpacity = 1.0f;
    float groupOpacity = 1.0f;
    bool evenOdd = false;
    Color current{0, 0, 0, 1};  // currentColor
};

// Split a style="a:b;c:d" string into a map.
std::unordered_map<std::string, std::string> parseStyle(const std::string& s) {
    std::unordered_map<std::string, std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        const auto semi = s.find(';', i);
        const std::string decl = s.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
        const auto colon = decl.find(':');
        if (colon != std::string::npos)
            out[trim(lower(decl.substr(0, colon)))] = trim(decl.substr(colon + 1));
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
    return out;
}

// Resolve a presentation property: style="" wins over the attribute.
std::string prop(const XmlNode& n, const std::unordered_map<std::string, std::string>& style,
                 const char* key) {
    const auto sit = style.find(key);
    if (sit != style.end()) return sit->second;
    if (const std::string* a = attr(n, key)) return *a;
    return std::string();
}

void inheritState(const XmlNode& n, GState& st) {
    const std::unordered_map<std::string, std::string> style =
        attr(n, "style") ? parseStyle(*attr(n, "style")) : std::unordered_map<std::string, std::string>{};
    std::string v;
    if (!(v = prop(n, style, "color")).empty()) parseColor(v, st.current, st.current);
    if (!(v = prop(n, style, "fill")).empty()) st.fill = v;
    if (!(v = prop(n, style, "stroke")).empty()) st.stroke = v;
    if (!(v = prop(n, style, "stroke-width")).empty()) { const auto x = numbers(v); if (!x.empty()) st.strokeWidth = x[0]; }
    if (!(v = prop(n, style, "fill-opacity")).empty()) { const auto x = numbers(v); if (!x.empty()) st.fillOpacity = x[0]; }
    if (!(v = prop(n, style, "stroke-opacity")).empty()) { const auto x = numbers(v); if (!x.empty()) st.strokeOpacity = x[0]; }
    if (!(v = prop(n, style, "fill-rule")).empty()) st.evenOdd = (lower(v) == "evenodd");
    if (!(v = prop(n, style, "opacity")).empty()) { const auto x = numbers(v); if (!x.empty()) st.groupOpacity *= x[0]; }
}

// ---- shape → path data -----------------------------------------------------

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.4g", v);
    return b;
}

std::string roundedRectPath(float x, float y, float w, float h, float rx, float ry) {
    rx = std::min(rx, w / 2);
    ry = std::min(ry, h / 2);
    if (rx <= 0 || ry <= 0) {
        return "M" + fmt(x) + " " + fmt(y) + "h" + fmt(w) + "v" + fmt(h) + "h" + fmt(-w) + "z";
    }
    std::string d;
    d += "M" + fmt(x + rx) + " " + fmt(y);
    d += "h" + fmt(w - 2 * rx);
    d += "a" + fmt(rx) + " " + fmt(ry) + " 0 0 1 " + fmt(rx) + " " + fmt(ry);
    d += "v" + fmt(h - 2 * ry);
    d += "a" + fmt(rx) + " " + fmt(ry) + " 0 0 1 " + fmt(-rx) + " " + fmt(ry);
    d += "h" + fmt(-(w - 2 * rx));
    d += "a" + fmt(rx) + " " + fmt(ry) + " 0 0 1 " + fmt(-rx) + " " + fmt(-ry);
    d += "v" + fmt(-(h - 2 * ry));
    d += "a" + fmt(rx) + " " + fmt(ry) + " 0 0 1 " + fmt(rx) + " " + fmt(-ry);
    d += "z";
    return d;
}

std::string ellipsePath(float cx, float cy, float rx, float ry) {
    // two arcs
    return "M" + fmt(cx - rx) + " " + fmt(cy) + "a" + fmt(rx) + " " + fmt(ry) + " 0 1 0 " +
           fmt(2 * rx) + " 0" + "a" + fmt(rx) + " " + fmt(ry) + " 0 1 0 " + fmt(-2 * rx) + " 0z";
}

std::string pointsPath(const std::string& pts, bool close) {
    const auto n = numbers(pts);
    if (n.size() < 4) return std::string();
    std::string d = "M" + fmt(n[0]) + " " + fmt(n[1]);
    for (size_t k = 2; k + 1 < n.size(); k += 2) d += "L" + fmt(n[k]) + " " + fmt(n[k + 1]);
    if (close) d += "z";
    return d;
}

// Returns the drawable's path data and whether it is closeable (for stroke-only
// elements like <line>/<polyline> there is no fill).
struct Geom { std::string d; bool fillable = true; };

Geom elementGeom(const XmlNode& n) {
    const std::string tag = n.tag;
    auto num = [&](const char* k, float def) {
        const std::string* a = attr(n, k);
        if (!a) return def;
        const auto v = numbers(*a);
        return v.empty() ? def : v[0];
    };
    if (tag == "path") { const std::string* d = attr(n, "d"); return {d ? *d : std::string(), true}; }
    if (tag == "rect") {
        float rx = attr(n, "rx") ? num("rx", 0) : -1, ry = attr(n, "ry") ? num("ry", 0) : -1;
        if (rx < 0 && ry >= 0) rx = ry;
        if (ry < 0 && rx >= 0) ry = rx;
        if (rx < 0) rx = 0;
        if (ry < 0) ry = 0;
        return {roundedRectPath(num("x", 0), num("y", 0), num("width", 0), num("height", 0), rx, ry), true};
    }
    if (tag == "circle") { const float r = num("r", 0); return {ellipsePath(num("cx", 0), num("cy", 0), r, r), true}; }
    if (tag == "ellipse") return {ellipsePath(num("cx", 0), num("cy", 0), num("rx", 0), num("ry", 0)), true};
    if (tag == "line")
        return {"M" + fmt(num("x1", 0)) + " " + fmt(num("y1", 0)) + "L" + fmt(num("x2", 0)) + " " + fmt(num("y2", 0)), false};
    if (tag == "polygon") { const std::string* p = attr(n, "points"); return {p ? pointsPath(*p, true) : std::string(), true}; }
    if (tag == "polyline") { const std::string* p = attr(n, "points"); return {p ? pointsPath(*p, false) : std::string(), false}; }
    return {std::string(), true};
}

// Accurate local bbox via ThorVG's path parser.
bool pathBounds(const std::string& d, float& x, float& y, float& w, float& h) {
    auto* s = tvg::Shape::gen();
    if (!s) return false;
    bool ok = figo::appendSvgPath(*s, d.c_str());
    if (ok) ok = s->bounds(&x, &y, &w, &h) == tvg::Result::Success;
    tvg::Paint::rel(s);
    return ok && w > 0 && h > 0;
}

// ---- gradients -------------------------------------------------------------

struct GradientIndex {
    std::unordered_map<std::string, const XmlNode*> byId;
    void collect(const XmlNode& n) {
        if ((n.tag == "linearGradient" || n.tag == "radialGradient") && attr(n, "id"))
            byId[*attr(n, "id")] = &n;
        for (const auto& k : n.kids) collect(k);
    }
};

std::string refId(const std::string& url) {  // "url(#id)" → "id"
    const auto h = url.find('#');
    if (h == std::string::npos) return std::string();
    std::string id = url.substr(h + 1);
    const auto close = id.find(')');
    if (close != std::string::npos) id = id.substr(0, close);
    return trim(id);
}

// Gather stops, following href inheritance.
void gatherStops(const GradientIndex& idx, const XmlNode& g, std::vector<GradientStop>& out,
                 const Color& current, int depth = 0) {
    if (depth > 8) return;
    const std::string* href = attr(g, "xlink:href");
    if (!href) href = attr(g, "href");
    if (href) {
        const auto it = idx.byId.find(refId(*href));
        if (it != idx.byId.end()) gatherStops(idx, *it->second, out, current, depth + 1);
    }
    std::vector<GradientStop> local;
    for (const auto& s : g.kids) {
        if (s.tag != "stop") continue;
        const auto style = attr(s, "style") ? parseStyle(*attr(s, "style"))
                                            : std::unordered_map<std::string, std::string>{};
        GradientStop gs;
        std::string off = prop(s, style, "offset");
        if (off.empty()) { const std::string* a = attr(s, "offset"); if (a) off = *a; }
        const auto on = numbers(off);
        gs.position = on.empty() ? 0.0f : (off.find('%') != std::string::npos ? on[0] / 100.0f : on[0]);
        gs.position = std::clamp(gs.position, 0.0f, 1.0f);
        Color c{0, 0, 0, 1};
        std::string sc = prop(s, style, "stop-color");
        if (sc.empty()) sc = "#000000";
        parseColor(sc, current, c);
        std::string so = prop(s, style, "stop-opacity");
        if (!so.empty()) { const auto x = numbers(so); if (!x.empty()) c.a *= x[0]; }
        gs.color = c;
        local.push_back(gs);
    }
    if (!local.empty()) out = std::move(local);  // own stops override inherited
}

template <typename F>
std::string gradAttr(const GradientIndex& idx, const XmlNode& g, const char* key, int depth, F def) {
    if (const std::string* a = attr(g, key)) return *a;
    const std::string* href = attr(g, "xlink:href");
    if (!href) href = attr(g, "href");
    if (href && depth < 8) {
        const auto it = idx.byId.find(refId(*href));
        if (it != idx.byId.end()) return gradAttr(idx, *it->second, key, depth + 1, def);
    }
    return def();
}

// Build a figo gradient Paint. Handles are fractions of the node's local box
// [0..nodeW] x [0..nodeH], matching makeGradient() in scene_builder.
bool buildGradient(const GradientIndex& idx, const std::string& id, float bx, float by,
                   float bw, float bh, float nodeW, float nodeH, const Color& current,
                   float fillOpacity, Paint& out) {
    const auto it = idx.byId.find(id);
    if (it == idx.byId.end()) return false;
    const XmlNode& g = *it->second;
    std::vector<GradientStop> stops;
    gatherStops(idx, g, stops, current);
    if (stops.size() < 2) {
        if (stops.size() == 1) stops.push_back(stops[0]);  // degenerate → solid-ish
        else return false;
    }
    for (auto& s : stops) s.color.a *= fillOpacity;

    const bool userSpace =
        gradAttr(idx, g, "gradientUnits", 0, [] { return std::string("objectBoundingBox"); }) ==
        "userSpaceOnUse";
    const std::string gtSpec = gradAttr(idx, g, "gradientTransform", 0, [] { return std::string(); });
    const Mat23 gt = gtSpec.empty() ? Mat23::identity() : parseTransform(gtSpec);

    // Map a gradient-space point to the node's local box, then normalize.
    auto toHandle = [&](float px, float py, std::array<float, 2>& h) {
        float ux, uy;
        if (userSpace) {
            gt.apply(px, py, ux, uy);  // already in user/local space
        } else {  // objectBoundingBox: px,py are 0..1 of the element bbox
            float tx, ty;
            gt.apply(px, py, tx, ty);
            ux = bx + tx * bw;
            uy = by + ty * bh;
        }
        h[0] = nodeW > 0 ? ux / nodeW : 0;
        h[1] = nodeH > 0 ? uy / nodeH : 0;
    };

    out = Paint{};
    out.stops = std::move(stops);
    out.opacity = 1.0f;
    if (g.tag == "radialGradient") {
        out.type = PaintType::GradientRadial;
        const float cx = std::strtof(gradAttr(idx, g, "cx", 0, [] { return std::string("0.5"); }).c_str(), nullptr);
        const float cy = std::strtof(gradAttr(idx, g, "cy", 0, [] { return std::string("0.5"); }).c_str(), nullptr);
        const float r = std::strtof(gradAttr(idx, g, "r", 0, [] { return std::string("0.5"); }).c_str(), nullptr);
        toHandle(cx, cy, out.handle0);
        toHandle(cx + r, cy, out.handle1);
        toHandle(cx, cy + r, out.handle2);
    } else {
        out.type = PaintType::GradientLinear;
        const float x1 = std::strtof(gradAttr(idx, g, "x1", 0, [] { return std::string("0"); }).c_str(), nullptr);
        const float y1 = std::strtof(gradAttr(idx, g, "y1", 0, [] { return std::string("0"); }).c_str(), nullptr);
        const float x2 = std::strtof(gradAttr(idx, g, "x2", 0, [] { return std::string("1"); }).c_str(), nullptr);
        const float y2 = std::strtof(gradAttr(idx, g, "y2", 0, [] { return std::string("0"); }).c_str(), nullptr);
        toHandle(x1, y1, out.handle0);
        toHandle(x2, y2, out.handle1);
        out.handle2 = {out.handle0[0], out.handle0[1] + 1.0f};
    }
    return true;
}

// ---- recolor ---------------------------------------------------------------

std::string hexOf(const Color& c) {
    char b[8];
    std::snprintf(b, sizeof(b), "%02x%02x%02x", static_cast<int>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 255)),
                  static_cast<int>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 255)),
                  static_cast<int>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 255)));
    return b;
}

void recolor(Paint& p, const SvgImportOptions& opt) {
    if (p.type != PaintType::Solid) return;
    if (!opt.palette.empty()) {
        const auto it = opt.palette.find(hexOf(p.color));
        if (it != opt.palette.end()) {
            Color c = p.color;
            parseColor(it->second, c, c);
            p.color.r = c.r; p.color.g = c.g; p.color.b = c.b;
            return;
        }
    }
    if (!opt.monochrome.empty()) {
        Color c = p.color;
        if (parseColor(opt.monochrome, c, c)) { p.color.r = c.r; p.color.g = c.g; p.color.b = c.b; }
    }
}

// ---- tree walk -------------------------------------------------------------

std::unique_ptr<Node> makeVector(const XmlNode& el, const GState& st, const GradientIndex& idx,
                                 const SvgImportOptions& opt) {
    const Geom geom = elementGeom(el);
    if (trim(geom.d).empty()) return nullptr;

    float bx, by, bw, bh;
    if (!pathBounds(geom.d, bx, by, bw, bh)) return nullptr;
    const float nodeW = std::max(1.0f, bx + bw);  // origin-anchored local box
    const float nodeH = std::max(1.0f, by + bh);

    auto node = std::make_unique<Node>();
    node->type = NodeType::Vector;
    node->name = el.tag == "path" ? "Path" : el.tag;
    node->width = node->baseWidth = nodeW;
    node->height = node->baseHeight = nodeH;
    node->relativeTransform = st.ctm;
    node->baseTransform = st.ctm;
    node->opacity = std::clamp(st.groupOpacity, 0.0f, 1.0f);

    figo::PathGeometry pg;
    pg.path = geom.d;
    pg.evenOdd = st.evenOdd;
    node->fillGeometry.push_back(pg);

    // fill
    if (geom.fillable && lower(trim(st.fill)) != "none") {
        if (st.fill.rfind("url(", 0) == 0) {
            Paint p;
            if (buildGradient(idx, refId(st.fill), bx, by, bw, bh, nodeW, nodeH, st.current,
                              st.fillOpacity, p))
                node->fills.push_back(p);
        } else {
            Color c{0, 0, 0, 1};
            if (parseColor(st.fill, st.current, c)) {
                Paint p;
                p.type = PaintType::Solid;
                c.a *= st.fillOpacity;
                p.color = c;
                recolor(p, opt);
                node->fills.push_back(p);
            }
        }
    }
    // stroke
    if (lower(trim(st.stroke)) != "none") {
        Color c{0, 0, 0, 1};
        if (st.stroke.rfind("url(", 0) != 0 && parseColor(st.stroke, st.current, c)) {
            Paint p;
            p.type = PaintType::Solid;
            c.a *= st.strokeOpacity;
            p.color = c;
            recolor(p, opt);
            node->strokes.push_back(p);
            node->strokeWeight = std::max(0.0f, st.strokeWidth);
            node->strokeAlign = StrokeAlign::Center;
            // Leave strokeGeometry empty: figo fills strokeGeometry (it expects
            // pre-outlined stroke paths), whereas we want live width-based
            // stroking of the fillGeometry outline.
        }
    }
    if (node->fills.empty() && node->strokes.empty()) return nullptr;
    return node;
}

void walk(const XmlNode& n, GState st, const GradientIndex& idx, const SvgImportOptions& opt,
          std::vector<std::unique_ptr<Node>>& out) {
    if (const std::string* t = attr(n, "transform")) st.ctm = st.ctm * parseTransform(*t);
    inheritState(n, st);

    const std::string& tag = n.tag;
    if (tag == "g" || tag == "svg" || tag == "a" || tag == "switch") {
        for (const auto& k : n.kids) walk(k, st, idx, opt, out);
        return;
    }
    if (tag == "defs" || tag == "linearGradient" || tag == "radialGradient" ||
        tag == "clipPath" || tag == "mask" || tag == "symbol" || tag == "style" ||
        tag == "title" || tag == "desc" || tag == "metadata" || tag == "filter")
        return;  // not drawn (gradients already indexed)

    if (auto v = makeVector(n, st, idx, opt)) out.push_back(std::move(v));
}

}  // namespace

std::unique_ptr<Node> importSvg(const std::string& svg, const SvgImportOptions& opt,
                                std::string& err) {
    XmlParser xp(svg);
    xp.skipMisc();
    if (xp.i >= svg.size() || svg[xp.i] != '<') { err = "no XML element found"; return nullptr; }
    XmlNode root;
    if (!xp.parseElement(root)) { err = "malformed XML"; return nullptr; }
    if (lower(root.tag) != "svg") { err = "root element is <" + root.tag + ">, expected <svg>"; return nullptr; }

    // viewBox / intrinsic size
    float vbX = 0, vbY = 0, vbW = 0, vbH = 0;
    if (const std::string* vb = attr(root, "viewBox")) {
        const auto v = numbers(*vb);
        if (v.size() == 4) { vbX = v[0]; vbY = v[1]; vbW = v[2]; vbH = v[3]; }
    }
    auto dim = [&](const char* k) -> float {
        const std::string* a = attr(root, k);
        if (!a) return 0;
        const auto v = numbers(*a);
        return v.empty() ? 0 : v[0];
    };
    if (vbW <= 0 || vbH <= 0) { vbW = dim("width"); vbH = dim("height"); }
    if (vbW <= 0 || vbH <= 0) { err = "SVG has no viewBox or width/height"; return nullptr; }

    const float targetW = opt.width > 0 ? opt.width : vbW;
    const float targetH = opt.height > 0 ? opt.height : vbH;

    GradientIndex idx;
    idx.collect(root);

    GState st;
    // root CTM: viewBox space → target box at origin
    st.ctm = scale(targetW / vbW, targetH / vbH) * translate(-vbX, -vbY);
    // The <svg> element's own presentation attributes (fill/stroke/…) and any
    // transform inherit into its children.
    if (const std::string* t = attr(root, "transform")) st.ctm = st.ctm * parseTransform(*t);
    inheritState(root, st);

    std::vector<std::unique_ptr<Node>> children;
    for (const auto& k : root.kids) walk(k, st, idx, opt, children);
    if (children.empty()) { err = "no drawable elements found"; return nullptr; }

    auto frame = std::make_unique<Node>();
    frame->type = NodeType::Frame;
    frame->name = opt.name.empty() ? "SVG" : opt.name;
    frame->width = frame->baseWidth = targetW;
    frame->height = frame->baseHeight = targetH;
    frame->relativeTransform = translate(opt.x, opt.y);
    frame->baseTransform = frame->relativeTransform;
    frame->clipsContent = true;
    for (auto& c : children) {
        c->parent = frame.get();
        frame->children.push_back(std::move(c));
    }
    return frame;
}

}  // namespace figmaedit
