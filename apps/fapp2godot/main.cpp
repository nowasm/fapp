// fapp2godot — convert a figmalib design (.fig / canvas.json / REST JSON) into
// a Godot 4 project: one .tscn per top-level frame + deduplicated PNG sprites +
// a manifest.json. The design's own ThorVG rasterizer bakes the sprites, so the
// textures are pixel-identical to the figmalib runtime.
//
// Node mapping:
//   TEXT                                   -> Label (vector text, font/size/color)
//   plain solid rectangle / container bg   -> ColorRect (crisp, resolution-free)
//   rounded panel (corner radius)          -> NinePatchRect (stretchable) + sprite
//   ellipse / vector / gradient / image /
//     stroke / effect                      -> TextureRect + baked sprite
//   container                              -> Control holding a baked bg + children
//   anything empty                         -> Control (passthrough)
//
// Sprites are baked with opacity forced to 1 and content-hashed, so the same
// shape at any opacity shares one PNG. Godot's modulate carries node opacity.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <figmalib/figmalib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using figmalib::Node;
using figmalib::NodeType;
using nlohmann::json;

// ===================== PNG encoder (RGBA8, self-contained) ==================
// Minimal valid PNG: stored (uncompressed) zlib stream. Larger files than a
// real deflate, but Godot reads them fine and there are zero dependencies.

static uint32_t g_crc[256];
static bool g_crcInit = false;
static void crcInit() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc[n] = c;
    }
    g_crcInit = true;
}
static uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    if (!g_crcInit) crcInit();
    for (size_t i = 0; i < n; ++i) crc = g_crc[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc;
}
static void put32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xff);
    v.push_back((x >> 16) & 0xff);
    v.push_back((x >> 8) & 0xff);
    v.push_back(x & 0xff);
}
static void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32be(out, static_cast<uint32_t>(data.size()));
    size_t typeStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = crc32(out.data() + typeStart, 4 + data.size());
    put32be(out, crc ^ 0xFFFFFFFFu);
}

// rgba: row-major uint32, memory byte order R,G,B,A (ThorVG ABGR8888S).
static bool writePng(const fs::path& path, const uint32_t* rgba, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    // Filtered raw data: each scanline prefixed with filter byte 0.
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + w * 4));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            uint32_t v = rgba[static_cast<size_t>(y) * w + x];
            raw.push_back(v & 0xff);          // R
            raw.push_back((v >> 8) & 0xff);   // G
            raw.push_back((v >> 16) & 0xff);  // B
            raw.push_back((v >> 24) & 0xff);  // A
        }
    }
    // zlib stream: stored deflate blocks + adler32.
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        bool final = (off + n) >= raw.size();
        z.push_back(final ? 1 : 0);
        z.push_back(n & 0xff);
        z.push_back((n >> 8) & 0xff);
        z.push_back(~n & 0xff);
        z.push_back((~n >> 8) & 0xff);
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put32be(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put32be(ihdr, w);
    put32be(ihdr, h);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // color type RGBA
    ihdr.push_back(0);  // compression
    ihdr.push_back(0);  // filter
    ihdr.push_back(0);  // interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), out.size());
    return static_cast<bool>(f);
}

// ===================== helpers ==============================================

static std::string sanitizeName(const std::string& name, const char* fallback) {
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || c == '.' || c == '/' || c == ':' || c == '@' || c == '%' ||
            c == '$' || c == '"' || c == '\\')
            out.push_back('_');
        else
            out.push_back(static_cast<char>(c));
    }
    size_t bgn = out.find_first_not_of(' ');
    size_t end = out.find_last_not_of(' ');
    if (bgn == std::string::npos) return fallback;
    out = out.substr(bgn, end - bgn + 1);
    return out.empty() ? std::string(fallback) : out;
}

static std::string escapeStr(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

static std::string num(float v) {
    if (std::fabs(v) < 1e-4f) return "0";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (s[last] == '.') --last;
        s.erase(last + 1);
    }
    return s;
}

static std::string colorLit(const figmalib::Color& c) {
    return "Color(" + num(c.r) + ", " + num(c.g) + ", " + num(c.b) + ", " + num(c.a) + ")";
}

static bool isRectish(NodeType t) {
    switch (t) {
        case NodeType::Rectangle:
        case NodeType::Frame:
        case NodeType::Group:
        case NodeType::Section:
        case NodeType::Component:
        case NodeType::ComponentSet:
        case NodeType::Instance:
        case NodeType::Canvas:
            return true;
        default:
            return false;
    }
}

static const figmalib::Paint* solidFill(const Node& n) {
    for (const auto& p : n.fills)
        if (p.visible && p.type == figmalib::PaintType::Solid) return &p;
    return nullptr;
}
static bool hasVisibleStroke(const Node& n) {
    for (const auto& p : n.strokes)
        if (p.visible) return true;
    return false;
}
static bool hasVisibleEffect(const Node& n) {
    for (const auto& e : n.effects)
        if (e.visible) return true;
    return false;
}
static bool nonSolidVisibleFill(const Node& n) {
    for (const auto& p : n.fills)
        if (p.visible && p.type != figmalib::PaintType::Solid) return true;
    return false;
}
static float cornerR(const Node& n) {
    if (n.cornerRadius > 0) return n.cornerRadius;
    if (n.rectangleCornerRadii) {
        const auto& r = *n.rectangleCornerRadii;
        return std::max({r[0], r[1], r[2], r[3]});
    }
    return 0;
}

static bool subtreeHasText(const Node& n) {
    if (n.type == NodeType::Text) return true;
    for (const auto& c : n.children)
        if (subtreeHasText(*c)) return true;
    return false;
}
static bool subtreeHasVector(const Node& n) {
    switch (n.type) {
        case NodeType::Vector:
        case NodeType::BooleanOperation:
        case NodeType::Star:
        case NodeType::RegularPolygon:
        case NodeType::Line:
            return true;
        default:
            break;
    }
    for (const auto& c : n.children)
        if (subtreeHasVector(*c)) return true;
    return false;
}

// A multi-part vector icon (or a boolean operation) is best rasterized as one
// flat image: its sub-shapes compose a single glyph (masks, overlaps, boolean
// ops) that fall apart when emitted as separate nodes. Heuristic: a container
// of only vector/shape parts (no text), icon-sized, with at least one true
// vector. Boolean operations always flatten (children are operands, not art).
static bool isVectorIcon(const Node& n) {
    if (n.type == NodeType::BooleanOperation) return true;
    if (n.children.empty()) return false;
    const float maxDim = std::max(n.width, n.height);
    if (maxDim > 128.0f) return false;  // keep larger compositions structured
    if (subtreeHasText(n)) return false;
    return subtreeHasVector(n);
}

// A node whose appearance a ColorRect can't reproduce -> must be rasterized.
static bool needsBake(const Node& n) {
    if (!n.visible) return false;
    switch (n.type) {
        case NodeType::Ellipse:
        case NodeType::Vector:
        case NodeType::Line:
        case NodeType::Star:
        case NodeType::RegularPolygon:
        case NodeType::BooleanOperation:
            return true;
        default:
            break;
    }
    if (nonSolidVisibleFill(n)) return true;
    if (!n.fillGeometry.empty()) return true;
    if (hasVisibleStroke(n)) return true;
    if (hasVisibleEffect(n)) return true;
    if (cornerR(n) > 1.0f) return true;
    return false;
}

// ===================== fonts ================================================

struct FontEntry {
    std::string familyNorm;  // lowercased, spaces removed
    int weight = 400;
    bool italic = false;
    std::string file;     // basename
    std::string resPath;  // res://fonts/<file>
};

static std::string normFamily(const std::string& s) {
    std::string out;
    for (char c : s)
        if (c != ' ') out.push_back((char)std::tolower((unsigned char)c));
    return out;
}

static uint16_t be16(const std::vector<uint8_t>& b, size_t o) {
    return o + 1 < b.size() ? (uint16_t)((b[o] << 8) | b[o + 1]) : 0;
}
static uint32_t be32(const std::vector<uint8_t>& b, size_t o) {
    return o + 3 < b.size() ? (uint32_t)((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) : 0;
}

// Parse a .ttf/.otf for family (name id 1), weight (OS/2 usWeightClass) and
// italic (OS/2 fsSelection bit0). Falls back to the filename when absent.
static bool parseFontMeta(const fs::path& path, std::string& family, int& weight, bool& italic) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12) return false;
    size_t base = 0;
    if (b[0] == 't' && b[1] == 't' && b[2] == 'c' && b[3] == 'f') base = be32(b, 12);  // .ttc -> font 0
    uint16_t numTables = be16(b, base + 4);
    size_t rec = base + 12;
    size_t nameOff = 0, os2Off = 0;
    for (uint16_t i = 0; i < numTables; ++i, rec += 16) {
        if (rec + 16 > b.size()) break;
        std::string tag((const char*)&b[rec], 4);
        uint32_t off = be32(b, rec + 8);
        if (tag == "name") nameOff = off;
        else if (tag == "OS/2") os2Off = off;
    }
    weight = 400;
    italic = false;
    family.clear();
    if (os2Off) {
        weight = be16(b, os2Off + 4);
        italic = (be16(b, os2Off + 62) & 0x01) != 0;
        if (weight <= 0) weight = 400;
    }
    if (nameOff) {
        uint16_t count = be16(b, nameOff + 2);
        size_t strBase = nameOff + be16(b, nameOff + 4);
        std::string best;
        for (uint16_t i = 0; i < count; ++i) {
            size_t r = nameOff + 6 + i * 12;
            uint16_t plat = be16(b, r), nameID = be16(b, r + 6);
            uint16_t len = be16(b, r + 8), so = be16(b, r + 10);
            if (nameID != 1) continue;  // family
            std::string s;
            if (plat == 3 || plat == 0) {  // UTF-16BE
                for (uint16_t k = 1; k < len; k += 2) s.push_back((char)b[strBase + so + k]);
            } else {  // Mac ASCII
                for (uint16_t k = 0; k < len; ++k) s.push_back((char)b[strBase + so + k]);
            }
            if (plat == 3) best = s;
            else if (best.empty()) best = s;
        }
        family = best;
    }
    if (family.empty()) family = path.stem().string();
    return true;
}

// ===================== converter ============================================

struct Converter {
    figmalib::FigmaUI* ui = nullptr;
    fs::path outDir, spritesDir;

    struct GSprite {
        std::string file;
        int w = 0, h = 0;
        int uses = 0;
    };
    std::map<uint64_t, GSprite> all;  // content hash -> sprite (global dedup)
    json manifestSprites = json::object();
    json manifestFrames = json::array();

    // Bundled fonts: (familyNorm, weight, italic) -> res path; populated by main.
    std::vector<FontEntry> fonts;

    // per-frame state
    std::string body;
    struct Ext {
        std::string id, type, path;
    };
    std::map<std::string, std::string> frameExtId;  // resPath -> ExtResource id
    std::vector<Ext> frameExt;                       // declaration order
    json frameNodes;

    struct Baked {
        bool ok = false;
        uint64_t hash = 0;
        float x = 0, y = 0;  // frame-absolute top-left of painted region (logical)
        int w = 0, h = 0;    // logical size (native px / scale)
    };

    int superScale = 2;     // sprite supersampling (1x for 9-slice)
    uint32_t curW = 0, curH = 0;  // current frame logical size

    // Bake a node to a deduped PNG at the given supersample scale. By default
    // only the node's own shape is rendered (children omitted); when flatten is
    // true the whole subtree is rasterized into one image (vector-composed icons
    // and boolean operations). PNG is native (scale x); Baked.{x,y,w,h} logical.
    Baked bake(const Node& n, int scale, bool flatten = false) {
        Baked out;
        ui->setViewport(curW * scale, curH * scale);

        auto clone = figmalib::cloneNode(n, nullptr);
        if (!flatten) clone->children.clear();
        clone->opacity = 1.0f;
        clone->runtimeOpacity = -1.0f;
        clone->runtimeVisible = -1;

        // Render at the node's FRAME-ABSOLUTE position (renderOverlay uses an
        // identity parent, so the clone's transform IS the world transform). The
        // frame-sized buffer then captures exactly the on-frame portion — off-
        // frame bleed clips to the frame (matching the root's clip_contents),
        // and in-frame overhang (a nav button poking above its parent) survives
        // because it sits nowhere near the buffer's (0,0) edge. absoluteTransform
        // was computed by the main render() in convertFrame.
        clone->relativeTransform = n.absoluteTransform;

        std::vector<uint32_t> buf;
        uint32_t bw = 0, bh = 0;
        std::vector<Node*> one{clone.get()};
        if (!ui->renderer().renderOverlay(one, 0.0f, buf, bw, bh)) return out;

        // Tight bounding box of painted (alpha > 0) pixels.
        int x0 = bw, y0 = bh, x1 = -1, y1 = -1;
        for (uint32_t y = 0; y < bh; ++y) {
            for (uint32_t x = 0; x < bw; ++x) {
                if ((buf[static_cast<size_t>(y) * bw + x] >> 24) & 0xff) {
                    if ((int)x < x0) x0 = x;
                    if ((int)x > x1) x1 = x;
                    if ((int)y < y0) y0 = y;
                    if ((int)y > y1) y1 = y;
                }
            }
        }
        if (x1 < x0 || y1 < y0) return out;  // nothing painted
        int cw = x1 - x0 + 1, ch = y1 - y0 + 1;

        std::vector<uint32_t> crop(static_cast<size_t>(cw) * ch);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                crop[static_cast<size_t>(y) * cw + x] = buf[static_cast<size_t>(y0 + y) * bw + x0 + x];

        // FNV-1a 64 over the cropped pixels.
        uint64_t h = 1469598103934665603ull;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(crop.data());
        for (size_t i = 0; i < crop.size() * 4; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }

        auto it = all.find(h);
        if (it == all.end()) {
            char name[32];
            std::snprintf(name, sizeof(name), "%016llx.png", (unsigned long long)h);
            if (!writePng(spritesDir / name, crop.data(), cw, ch)) return out;
            GSprite g;
            g.file = std::string("sprites/") + name;
            g.w = cw;
            g.h = ch;
            it = all.emplace(h, g).first;
        }
        it->second.uses++;

        // Painted top-left in frame-absolute (logical) coords; contentTransform
        // is a pure `scale` with zero translate when viewport == frame*scale.
        const float sc = (float)scale;
        out.x = x0 / sc;  // absolute; callers subtract the parent's absolute pos
        out.y = y0 / sc;
        out.w = (int)std::lround(cw / sc);
        out.h = (int)std::lround(ch / sc);
        out.ok = true;
        out.hash = h;
        return out;
    }

    std::string useExt(const std::string& resPath, const char* type, const char* idPrefix) {
        auto it = frameExtId.find(resPath);
        if (it != frameExtId.end()) return it->second;
        std::string id = std::string(idPrefix) + std::to_string(frameExt.size() + 1);
        frameExtId[resPath] = id;
        frameExt.push_back({id, type, resPath});
        return id;
    }
    std::string useTexture(uint64_t hash) {
        return useExt(std::string("res://") + all[hash].file, "Texture2D", "tex_");
    }

    // Best font file for a (family, weight, italic): exact, then nearest weight
    // in the family, then any in the family.
    const FontEntry* matchFont(const std::string& family, int weight, bool italic) {
        std::string fam = normFamily(family);
        const FontEntry* best = nullptr;
        int bestScore = 1 << 30;
        for (const auto& fe : fonts) {
            if (fe.familyNorm != fam) continue;
            int score = std::abs(fe.weight - weight) + (fe.italic == italic ? 0 : 1000);
            if (score < bestScore) {
                bestScore = score;
                best = &fe;
            }
        }
        return best;
    }

    // ---- anchors / responsive placement ----
    static void axis(float pos, float size, figmalib::Constraint c, float parent,
                     float& a0, float& a1, float& o0, float& o1) {
        if (parent <= 0) {
            a0 = a1 = 0;
            o0 = pos;
            o1 = pos + size;
            return;
        }
        switch (c) {
            case figmalib::Constraint::Max:
                a0 = 1; a1 = 1; o0 = pos - parent; o1 = pos + size - parent; break;
            case figmalib::Constraint::Stretch:
                a0 = 0; a1 = 1; o0 = pos; o1 = pos + size - parent; break;
            case figmalib::Constraint::Center:
                a0 = 0.5f; a1 = 0.5f; o0 = pos - parent / 2; o1 = pos + size - parent / 2; break;
            case figmalib::Constraint::Scale:
                a0 = pos / parent; a1 = (pos + size) / parent; o0 = 0; o1 = 0; break;
            case figmalib::Constraint::Min:
            default:
                a0 = 0; a1 = 0; o0 = pos; o1 = pos + size; break;
        }
    }

    void place(float x, float y, float w, float h, figmalib::Constraint ch,
               figmalib::Constraint cv, float pw, float ph) {
        float al, ar, ol, orr, at, ab, ot, ob;
        axis(x, w, ch, pw, al, ar, ol, orr);
        axis(y, h, cv, ph, at, ab, ot, ob);
        if (al || ar || at || ab) {
            body += "anchor_left = " + num(al) + "\n";
            body += "anchor_top = " + num(at) + "\n";
            body += "anchor_right = " + num(ar) + "\n";
            body += "anchor_bottom = " + num(ab) + "\n";
        }
        body += "offset_left = " + num(ol) + "\n";
        body += "offset_top = " + num(ot) + "\n";
        body += "offset_right = " + num(orr) + "\n";
        body += "offset_bottom = " + num(ob) + "\n";
    }

    // Full-rect placement (root frame, container backgrounds).
    void placeFull() {
        body += "anchor_right = 1\nanchor_bottom = 1\n";
        body += "offset_left = 0\noffset_top = 0\noffset_right = 0\noffset_bottom = 0\n";
    }

    void commonProps(const Node& n, bool isRoot) {
        const float rot = std::atan2(n.relativeTransform.m10, n.relativeTransform.m00);
        if (std::fabs(rot) > 1e-3f) body += "rotation = " + num(rot) + "\n";
        if (!n.visible) body += "visible = false\n";
        if (n.opacity < 0.999f) body += "modulate = Color(1, 1, 1, " + num(n.opacity) + ")\n";
        // Figma frames clip their content; the top-level frame always clips to
        // the screen. (fig2json strips the default-on clip flag, so honor both
        // the explicit flag and the root.)
        if (isRoot || n.clipsContent) body += "clip_contents = true\n";
    }

    void rect(float x, float y, float w, float h) {
        body += "offset_left = " + num(x) + "\n";
        body += "offset_top = " + num(y) + "\n";
        if (w > 0) body += "offset_right = " + num(x + w) + "\n";
        if (h > 0) body += "offset_bottom = " + num(y + h) + "\n";
    }

    void header(const std::string& name, const char* type, const std::string& parentAttr) {
        body += "\n[node name=\"" + name + "\" type=\"" + type + "\"";
        if (!parentAttr.empty()) body += " parent=\"" + parentAttr + "\"";
        body += "]\n";
    }

    std::string useFont(const FontEntry* fe) { return useExt(fe->resPath, "FontFile", "font_"); }

    void textProps(const Node& n) {
        body += "text = \"" + escapeStr(n.characters) + "\"\n";
        if (const FontEntry* fe = matchFont(n.textStyle.fontFamily, n.textStyle.fontWeight,
                                            n.textStyle.italic))
            body += "theme_override_fonts/font = ExtResource(\"" + useFont(fe) + "\")\n";
        body += "theme_override_font_sizes/font_size = " +
                std::to_string((int)(n.textStyle.fontSize + 0.5f)) + "\n";
        if (const auto* f = solidFill(n))
            body += "theme_override_colors/font_color = " + colorLit(f->color) + "\n";
        int ha = 0;
        switch (n.textStyle.alignH) {
            case figmalib::TextStyle::AlignH::Center: ha = 1; break;
            case figmalib::TextStyle::AlignH::Right: ha = 2; break;
            case figmalib::TextStyle::AlignH::Justified: ha = 3; break;
            default: break;
        }
        int va = 0;
        switch (n.textStyle.alignV) {
            case figmalib::TextStyle::AlignV::Center: va = 1; break;
            case figmalib::TextStyle::AlignV::Bottom: va = 2; break;
            default: break;
        }
        if (ha) body += "horizontal_alignment = " + std::to_string(ha) + "\n";
        if (va) body += "vertical_alignment = " + std::to_string(va) + "\n";
        // Figma textAutoResize: NONE/HEIGHT keep a fixed width and may wrap.
        // Only turn on wrapping when the box is clearly tall enough for more
        // than one line — Godot DROPS a wrapped line entirely when the single
        // line is taller than the control's box (a font-18 heading in a 20px
        // box), whereas figmalib just lets it overflow. So a single-line box
        // (most headings/labels) stays autowrap-off and renders normally.
        const std::string& ar = n.textStyle.autoResize;
        const bool truncate = n.textStyle.truncateEnding || ar == "TRUNCATE";
        const bool multiline =
            !truncate && (ar == "HEIGHT" || n.height > n.textStyle.fontSize * 1.6f);
        if (multiline) body += "autowrap_mode = 3\n";        // WORD_SMART
        if (truncate) body += "text_overrun_behavior = 3\n";  // single-line ellipsis
    }

    // Emit node `n` (already named `name`, unique among siblings) under parentAttr.
    // pw/ph are the parent's logical size, for constraint -> anchor mapping.
    void emit(Node& n, const std::string& parentAttr, const std::string& name, float pw, float ph) {
        if (n.type == NodeType::Slice) return;

        const float lx = n.relativeTransform.m02, ly = n.relativeTransform.m12;
        // Parent's absolute position, to turn absolute bake coords into the
        // node's parent-relative offset for the .tscn.
        const float pax = n.parent ? n.parent->absoluteTransform.m02 : 0.0f;
        const float pay = n.parent ? n.parent->absoluteTransform.m12 : 0.0f;
        const bool isRoot = parentAttr.empty();
        const bool isContainer = !n.children.empty();
        const std::string childAttr =
            isRoot ? "." : (parentAttr == "." ? name : parentAttr + "/" + name);

        auto placeNode = [&](float x, float y, float w, float h) {
            // Root gets an explicit design-size rect so the scene opens at the
            // right size in the editor; children anchor within it. Project
            // stretch (canvas_items) scales the whole UI to the window.
            if (isRoot) rect(0, 0, n.width, n.height);
            else place(x, y, w, h, n.constraintH, n.constraintV, pw, ph);
        };

        json nodeJson;
        nodeJson["name"] = name;
        nodeJson["path"] = childAttr;

        if (n.type == NodeType::Text) {
            header(name, "Label", parentAttr);
            placeNode(lx, ly, n.width, n.height);
            commonProps(n, isRoot);
            textProps(n);
            nodeJson["type"] = "Label";
        } else if (!isRoot && isVectorIcon(n)) {
            // Flatten a vector-composed icon (or boolean op) to one image.
            Baked b = bake(n, superScale, /*flatten=*/true);
            if (b.ok) {
                std::string id = useTexture(b.hash);
                header(name, "TextureRect", parentAttr);
                placeNode(b.x - pax, b.y - pay, b.w, b.h);
                commonProps(n, isRoot);
                body += "texture = ExtResource(\"" + id + "\")\n";
                body += "expand_mode = 1\nstretch_mode = 0\n";
                nodeJson["type"] = "TextureRect";
                nodeJson["sprite"] = all[b.hash].file;
                nodeJson["flattened"] = true;
            } else {
                header(name, "Control", parentAttr);
                placeNode(lx, ly, n.width, n.height);
                commonProps(n, isRoot);
                nodeJson["type"] = "Control";
            }
            frameNodes.push_back(nodeJson);
            return;  // do not recurse into the flattened subtree
        } else if (isContainer) {
            header(name, "Control", parentAttr);
            placeNode(lx, ly, n.width, n.height);
            commonProps(n, isRoot);
            nodeJson["type"] = "Control";
            if (needsBake(n)) emitBg(n, childAttr, nodeJson);
            else if (const auto* f = solidFill(n)) {
                nodeJson["solidBg"] = colorLit(f->color);
                emitColorBg(f, childAttr);
            }
        } else if (needsBake(n)) {
            bool wantNine = ninePatchWanted(n);
            Baked b = bake(n, wantNine ? 1 : superScale);
            if (b.ok) {
                bool nine = wantNine && ninePatchFits(n, b);
                std::string id = useTexture(b.hash);
                if (nine) {
                    header(name, "NinePatchRect", parentAttr);
                    placeNode(lx, ly, n.width, n.height);
                    commonProps(n, isRoot);
                    body += "texture = ExtResource(\"" + id + "\")\n";
                    int r = (int)std::lround(cornerR(n));
                    r = std::min(r, std::min(b.w, b.h) / 2 - 1);
                    if (r < 0) r = 0;
                    body += "patch_margin_left = " + std::to_string(r) + "\n";
                    body += "patch_margin_top = " + std::to_string(r) + "\n";
                    body += "patch_margin_right = " + std::to_string(r) + "\n";
                    body += "patch_margin_bottom = " + std::to_string(r) + "\n";
                    nodeJson["type"] = "NinePatchRect";
                    nodeJson["ninePatch"] = {r, r, r, r};
                } else {
                    header(name, "TextureRect", parentAttr);
                    placeNode(b.x - pax, b.y - pay, b.w, b.h);
                    commonProps(n, isRoot);
                    body += "texture = ExtResource(\"" + id + "\")\n";
                    body += "expand_mode = 1\nstretch_mode = 0\n";  // scale texture to rect
                    nodeJson["type"] = "TextureRect";
                }
                nodeJson["sprite"] = all[b.hash].file;
            } else {
                header(name, "Control", parentAttr);
                placeNode(lx, ly, n.width, n.height);
                commonProps(n, isRoot);
                nodeJson["type"] = "Control";
            }
        } else if (const auto* f = solidFill(n)) {
            header(name, "ColorRect", parentAttr);
            placeNode(lx, ly, n.width, n.height);
            commonProps(n, isRoot);
            body += "color = " + colorLit(f->color) + "\n";
            nodeJson["type"] = "ColorRect";
        } else {
            header(name, "Control", parentAttr);
            placeNode(lx, ly, n.width, n.height);
            commonProps(n, isRoot);
            nodeJson["type"] = "Control";
        }

        frameNodes.push_back(nodeJson);

        // Children anchor within this node's rect; names unique among siblings.
        std::map<std::string, int> used;
        used["__bg"] = 1;
        int idx = 0;
        for (auto& child : n.children) {
            std::string fb = "Node" + std::to_string(idx++);
            std::string base = sanitizeName(child->name, fb.c_str());
            std::string uniq = base;
            int k = 2;
            while (used.count(uniq)) uniq = base + "_" + std::to_string(k++);
            used[uniq] = 1;
            emit(*child, childAttr, uniq, n.width, n.height);
        }
    }

    // Plain solid background for a container: a full-rect ColorRect child.
    void emitColorBg(const figmalib::Paint* f, const std::string& parentAttr) {
        if (!f) return;
        header("__bg", "ColorRect", parentAttr);
        placeFull();
        body += "color = " + colorLit(f->color) + "\n";
    }

    // Baked background for a container with a complex fill/stroke/corners.
    void emitBg(Node& n, const std::string& parentAttr, json& nodeJson) {
        bool wantNine = ninePatchWanted(n);
        Baked b = bake(n, wantNine ? 1 : superScale);
        if (!b.ok) return;
        std::string id = useTexture(b.hash);
        bool nine = wantNine && ninePatchFits(n, b);
        if (nine) {
            header("__bg", "NinePatchRect", parentAttr);
            placeFull();  // stretch to the container's full rect
            body += "texture = ExtResource(\"" + id + "\")\n";
            int r = (int)std::lround(cornerR(n));
            r = std::min(r, std::min(b.w, b.h) / 2 - 1);
            if (r < 0) r = 0;
            body += "patch_margin_left = " + std::to_string(r) + "\n";
            body += "patch_margin_top = " + std::to_string(r) + "\n";
            body += "patch_margin_right = " + std::to_string(r) + "\n";
            body += "patch_margin_bottom = " + std::to_string(r) + "\n";
            nodeJson["ninePatch"] = {r, r, r, r};
        } else {
            header("__bg", "TextureRect", parentAttr);
            // Baked bg is in absolute coords; place it relative to the container.
            float ox = b.x - n.absoluteTransform.m02;
            float oy = b.y - n.absoluteTransform.m12;
            rect(ox, oy, b.w, b.h);
            body += "texture = ExtResource(\"" + id + "\")\n";
            body += "expand_mode = 1\nstretch_mode = 0\n";
        }
        nodeJson["bg"] = all[b.hash].file;
    }

    // Rounded rectangular panels become stretchable 9-slices (baked at 1x so the
    // corner pixels map 1:1 to screen — supersampling would inflate them).
    bool ninePatchWanted(const Node& n) {
        if (!isRectish(n.type)) return false;
        if (hasVisibleEffect(n)) return false;
        if (cornerR(n) <= 2) return false;
        for (const auto& p : n.fills)
            if (p.visible && p.type == figmalib::PaintType::Image) return false;
        return true;
    }
    bool ninePatchFits(const Node& n, const Baked& b) {
        float r = cornerR(n);
        return b.w >= 2 * r + 2 && b.h >= 2 * r + 2;
    }

    void convertFrame(Node* frame) {
        curW = (uint32_t)std::ceil(std::max(1.0f, frame->width));
        curH = (uint32_t)std::ceil(std::max(1.0f, frame->height));
        ui->setViewport(curW, curH);
        if (!ui->selectFrame(frame->name)) {
            std::fprintf(stderr, "  WARN: selectFrame(%s) failed\n", frame->name.c_str());
            return;
        }
        ui->render();  // computes absoluteTransform + content transform

        body.clear();
        frameExtId.clear();
        frameExt.clear();
        frameNodes = json::array();

        std::string rootName = sanitizeName(frame->name, "Frame");
        emit(*frame, "", rootName, (float)curW, (float)curH);

        std::string head = "[gd_scene load_steps=" +
                           std::to_string(frameExt.size() + 1) + " format=3]\n";
        for (auto& e : frameExt)
            head += "\n[ext_resource type=\"" + e.type + "\" path=\"" + e.path +
                    "\" id=\"" + e.id + "\"]\n";

        fs::path scenePath = outDir / (rootName + ".tscn");
        std::ofstream f(scenePath, std::ios::binary);
        f << head << body;
        std::printf("  %s -> %s (%zu ext resources)\n", frame->name.c_str(),
                    scenePath.string().c_str(), frameExt.size());

        json fj;
        fj["name"] = frame->name;
        fj["scene"] = rootName + ".tscn";
        fj["size"] = {curW, curH};
        fj["nodes"] = frameNodes;
        manifestFrames.push_back(fj);
    }

    void writeManifest() {
        for (auto& kv : all) {
            char hex[32];
            std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)kv.first);
            manifestSprites[hex] = {{"file", kv.second.file},
                                    {"width", kv.second.w},
                                    {"height", kv.second.h},
                                    {"uses", kv.second.uses}};
        }
        json m;
        m["frames"] = manifestFrames;
        m["sprites"] = manifestSprites;
        std::ofstream f(outDir / "manifest.json", std::ios::binary);
        f << m.dump(2);
    }
};

static void writeProjectGodot(const fs::path& outDir, uint32_t baseW, uint32_t baseH) {
    fs::path p = outDir / "project.godot";
    if (fs::exists(p)) return;
    std::ofstream f(p, std::ios::binary);
    f << "config_version=5\n\n[application]\n\n"
         "config/name=\"fapp2godot export\"\n"
         "config/features=PackedStringArray(\"4.6\", \"Forward Plus\")\n\n"
         "[display]\n\n"
         "window/size/viewport_width=" << baseW << "\n"
         "window/size/viewport_height=" << baseH << "\n"
         // Uniformly scale the design-sized scenes to the window (letterbox on
         // aspect mismatch) — faithful layout at any resolution.
         "window/stretch/mode=\"canvas_items\"\n"
         "window/stretch/aspect=\"keep\"\n";
}

// Index a directory of .ttf/.otf into FontEntry list, copying each into the
// project's fonts/ dir so res:// can reference it.
static std::vector<FontEntry> loadFonts(const fs::path& fontsSrc, const fs::path& outFonts) {
    std::vector<FontEntry> out;
    if (fontsSrc.empty() || !fs::exists(fontsSrc)) return out;
    std::error_code ec;
    fs::create_directories(outFonts, ec);
    for (const auto& de : fs::directory_iterator(fontsSrc, ec)) {
        if (!de.is_regular_file()) continue;
        std::string ext = de.path().extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".ttf" && ext != ".otf") continue;
        std::string family;
        int weight;
        bool italic;
        if (!parseFontMeta(de.path(), family, weight, italic)) continue;
        std::string base = de.path().filename().string();
        fs::copy_file(de.path(), outFonts / base, fs::copy_options::overwrite_existing, ec);
        out.push_back({normFamily(family), weight, italic, base, "res://fonts/" + base});
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: fapp2godot <input> [outDir] [--frame NAME] [--fonts DIR] [--scale N]\n");
        return 2;
    }
    const std::string input = argv[1];
    fs::path outDir = "godot_out";
    std::string onlyFrame;
    fs::path fontsDir;
    int scale = 2;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frame" && i + 1 < argc)
            onlyFrame = argv[++i];
        else if (a == "--fonts" && i + 1 < argc)
            fontsDir = argv[++i];
        else if (a == "--scale" && i + 1 < argc)
            scale = std::max(1, atoi(argv[++i]));
        else if (!a.empty() && a[0] != '-')
            outDir = a;
    }
    // Default: a "fonts" directory next to the input file.
    if (fontsDir.empty()) {
        fs::path sib = fs::path(input).parent_path() / "fonts";
        if (fs::exists(sib)) fontsDir = sib;
    }

    std::printf("loading %s\n", input.c_str());
    std::unique_ptr<figmalib::FigmaUI> ui;
    try {
        ui = figmalib::FigmaUI::fromFile(input);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "FAIL: load: %s\n", ex.what());
        return 1;
    }

    std::error_code ec;
    fs::create_directories(outDir, ec);
    fs::path spritesDir = outDir / "sprites";
    fs::create_directories(spritesDir, ec);

    auto frames = ui->document().topLevelFrames();
    if (frames.empty()) {
        std::fprintf(stderr, "FAIL: no top-level frames\n");
        return 1;
    }

    Converter cv;
    cv.ui = ui.get();
    cv.outDir = outDir;
    cv.spritesDir = spritesDir;
    cv.superScale = scale;
    cv.fonts = loadFonts(fontsDir, outDir / "fonts");
    std::printf("fonts: %zu file(s) from %s\n", cv.fonts.size(),
                fontsDir.empty() ? "(none)" : fontsDir.string().c_str());

    int n = 0;
    uint32_t baseW = 0, baseH = 0;
    for (Node* frame : frames) {
        if (!onlyFrame.empty() && frame->name != onlyFrame) continue;
        cv.convertFrame(frame);
        if (!baseW) {
            baseW = (uint32_t)std::ceil(std::max(1.0f, frame->width));
            baseH = (uint32_t)std::ceil(std::max(1.0f, frame->height));
        }
        ++n;
    }
    cv.writeManifest();
    writeProjectGodot(outDir, baseW ? baseW : 390, baseH ? baseH : 844);

    std::printf("RESULT: OK, %d scene(s), %zu unique sprite(s)\n", n, cv.all.size());
    return 0;
}
