// figo2godot — convert a figo design (.fig / canvas.json / REST JSON) into
// a Godot 4 project: one .tscn per top-level frame + deduplicated PNG sprites +
// a manifest.json. The design's own ThorVG rasterizer bakes the sprites, so the
// textures are pixel-identical to the figo runtime.
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
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <figo/figo.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using figo::Node;
using figo::NodeType;
using figo::NodeAnim;
using figo::AnimKey;
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

static std::string colorLit(const figo::Color& c) {
    return "Color(" + num(c.r) + ", " + num(c.g) + ", " + num(c.b) + ", " + num(c.a) + ")";
}
// A paint's effective color folds its separate `opacity` into the alpha — a
// translucent fill (e.g. inkSoft text at 0.55) must keep that dimming, not 1.0.
static std::string paintLit(const figo::Paint& p) {
    figo::Color c = p.color;
    c.a *= p.opacity;
    return colorLit(c);
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

static const figo::Paint* solidFill(const Node& n) {
    for (const auto& p : n.fills)
        if (p.visible && p.type == figo::PaintType::Solid) return &p;
    return nullptr;
}
// 8-bit-quantized color inequality (matches how colors are emitted/grouped).
static bool colorNe(const figo::Color& a, const figo::Color& b) {
    return std::lround(a.r * 255) != std::lround(b.r * 255) ||
           std::lround(a.g * 255) != std::lround(b.g * 255) ||
           std::lround(a.b * 255) != std::lround(b.b * 255) ||
           std::lround(a.a * 255) != std::lround(b.a * 255);
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
        if (p.visible && p.type != figo::PaintType::Solid) return true;
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

// Count Unicode codepoints in a UTF-8 string (lead bytes != 10xxxxxx).
static size_t utf8Count(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
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
    figo::FigmaUI* ui = nullptr;
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

    // ---- prefab extraction (--prefabs) ----
    bool prefabs = false;
    bool prefabAnon = false;   // --prefab-anon: also extract repeated ANONYMOUS
                               // (no-compType) containers, not just source comps
    bool inComponent = false;  // true while emitting a component scene (no nesting-instances)
    std::set<std::string> noPrefab;  // compTypes never extracted (generic wrappers: HPanel…)
    std::set<std::string> usedComps; // comp names actually instanced by a frame — only
                                     // these get a .tscn (a comp used only NESTED inside
                                     // another comp is inlined there, never instanced)
    struct Comp {
        std::string name;            // unique component name / file stem
        const Node* canon = nullptr; // canonical (superset) instance
        Node* frame = nullptr;       // canon's top-level frame (for render context)
        int count = 0;
        bool extracted = false;
        std::vector<std::pair<const Node*, Node*>> insts;  // (instance node, its frame)
    };
    std::map<std::string, Comp> compBySig;  // groupKey (compType+size) -> all its instances
    // Each group is partitioned into STRUCTURAL CLUSTERS: a component with
    // incompatible variants (a slider vs a segmented SettingRow; a 2-state
    // VoteCard) yields one prefab PER cluster, not one prefab plus inlined
    // leftovers. variants owns the clusters (stable addresses); nodeComp maps an
    // instance node to the cluster it was assigned to.
    std::vector<std::unique_ptr<Comp>> variants;
    std::map<const Node*, Comp*> nodeComp;

    // Structural signature: groups visually-identical components regardless of
    // text content / text box geometry (those become per-instance overrides).
    // Includes node types, sizes, fills (color/image ref), corners, strokes,
    // effects, and non-text children's relative positions.
    // struct_ = a STRUCTURAL signature: also ignore the specific image ref (a
    // per-instance avatar/thumb) so same-component instances differing only in
    // their sprite still group — the differing sprite becomes an instance
    // override. Plain sig() keeps the ref (pixel-identical grouping).
    // maxDepth caps recursion: at depth 0 a child is emitted by its bare type
    // (+compType) only, not its subtree. slotKey() uses a SHALLOW sig so a slot
    // matches across deep state changes (a vote card whose voter list / badge
    // differs a couple nodes down) while still distinguishing structurally
    // different immediate shapes (a plain vs a selected segment). groupKey() keeps
    // the full-depth sig (maxDepth<0) for prefab grouping.
    std::string sig(const Node& n, bool struct_ = false, int maxDepth = -1) const {
        std::string s = std::to_string((int)n.type);
        if (n.type == NodeType::Text) {
            // struct_ ignores font size too (a heading vs label of the same
            // component slot still aligns; size becomes a per-instance override).
            s += struct_ ? "T" : ("T" + std::to_string((int)(n.textStyle.fontSize + 0.5f)) + n.textStyle.fontFamily);
        } else {
            // struct_ = pure shape: drop solid-color VALUES and image refs (those
            // become per-instance overrides) so instances differing only in
            // sprite/text collapse to ONE prefab. Size is dropped for plain/
            // transparent boxes (a text-width-driven reflow is reproduced by the
            // child offset overrides), but KEPT (coarsely) for a RASTER node: its
            // sprite IS its size, and a different-size raster (a 3-seg "玩家人数"
            // 6/8/10 track vs a wider "画质" 流畅/均衡/极致 track in the same
            // SettingRow slot) can't be stretched into the canon's box without
            // overflow — such instances must stay separate variants.
            bool hasImg = false;
            for (const auto& p : n.fills) if (p.visible && p.type == figo::PaintType::Image) { hasImg = true; break; }
            if (!struct_) s += "#" + std::to_string((int)std::lround(n.width)) + "x" +
                               std::to_string((int)std::lround(n.height));
            else if (hasImg) s += "#" + std::to_string((int)std::lround(n.width) / 16) + "x" +
                                  std::to_string((int)std::lround(n.height) / 16);
            for (const auto& p : n.fills) {
                if (!p.visible) continue;
                if (p.type == figo::PaintType::Solid) {
                    if (struct_) s += "s";
                    else { char c[10]; std::snprintf(c, sizeof(c), "s%02x%02x%02x",
                        (int)(p.color.r * 255), (int)(p.color.g * 255), (int)(p.color.b * 255)); s += c; }
                } else if (p.type == figo::PaintType::Image) s += struct_ ? "i" : "i" + p.imageRef;
                else s += "g";
            }
            if (!n.strokes.empty()) s += "k";
            if (cornerR(n) > 0) s += "r";
            if (hasVisibleEffect(n)) s += "e";
        }
        s += "{";
        for (const auto& c : n.children) {
            if (!struct_ && c->type != NodeType::Text)
                s += "@" + std::to_string((int)std::lround(c->relativeTransform.m02)) + "," +
                     std::to_string((int)std::lround(c->relativeTransform.m12));
            if (maxDepth == 0)
                s += std::to_string((int)c->type) + (c->compType.empty() ? "" : "c" + c->compType) + ";";
            else
                s += sig(*c, struct_, maxDepth < 0 ? -1 : maxDepth - 1) + ";";
        }
        s += "}";
        return s;
    }

    // Grouping key for prefab extraction. A source component (web2canvas / hand-
    // authored compType) groups by TYPE + a COARSE outer-size bucket ONLY — the
    // structural sig is intentionally NOT in the key. Same compType = same React
    // component, so ALL of its state variants (an online vs offline vs eliminated
    // player row differing only by an optional dot/badge child) collapse to ONE
    // prefab: the canon is the superset instance and a leaner variant hides the
    // children it lacks (see emitInstanceOverrides). The size bucket still splits
    // a component instanced at a very different OUTER size (MenuChatBar 660 vs
    // 840), whose non-uniform flex reflow per-child offset overrides can't
    // reproduce. Anonymous (no-compType) containers keep a pixel/structural key;
    // phase 1 never extracts them, but the key stays stable.
    std::string groupKey(const Node& n) const {
        const int wb = (int)std::lround(n.width) / 64, hb = (int)std::lround(n.height) / 64;
        if (!n.compType.empty())
            return n.compType + "|" + std::to_string(wb) + "x" + std::to_string(hb);
        return "|" + std::to_string(wb) + "x" + std::to_string(hb) + "|" + sig(n, true);
    }
    // Slot identity for matching a child across a component's state variants:
    // the React role (compType) when present, then the PURE structural shape
    // (sig with struct_=true ignores text/color/image/size). The same slot keeps
    // its shape across states, so it matches; an optional badge present in one
    // variant and absent in another has a unique shape, so it matches only its
    // counterpart and otherwise hides. Sibling look-alikes (two dots) collide on
    // key but are consumed in order by the matcher below.
    // A SHALLOW slot identity: own shape + IMMEDIATE children's bare types (depth
    // 0), nothing deeper. So the same slot matches across deep state changes (a
    // vote card whose voter list / badge differs a few nodes down) — those become
    // recursive hide/add/override — while structurally different controls stay
    // distinct: a SettingRow's slider wrapper {Control,Label} ≠ its segmented
    // wrapper {Control,Control,Control}, so a segmented row hides the slider and
    // adds the segments instead of mis-pairing slider textures onto segment
    // labels. (Going fully type-only over-merges those; full-depth under-merges
    // state variants.) matchInst's position tiebreak separates same-key siblings.
    std::string slotKey(const Node& n) const {
        return (n.compType.empty() ? "" : "c:" + n.compType + "|") + sig(n, true, 0);
    }
    // Match-quality gate: how well a state variant aligns with the prefab canon.
    // Counts canon children that find a same-slotKey instance child (the same
    // greedy matching emitInstanceOverrides uses). A variant is a POOR FIT when
    // it would reuse less than ~60% of the canon's top-level children OR less
    // than ~60% of its own — i.e. most of the prefab gets hidden and most of the
    // instance gets newly added (a segmented control whose option set diverges
    // from the canon's). Such an instance is better emitted inline than forced
    // onto the prefab, where mismatched children stack and overlap. The canon
    // matches itself fully, so it is never a poor fit.
    bool poorFit(const Node& canon, const Node& inst) const {
        const Node* c = &canon;
        const Node* i = &inst;
        // Descend through 1:1 wrapper layers first: a variant that differs only a
        // few nodes below an outer wrapper should be judged on its real content,
        // not on a wrapper whose shallow shape happens to differ (which would make
        // the binary 1-child ratio read 0%).
        while (c->children.size() == 1 && i->children.size() == 1) {
            c = c->children[0].get();
            i = i->children[0].get();
        }
        const size_t cc = c->children.size(), ic = i->children.size();
        if (cc == 0) return false;
        std::vector<char> usedI(ic, 0);
        size_t matched = 0;
        for (const auto& cch : c->children) {
            const std::string key = slotKey(*cch);
            for (size_t j = 0; j < ic; ++j)
                if (!usedI[j] && slotKey(*i->children[j]) == key) { usedI[j] = 1; ++matched; break; }
        }
        return matched * 5 < cc * 3 || (ic > 0 && matched * 5 < ic * 3);
    }
    // First visible image-fill ref of a node (empty if none).
    static std::string imageRefOf(const Node& n) {
        for (const auto& p : n.fills)
            if (p.visible && p.type == figo::PaintType::Image) return p.imageRef;
        return "";
    }

    // Count descendants (cheap gate against extracting trivial sub-groups).
    static int descendants(const Node& n) {
        int c = 0;
        for (const auto& k : n.children) c += 1 + descendants(*k);
        return c;
    }

    // A generic/auto-generated container name (div_12, span3, css-1abc, node5,
    // wrapper, "") carries no semantic meaning — not worth using as a prefab name.
    static bool genericName(const std::string& s) {
        if (s.empty()) return true;
        std::string l;
        for (char c : s) l.push_back((char)std::tolower((unsigned char)c));
        static const char* pre[] = {"div", "span", "css", "node", "wrapper", "container", "box", "group", "item"};
        for (const char* p : pre) {
            size_t n = std::strlen(p);
            if (l.rfind(p, 0) != 0) continue;
            bool g = true;  // prefix followed only by digits / - / _ -> generic
            for (size_t i = n; i < l.size(); ++i) {
                char c = l[i];
                if (!(std::isdigit((unsigned char)c) || c == '-' || c == '_')) { g = false; break; }
            }
            if (g) return true;
        }
        return false;
    }
    // First non-empty text in a subtree (for naming an anonymous container).
    static std::string firstText(const Node& n) {
        if (n.type == NodeType::Text && !n.characters.empty()) return n.characters;
        for (const auto& c : n.children) {
            std::string s = firstText(*c);
            if (!s.empty()) return s;
        }
        return "";
    }
    // A readable prefab name for an anonymous repeated container: its own name if
    // meaningful, else a leading ASCII word of the first text in its subtree (CJK
    // sanitizes to underscores downstream, so it falls through), else "Group".
    static std::string genName(const Node& n) {
        if (!genericName(n.name)) return n.name;
        std::string out;
        for (char c : firstText(n)) {
            if (std::isalnum((unsigned char)c)) out.push_back(c);
            else if (!out.empty()) break;  // stop at the first break after a word
            if (out.size() >= 16) break;
        }
        return out.empty() ? "Group" : out;
    }

    // Tally repeated component candidates across a frame's tree. Only containers
    // with real substance (>=3 descendants) so we extract meaningful components
    // (cards, buttons), not every 2-label sub-group.
    void scanComponents(Node* frame) {
        std::function<void(Node&)> rec = [&](Node& n) {
            for (auto& c : n.children) {
                Node& cn = *c;
                // SOURCE components (web2canvas compType from the React fiber)
                // always become prefab candidates. With --prefab-anon, ANONYMOUS
                // structural containers join too — grouped by their structural sig
                // (groupKey), so any container repeated >1 deduplicates into a
                // prefab (problem: "重复显示次数大于1的就应设为预制体"). desc>=3
                // gates trivia; the poorFit guard later inlines bad merges, so a
                // coincidental same-shape match never breaks a frame visually.
                const bool isComp = !cn.compType.empty();
                const bool anon = prefabAnon && cn.compType.empty();
                if ((isComp || anon) && !cn.children.empty() && cn.type != NodeType::Text &&
                    !isVectorIcon(cn) && cn.width > 4 && cn.height > 4 && descendants(cn) >= 3) {
                    auto& comp = compBySig[groupKey(cn)];
                    comp.count++;
                    comp.insts.push_back({ &cn, frame });
                    if (comp.name.empty()) comp.name = isComp ? cn.compType : genName(cn);
                }
                rec(cn);
            }
        };
        rec(*frame);
    }

    const Comp* extractedComponent(const Node& n) {
        if (!prefabs || inComponent || n.children.empty() || n.type == NodeType::Text) return nullptr;
        // Clustering (in main) already assigned this node to the prefab it fits.
        auto it = nodeComp.find(&n);
        return (it != nodeComp.end() && it->second->extracted) ? it->second : nullptr;
    }

    // per-frame state
    std::string body;
    struct Ext {
        std::string id, type, path;
    };
    std::map<std::string, std::string> frameExtId;  // resPath -> ExtResource id
    std::vector<Ext> frameExt;                       // declaration order
    std::vector<std::string> frameSub;               // [sub_resource ...] blocks (animations)
    int subId = 0;                                   // unique sub_resource id counter, per file
    json frameNodes;

    struct Baked {
        bool ok = false;
        uint64_t hash = 0;
        float x = 0, y = 0;  // frame-absolute top-left of painted region (logical)
        int w = 0, h = 0;    // logical size (native px / scale)
        bool nine = false;   // texture was shrunk to a 9-slice
        int ml = 0, mr = 0, mt = 0, mb = 0;  // 9-slice patch margins (logical px)
    };

    // Collapse a 9-sliceable image: find the longest run of identical adjacent
    // columns (and rows) — the stretchable middle — and shrink it to 1px, leaving
    // the corners/edges intact. Sets the patch margins and rewrites px/w/h in
    // place. Returns false when there's no worthwhile run (keep the full bake).
    // This catches clip-path cut-corner panels (uniform fill, only the corners
    // differ) that carry no Figma cornerRadius, so many same-style different-size
    // panels collapse to ONE tiny shared texture.
    static bool nineShrink(std::vector<uint32_t>& px, int& w, int& h, int scale,
                           int& ml, int& mr, int& mt, int& mb) {
        if (w < 2 * scale + 1 && h < 2 * scale + 1) return false;
        auto colEq = [&](int a, int b) {
            for (int y = 0; y < h; ++y) if (px[(size_t)y * w + a] != px[(size_t)y * w + b]) return false;
            return true;
        };
        auto rowEq = [&](int a, int b) {
            for (int x = 0; x < w; ++x) if (px[(size_t)a * w + x] != px[(size_t)b * w + x]) return false;
            return true;
        };
        // longest run of consecutive equal columns -> [cs, ce] (native/2x px)
        int cs = 0, ce = 0, s = 0;
        for (int x = 1; x < w; ++x) {
            if (colEq(x, x - 1)) { if (x - s > ce - cs) { cs = s; ce = x; } }
            else s = x;
        }
        int rs = 0, re = 0; s = 0;
        for (int y = 1; y < h; ++y) {
            if (rowEq(y, y - 1)) { if (y - s > re - rs) { rs = s; re = y; } }
            else s = y;
        }
        // Collapse the stretchable middle only if it saves at least one supersample
        // block per axis (so the texture genuinely shrinks after downsampling).
        const bool cw9 = ce - cs >= scale, ch9 = re - rs >= scale;
        if (!cw9 && !ch9) return false;
        // 2x margins -> logical (snap the corner extents down to whole blocks).
        ml = cs / scale; mr = (w - 1 - ce) / scale; mt = rs / scale; mb = (h - 1 - re) / scale;
        const int nw = ml + 1 + mr, nh = mt + 1 + mb;
        // Downsample (box-average each scale×scale block); the 1px middle samples
        // the run's first native row/col (the run is uniform, so any is equal).
        std::vector<uint32_t> out((size_t)nw * nh);
        auto srcX = [&](int ox) { return ox < ml ? ox * scale : (ox == ml ? cs : ce + 1 + (ox - ml - 1) * scale); };
        auto srcY = [&](int oy) { return oy < mt ? oy * scale : (oy == mt ? rs : re + 1 + (oy - mt - 1) * scale); };
        const int blk = scale;
        for (int oy = 0; oy < nh; ++oy) {
            const int sy = srcY(oy), bh = (oy == mt) ? 1 : blk;
            for (int ox = 0; ox < nw; ++ox) {
                const int sx = srcX(ox), bw2 = (ox == ml) ? 1 : blk;
                uint32_t r = 0, g = 0, b = 0, a = 0; int cnt = 0;
                for (int yy = 0; yy < bh && sy + yy < h; ++yy)
                    for (int xx = 0; xx < bw2 && sx + xx < w; ++xx) {
                        uint32_t p = px[(size_t)(sy + yy) * w + sx + xx];
                        r += p & 0xff; g += (p >> 8) & 0xff; b += (p >> 16) & 0xff; a += (p >> 24) & 0xff; ++cnt;
                    }
                if (!cnt) cnt = 1;
                out[(size_t)oy * nw + ox] = (r / cnt) | ((g / cnt) << 8) | ((b / cnt) << 16) | ((a / cnt) << 24);
            }
        }
        px.swap(out); w = nw; h = nh;
        return true;
    }

    int superScale = 2;     // sprite supersampling (1x for 9-slice)
    uint32_t curW = 0, curH = 0;  // current frame logical size

    // Bake a node to a deduped PNG at the given supersample scale. By default
    // only the node's own shape is rendered (children omitted); when flatten is
    // true the whole subtree is rasterized into one image (vector-composed icons
    // and boolean operations). PNG is native (scale x); Baked.{x,y,w,h} logical.
    Baked bake(const Node& n, int scale, bool flatten = false, bool tryNine = false,
               bool nineOnly = false) {
        Baked out;
        ui->setViewport(curW * scale, curH * scale);

        auto clone = figo::cloneNode(n, nullptr);
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

        // 9-slice shrink (baked at 1x so corners map 1:1 to screen): collapse a
        // uniform stretchable middle to 1px and dedup by the resulting tiny image,
        // so same-style different-size panels share one texture.
        if (tryNine && nineShrink(crop, cw, ch, scale, out.ml, out.mr, out.mt, out.mb)) out.nine = true;
        else if (nineOnly) return out;  // not 9-sliceable at this scale; caller re-bakes full

        // FNV-1a 64 over the cropped pixels.
        uint64_t h = 1469598103934665603ull;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(crop.data());
        for (size_t i = 0; i < crop.size() * 4; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }

        auto it = all.find(h);
        if (it == all.end()) {
            // Readable, content-addressed filename: <node-name>_<hash8>.png. The
            // hash keeps dedup correct; the name makes the resource identifiable.
            std::string stem;
            for (char c : sanitizeName(n.name, "sprite"))
                stem.push_back((std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_');
            if (stem.size() > 40) stem.resize(40);
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "_%08x.png", (unsigned)(h & 0xffffffffu));
            std::string name = stem + suffix;
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
    // Nearest-weight entry within one normalized family (null if absent).
    const FontEntry* bestInFamily(const std::string& fam, int weight, bool italic) {
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

    const FontEntry* matchFont(const std::string& family, int weight, bool italic) {
        if (const FontEntry* fe = bestInFamily(normFamily(family), weight, italic))
            return fe;
        // No bundled file for this family (Arial/Helvetica/system-ui/generic
        // sans…). Without an override Godot falls back to its default font plus
        // OS glyph substitution, whose metrics & coverage are unpredictable — a
        // "☺" becomes a system emoji, CJK shows tofu, and a glyph-tight Label
        // box no longer centers. Route to a bundled Unicode-broad sans (Noto
        // Sans covers Latin + CJK + common symbols) so glyphs stay consistent.
        // Prefix-match because a variable font's default instance reports a
        // named family like "Noto Sans SC Thin" (norm "notosansscthin").
        const FontEntry* fb = nullptr;
        int fbScore = 1 << 30;
        for (const auto& fe : fonts) {
            if (fe.familyNorm.rfind("notosans", 0) != 0) continue;
            int score = std::abs(fe.weight - weight) + (fe.italic == italic ? 0 : 1000);
            if (score < fbScore) { fbScore = score; fb = &fe; }
        }
        return fb;
    }

    // ---- anchors / responsive placement ----
    static void axis(float pos, float size, figo::Constraint c, float parent,
                     float& a0, float& a1, float& o0, float& o1) {
        if (parent <= 0) {
            a0 = a1 = 0;
            o0 = pos;
            o1 = pos + size;
            return;
        }
        switch (c) {
            case figo::Constraint::Max:
                a0 = 1; a1 = 1; o0 = pos - parent; o1 = pos + size - parent; break;
            case figo::Constraint::Stretch:
                a0 = 0; a1 = 1; o0 = pos; o1 = pos + size - parent; break;
            case figo::Constraint::Center:
                a0 = 0.5f; a1 = 0.5f; o0 = pos - parent / 2; o1 = pos + size - parent / 2; break;
            case figo::Constraint::Scale:
                a0 = pos / parent; a1 = (pos + size) / parent; o0 = 0; o1 = 0; break;
            case figo::Constraint::Min:
            default:
                a0 = 0; a1 = 0; o0 = pos; o1 = pos + size; break;
        }
    }

    void place(float x, float y, float w, float h, figo::Constraint ch,
               figo::Constraint cv, float pw, float ph) {
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
        // Figma frames clip their content; the top-level SCREEN frame always
        // clips to the screen. (fig2json strips the default-on clip flag, so
        // honor both the explicit flag and the screen root.) A component scene
        // root is NOT a screen — force-clipping it would shave off children that
        // intentionally overhang it (e.g. a nav center button), so only clip a
        // component root when its own clipsContent says so.
        if ((isRoot && !inComponent) || n.clipsContent) body += "clip_contents = true\n";
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
            body += "theme_override_colors/font_color = " + paintLit(*f) + "\n";
        int ha = 0;
        switch (n.textStyle.alignH) {
            case figo::TextStyle::AlignH::Center: ha = 1; break;
            case figo::TextStyle::AlignH::Right: ha = 2; break;
            case figo::TextStyle::AlignH::Justified: ha = 3; break;
            default: break;
        }
        if (ha) body += "horizontal_alignment = " + std::to_string(ha) + "\n";
        // Figma textAutoResize: NONE/HEIGHT keep a fixed width and may wrap.
        // Only turn on wrapping when the box is clearly tall enough for more
        // than one line — Godot DROPS a wrapped line entirely when the single
        // line is taller than the control's box (a font-18 heading in a 20px
        // box), whereas figo just lets it overflow. So a single-line box
        // (most headings/labels) stays autowrap-off and renders normally.
        const std::string& ar = n.textStyle.autoResize;
        const bool truncate = n.textStyle.truncateEnding || ar == "TRUNCATE";
        // Wrap only when the box is genuinely tall enough for 2+ lines, measured
        // against the LINE height (not font size — a generous line-height on a
        // single line must not be mistaken for multi-line and wrap+clip).
        const float lineH = n.textStyle.lineHeightPx > 0 ? n.textStyle.lineHeightPx
                                                         : n.textStyle.fontSize * 1.3f;
        const bool multiline = !truncate && lineH > 0 && n.height > lineH * 1.8f;
        // Vertical alignment. Figma's text box is sized tight to the glyphs, so a
        // single-line label (icon-as-emoji, button caption…) must center in its
        // box — Godot's default is TOP, which makes short text hug the top and
        // look off-center, and emoji metrics differ between the measuring engine
        // and Godot's rasterizer, widening the gap. Honor an explicit Center/
        // Bottom; otherwise default single-line text to center, leave multi-line
        // top-aligned so wrapped paragraphs flow downward as authored.
        int va = 0;
        switch (n.textStyle.alignV) {
            case figo::TextStyle::AlignV::Center: va = 1; break;
            case figo::TextStyle::AlignV::Bottom: va = 2; break;
            default: va = multiline ? 0 : 1; break;
        }
        if (va) body += "vertical_alignment = " + std::to_string(va) + "\n";
        if (multiline) body += "autowrap_mode = 3\n";        // WORD_SMART
        if (truncate) body += "text_overrun_behavior = 3\n";  // single-line ellipsis
    }

    // Emit node `n` (already named `name`, unique among siblings) under parentAttr.
    // pw/ph are the parent's logical size, for constraint -> anchor mapping.
    // Replay a node's CSS animation (opacity + 2D scale) as a child
    // AnimationPlayer. The player's root_node defaults to its parent (this
    // node), so tracks target "." . Must be called while `body`'s last [node]
    // is still this node (before any child [node] is written) so pivot_offset
    // attaches here. selfPath is the node's own .tscn path (its children's parent).
    // CSS timing function → cubic-bezier control points (x1,y1,x2,y2); false
    // for linear/steps. Non-linear curves get SAMPLED into sub-keys per segment
    // below — linear interpolation between samples reproduces any bezier,
    // including overshoot bounces (y outside [0,1]), which Godot's per-key
    // exponent transitions cannot express.
    static bool cssBezier(const std::string& e, float b[4]) {
        if (e == "ease")        { b[0]=0.25f; b[1]=0.1f;  b[2]=0.25f; b[3]=1; return true; }
        if (e == "ease-in")     { b[0]=0.42f; b[1]=0;     b[2]=1;     b[3]=1; return true; }
        if (e == "ease-out")    { b[0]=0;     b[1]=0;     b[2]=0.58f; b[3]=1; return true; }
        if (e == "ease-in-out") { b[0]=0.42f; b[1]=0;     b[2]=0.58f; b[3]=1; return true; }
        if (e.rfind("cubic-bezier(", 0) == 0 &&
            std::sscanf(e.c_str(), "cubic-bezier(%f ,%f ,%f ,%f", &b[0], &b[1], &b[2], &b[3]) == 4)
            return true;
        return false;
    }
    // Eased fraction y at time fraction x: solve the bezier's x(s)=x by
    // bisection (x(s) is monotonic for x1,x2 ∈ [0,1]), then evaluate y(s).
    static float bezY(const float b[4], float x) {
        float lo = 0, hi = 1;
        for (int i = 0; i < 40; ++i) {
            const float s = 0.5f * (lo + hi);
            const float xs = 3*(1-s)*(1-s)*s*b[0] + 3*(1-s)*s*s*b[2] + s*s*s;
            (xs < x ? lo : hi) = s;
        }
        const float s = 0.5f * (lo + hi);
        return 3*(1-s)*(1-s)*s*b[1] + 3*(1-s)*s*s*b[3] + s*s*s;
    }

    void emitAnim(const Node& n, const std::string& selfPath, float baseX, float baseY) {
        if (!n.anim) return;
        const NodeAnim& a = *n.anim;
        bool anyScale = false, anyOpacity = false, anyPos = false;
        for (const auto& k : a.keys) { anyScale |= k.hasScale; anyOpacity |= k.hasOpacity; anyPos |= k.hasPos; }
        if (!anyScale && !anyOpacity && !anyPos) return;

        // Scale pivots about transform-origin. The pivot is authored on the
        // element's logical box (lx/ly + width/height), but a raster branch
        // emits the node at the baked bbox origin (baseX/baseY, tight-cropped
        // with glow margins) — express the pivot in the emitted node's local
        // coordinates or the scale drifts off-center.
        if (anyScale && n.width > 0 && n.height > 0)
            body += "pivot_offset = Vector2(" +
                    num(n.relativeTransform.m02 + a.pivotX * n.width - baseX) + ", " +
                    num(n.relativeTransform.m12 + a.pivotY * n.height - baseY) + ")\n";

        // A CSS step timing (mcflash blinks 1↔0.35) maps to a DISCRETE update
        // (hold the key value, jump at the next key) — NOT interp=NEAREST, which
        // on a `modulate:a` sub-property track collapses to 0 in Godot (the flash
        // vanished). Everything else is continuous + linear; cubic is avoided
        // because its spline overshoots through the snap's near-coincident keys,
        // distorting size and even reversing the visible direction.
        const bool stepEase = a.ease.find("step") != std::string::npos;
        const int interp = 1;            // discrete update ignores interp; linear otherwise
        const int update = stepEase ? 1 : 0;   // 1 = UPDATE_DISCRETE, 0 = UPDATE_CONTINUOUS

        const std::string animRes = "Anim_" + std::to_string(subId);
        const std::string libRes = "AnimLib_" + std::to_string(subId);
        ++subId;

        // animation-delay on a FINITE animation is a choreography offset (the
        // death sequence staggers slash/shake/stamp/title by delay). Godot
        // players all autoplay at t=0, so bake it in: extend the length, shift
        // every key by +delay, and hold the first key's value from t=0 — which
        // is exactly CSS `both`/backwards fill during the delay. Infinite
        // loops already had their delay phase-baked into the keys by
        // web2canvas, so they get no shift here.
        const float dur = a.dur > 0 ? a.dur : 1.0f;
        const float delay = (a.iter != 0 && a.delay > 0) ? a.delay : 0.0f;
        std::string s = "[sub_resource type=\"Animation\" id=\"" + animRes + "\"]\n";
        s += "resource_name = \"a\"\n";
        s += "length = " + num(dur + delay) + "\n";
        s += "loop_mode = " + std::string(a.iter == 0 ? "1" : "0") + "\n";
        float bz[4];
        const bool haveBez = !stepEase && cssBezier(a.ease, bz);
        int tn = 0;
        auto track = [&](const char* prop, const std::function<bool(const AnimKey&)>& has,
                         const std::function<std::vector<float>(const AnimKey&)>& val,
                         const std::function<std::string(const std::vector<float>&)>& fmt) {
            std::vector<const AnimKey*> src;
            for (const auto& k : a.keys) if (has(k)) src.push_back(&k);
            if (src.size() < 2) return;
            std::vector<std::pair<float, std::vector<float>>> ks;
            if (delay > 0) ks.emplace_back(0.0f, val(*src[0]));  // pre-delay hold
            for (const AnimKey* k : src) ks.emplace_back(delay + k->t * a.dur, val(*k));
            // Replay the CSS timing function (applied per keyframe SEGMENT) by
            // sampling it into sub-keys — restores the rhythm: a slash's
            // fast-middle cubic-bezier, an ease-out settle, the ✕ stamp's
            // overshoot bounce. Flat (equal-value) and near-instant segments
            // (the delay hold, phase-baked seam snaps) are left unsampled.
            if (haveBez) {
                std::vector<std::pair<float, std::vector<float>>> ex;
                for (size_t i = 0; i + 1 < ks.size(); ++i) {
                    ex.push_back(ks[i]);
                    const auto& A = ks[i];
                    const auto& B = ks[i + 1];
                    bool flat = true;
                    for (size_t c = 0; c < A.second.size() && flat; ++c)
                        flat = std::fabs(A.second[c] - B.second[c]) <= 1e-4f;
                    if (flat || B.first - A.first < 0.04f) continue;
                    const int SUB = 12;
                    for (int u = 1; u < SUB; ++u) {
                        const float x = float(u) / SUB;
                        const float y = bezY(bz, x);
                        std::vector<float> v(A.second.size());
                        for (size_t c = 0; c < v.size(); ++c)
                            v[c] = A.second[c] + (B.second[c] - A.second[c]) * y;
                        ex.emplace_back(A.first + (B.first - A.first) * x, std::move(v));
                    }
                }
                ex.push_back(ks.back());
                ks.swap(ex);
            }
            std::string times, vals, trans;
            for (size_t i = 0; i < ks.size(); ++i) {
                if (i) { times += ", "; vals += ", "; trans += ", "; }
                times += num(ks[i].first);
                vals += fmt(ks[i].second);
                trans += "1";
            }
            s += "tracks/" + std::to_string(tn) + "/type = \"value\"\n";
            s += "tracks/" + std::to_string(tn) + "/imported = false\n";
            s += "tracks/" + std::to_string(tn) + "/enabled = true\n";
            s += "tracks/" + std::to_string(tn) + "/path = NodePath(\".:" + prop + "\")\n";
            s += "tracks/" + std::to_string(tn) + "/interp = " + std::to_string(interp) + "\n";
            s += "tracks/" + std::to_string(tn) + "/loop_wrap = true\n";
            s += "tracks/" + std::to_string(tn) + "/keys = {\n";
            s += "\"times\": PackedFloat32Array(" + times + "),\n";
            s += "\"transitions\": PackedFloat32Array(" + trans + "),\n";
            s += "\"update\": " + std::to_string(update) + ",\n";
            s += "\"values\": [" + vals + "]\n";
            s += "}\n";
            ++tn;
        };
        const auto fmt1 = [](const std::vector<float>& v) { return num(v[0]); };
        const auto fmt2 = [](const std::vector<float>& v) {
            return std::string("Vector2(") + num(v[0]) + ", " + num(v[1]) + ")";
        };
        track("modulate:a", [](const AnimKey& k) { return k.hasOpacity; },
              [](const AnimKey& k) { return std::vector<float>{k.opacity}; }, fmt1);
        track("scale", [](const AnimKey& k) { return k.hasScale; },
              [](const AnimKey& k) { return std::vector<float>{k.sx, k.sy}; }, fmt2);
        // Position track: the rest-relative px delta added to the node's exported
        // top-left offset (a translate shake / a rotated slash sweep).
        track("position", [](const AnimKey& k) { return k.hasPos; },
              [&](const AnimKey& k) { return std::vector<float>{baseX + k.dx, baseY + k.dy}; }, fmt2);
        if (tn == 0) return;  // nothing replayable

        s += "\n[sub_resource type=\"AnimationLibrary\" id=\"" + libRes + "\"]\n";
        s += "_data = {\n\"a\": SubResource(\"" + animRes + "\")\n}\n";
        frameSub.push_back(s);

        body += "\n[node name=\"AnimPlayer\" type=\"AnimationPlayer\" parent=\"" + selfPath + "\"]\n";
        body += "libraries = {\n\"\": SubResource(\"" + libRes + "\")\n}\n";
        body += "autoplay = \"a\"\n";
    }

    void emit(Node& n, const std::string& parentAttr, const std::string& name, float pw, float ph) {
        if (n.type == NodeType::Slice) return;

        const float lx = n.relativeTransform.m02, ly = n.relativeTransform.m12;
        // Parent's absolute position, to turn absolute bake coords into the
        // node's parent-relative offset for the .tscn.
        const float pax = n.parent ? n.parent->absoluteTransform.m02 : 0.0f;
        const float pay = n.parent ? n.parent->absoluteTransform.m12 : 0.0f;
        const bool isRoot = parentAttr.empty();
        const bool isContainer = !n.children.empty();
        // Scroll analysis: a frame becomes a ScrollContainer only when it is
        // marked scrollable AND its content actually exceeds its box on that
        // axis (CSS overflow:auto shows a scrollbar only when overflowing). This
        // keeps full-screen frames — which carry a blanket scrollDirection but
        // whose content exactly fills them — as plain clipped containers.
        using SD = figo::ScrollDirection;
        float scrollCW = 0.0f, scrollCH = 0.0f;
        if (isContainer && n.scrolls())
            for (auto& c : n.children) {
                scrollCW = std::max(scrollCW, c->relativeTransform.m02 + c->width);
                scrollCH = std::max(scrollCH, c->relativeTransform.m12 + c->height);
            }
        const bool scrollV = isContainer && n.scrolls() &&
            (n.scrollDirection == SD::Vertical || n.scrollDirection == SD::Both) && scrollCH > n.height + 1.0f;
        const bool scrollH = isContainer && n.scrolls() &&
            (n.scrollDirection == SD::Horizontal || n.scrollDirection == SD::Both) && scrollCW > n.width + 1.0f;
        const bool scrollFrame = scrollV || scrollH;
        const std::string childAttr =
            isRoot ? "." : (parentAttr == "." ? name : parentAttr + "/" + name);

        auto placeNode = [&](float x, float y, float w, float h) {
            // Root gets an explicit design-size rect so the scene opens at the
            // right size in the editor; children anchor within it. Project
            // stretch (canvas_items) scales the whole UI to the window.
            if (isRoot) rect(0, 0, n.width, n.height);
            else place(x, y, w, h, n.constraintH, n.constraintV, pw, ph);
        };

        // Prefab reuse: a repeated component is instanced from its PackedScene,
        // with per-instance text overrides; its subtree is not re-emitted.
        if (const Comp* comp = extractedComponent(n)) {
            usedComps.insert(comp->name);  // only referenced comps get a scene file
            std::string id = useExt("res://components/" + comp->name + ".tscn", "PackedScene", "scn_");
            body += "\n[node name=\"" + name + "\" parent=\"" + parentAttr +
                    "\" instance=ExtResource(\"" + id + "\")]\n";
            placeNode(lx, ly, n.width, n.height);
            if (!n.visible) body += "visible = false\n";
            if (n.opacity < 0.999f) body += "modulate = Color(1, 1, 1, " + num(n.opacity) + ")\n";
            emitInstanceOverrides(*comp->canon, n, childAttr);
            json nj;
            nj["name"] = name; nj["type"] = "instance"; nj["component"] = comp->name;
            frameNodes.push_back(nj);
            return;
        }

        json nodeJson;
        nodeJson["name"] = name;
        nodeJson["path"] = childAttr;
        // Top-left offset actually emitted for this node — the base a position
        // animation track adds its delta to (raster branches bake from b.x/b.y).
        float baseX = lx, baseY = ly;

        if (n.type == NodeType::Text) {
            header(name, "Label", parentAttr);
            // Icon-glyph centering: a short glyph (emoji/symbol button icon) is
            // measured in the source font as a tight ink box. After font
            // substitution in Godot the glyph's advance/baseline differ, so
            // re-centering it inside that tiny box drifts off-center. When the
            // glyph already sits centered in its parent (equal margins on both
            // axes), expand the Label to fill that parent and center — the full
            // box has the slack to absorb metric differences. Gated tight to
            // avoid moving glyphs that are deliberately offset within a large
            // container (a resize handle pinned to the right of an input row):
            // ≤2 codepoints, center-aligned, smaller than the parent, AND
            // symmetric margins on both axes.
            const float marginTol = 4.0f;
            const float lM = lx, rM = pw - (lx + n.width);
            const float tM = ly, bM = ph - (ly + n.height);
            const bool iconGlyph =
                !isRoot && n.textStyle.alignH == figo::TextStyle::AlignH::Center &&
                pw > 1 && ph > 1 && n.width < pw - 1 && n.height < ph - 1 &&
                utf8Count(n.characters) <= 2 &&
                std::fabs(lM - rM) <= marginTol && std::fabs(tM - bM) <= marginTol;
            if (iconGlyph) rect(0, 0, pw, ph);
            else placeNode(lx, ly, n.width, n.height);
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
                baseX = b.x - pax; baseY = b.y - pay;
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
            emitAnim(n, childAttr, baseX, baseY);
            return;  // do not recurse into the flattened subtree
        } else if (scrollFrame) {
            // overflow:auto/scroll frame whose content overflows -> Godot
            // ScrollContainer. The container clips + shows a scrollbar (AUTO) on
            // the overflowing axis; its single content child "__scroll" holds the
            // absolutely-placed children and carries custom_minimum_size = the
            // content extent, which is what tells the container how far to
            // scroll. A uniform-solid background would scroll invisibly and a
            // baked/gradient bg can't ride along, so a scroll frame keeps only
            // its content (its panel chrome lives on an ancestor, as in the chat
            // log inside its bordered panel).
            header(name, "ScrollContainer", parentAttr);
            placeNode(lx, ly, n.width, n.height);
            commonProps(n, isRoot);
            body += "horizontal_scroll_mode = " + std::string(scrollH ? "1" : "0") + "\n";  // 1=auto, 0=disabled
            body += "vertical_scroll_mode = "   + std::string(scrollV ? "1" : "0") + "\n";
            nodeJson["type"] = "ScrollContainer";
            frameNodes.push_back(nodeJson);
            emitAnim(n, childAttr, baseX, baseY);

            const std::string contentAttr = (childAttr == ".") ? "__scroll" : childAttr + "/__scroll";
            header("__scroll", "Control", childAttr);
            // Only the overflowing axis gets an explicit extent; the cross axis
            // is left at 0 so the container fits the content to its own size.
            body += "custom_minimum_size = Vector2(" + num(scrollH ? scrollCW : 0.0f) + ", " +
                    num(scrollV ? scrollCH : 0.0f) + ")\n";
            std::map<std::string, int> usedS;
            int idxS = 0;
            for (auto& child : n.children) {
                std::string fb = "Node" + std::to_string(idxS++);
                std::string base = sanitizeName(child->name, fb.c_str());
                std::string uniq = base; int k = 2;
                while (usedS.count(uniq)) uniq = base + "_" + std::to_string(k++);
                usedS[uniq] = 1;
                emit(*child, contentAttr, uniq, scrollH ? scrollCW : n.width, scrollV ? scrollCH : n.height);
            }
            return;
        } else if (isContainer) {
            header(name, "Control", parentAttr);
            placeNode(lx, ly, n.width, n.height);
            commonProps(n, isRoot);
            nodeJson["type"] = "Control";
            if (needsBake(n)) emitBg(n, childAttr, nodeJson);
            else if (const auto* f = solidFill(n)) {
                nodeJson["solidBg"] = paintLit(*f);
                emitColorBg(f, childAttr);
            }
        } else if (needsBake(n)) {
            // 9-slice candidates: bake at 1x first (crisp corners, no downsample
            // blur) and keep only if it's truly a 9-slice; otherwise full 2x bake.
            Baked b;
            if (nineCandidate(n)) b = bake(n, 1, /*flatten=*/false, /*tryNine=*/true, /*nineOnly=*/true);
            if (!b.ok) b = bake(n, superScale);
            if (b.ok) {
                std::string id = useTexture(b.hash);
                if (b.nine) {
                    header(name, "NinePatchRect", parentAttr);
                    placeNode(lx, ly, n.width, n.height);
                    commonProps(n, isRoot);
                    body += "texture = ExtResource(\"" + id + "\")\n";
                    body += "patch_margin_left = " + std::to_string(b.ml) + "\n";
                    body += "patch_margin_top = " + std::to_string(b.mt) + "\n";
                    body += "patch_margin_right = " + std::to_string(b.mr) + "\n";
                    body += "patch_margin_bottom = " + std::to_string(b.mb) + "\n";
                    nodeJson["type"] = "NinePatchRect";
                    nodeJson["ninePatch"] = {b.ml, b.mt, b.mr, b.mb};
                } else {
                    header(name, "TextureRect", parentAttr);
                    placeNode(b.x - pax, b.y - pay, b.w, b.h);
                    baseX = b.x - pax; baseY = b.y - pay;
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
            body += "color = " + paintLit(*f) + "\n";
            nodeJson["type"] = "ColorRect";
        } else {
            header(name, "Control", parentAttr);
            placeNode(lx, ly, n.width, n.height);
            commonProps(n, isRoot);
            nodeJson["type"] = "Control";
        }

        frameNodes.push_back(nodeJson);
        emitAnim(n, childAttr, baseX, baseY);

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
    void emitColorBg(const figo::Paint* f, const std::string& parentAttr) {
        if (!f) return;
        header("__bg", "ColorRect", parentAttr);
        placeFull();
        body += "color = " + paintLit(*f) + "\n";
    }

    // Baked background for a container with a complex fill/stroke/corners.
    void emitBg(Node& n, const std::string& parentAttr, json& nodeJson) {
        Baked b;
        if (nineCandidate(n)) b = bake(n, 1, /*flatten=*/false, /*tryNine=*/true, /*nineOnly=*/true);
        if (!b.ok) b = bake(n, superScale);
        if (!b.ok) return;
        std::string id = useTexture(b.hash);
        if (b.nine) {
            header("__bg", "NinePatchRect", parentAttr);
            placeFull();  // stretch to the container's full rect
            body += "texture = ExtResource(\"" + id + "\")\n";
            body += "patch_margin_left = " + std::to_string(b.ml) + "\n";
            body += "patch_margin_top = " + std::to_string(b.mt) + "\n";
            body += "patch_margin_right = " + std::to_string(b.mr) + "\n";
            body += "patch_margin_bottom = " + std::to_string(b.mb) + "\n";
            nodeJson["ninePatch"] = {b.ml, b.mt, b.mr, b.mb};
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

    // A flat rectangular panel (solid/translucent fill, optional 1px border,
    // clip-path cut OR rounded corners — no gradient-with-content, no image, no
    // glow) is a 9-slice CANDIDATE: baked at 1x and tested pixel-wise by
    // nineShrink. Icons/photos (image fill) and shadowed panels keep the full 2x
    // bake. The actual slice only happens if the pixels really are stretchable.
    // Only LARGE panels 9-slice — their corners are a small fraction of the area,
    // so a 1x-corner NinePatch is barely noticeable while the size win is huge.
    // Small controls (buttons, sliders, segments, toggles, progress bars) keep
    // their full 2x bake so their corners/edges stay crisp; the bytes saved there
    // would be tiny anyway.
    static constexpr float kNineMinArea = 40000.0f;  // ~200x200 logical
    bool nineCandidate(const Node& n) {
        if (!isRectish(n.type)) return false;
        if (hasVisibleEffect(n)) return false;
        if (n.width * n.height < kNineMinArea) return false;
        return true;  // image fills allowed: a web2canvas-baked clip-corner panel
                      // is an IMAGE fill; nineShrink rejects real photos (no run).
    }

    void emitOffsets(const Node& ic) {
        body += "offset_left = " + num(ic.relativeTransform.m02) + "\n";
        body += "offset_top = " + num(ic.relativeTransform.m12) + "\n";
        body += "offset_right = " + num(ic.relativeTransform.m02 + ic.width) + "\n";
        body += "offset_bottom = " + num(ic.relativeTransform.m12 + ic.height) + "\n";
    }
    // Per-instance overrides vs the canon. The grouping key includes the
    // structural shape, but state variants of one component differ in WHICH
    // children they have, so canon and instance no longer share a child sequence.
    // Children are matched by slot identity (slotKey): a matched leaf gets the
    // override it needs (offset/size, text, font color, solid fill, re-baked
    // texture); a canon-only slot is HIDDEN in this state; an instance-only slot
    // (phase 2) is ADDED to this instance as a fresh node. Net effect: ONE prefab
    // (the superset canon) realizes every state via per-instance toggles + adds.
    void emitInstanceOverrides(const Node& canon, Node& inst, const std::string& parentPath) {
        std::map<std::string, int> used;
        used["__bg"] = 1;
        // A container's solid background is a `__bg` ColorRect; override its color.
        if (!canon.children.empty())
        if (const auto* cb = solidFill(canon)) if (const auto* ib = solidFill(inst))
            if (colorNe(cb->color, ib->color)) {
                body += "\n[node name=\"__bg\" parent=\"" + parentPath + "\"]\n";
                body += "color = " + paintLit(*ib) + "\n";
            }
        // Match each canon child to an instance child by slot identity, not by
        // index: once state variants share one prefab, a leaner instance is
        // missing some canon children and index alignment shifts everything after
        // the gap. A canon child with no instance match is HIDDEN (the state
        // toggle). Iterate ALL canon children in order so the generated node names
        // line up exactly with emitComponentScene()'s emit() (which names by the
        // running child index), keeping the override [node] paths valid.
        std::vector<char> instUsed(inst.children.size(), 0);
        auto matchInst = [&](const Node& cc) -> Node* {
            const std::string key = slotKey(cc);
            // Among unused instance children of the same slot class, take the one
            // POSITIONALLY nearest to the canon child. Identical-shape repeated
            // siblings (segmented-control options, grid cells) share a slotKey;
            // without the position tiebreak they pair by source/DOM order, which
            // need not be the visual order — pairing the canon's left segment to
            // the instance's first-in-DOM (maybe rightmost) segment and then
            // repositioning collapses two labels onto one spot. Nearest-position
            // pairing keeps each sibling at its own slot.
            int best = -1;
            float bestD = 0;
            for (size_t j = 0; j < inst.children.size(); ++j) {
                if (instUsed[j] || slotKey(*inst.children[j]) != key) continue;
                const Node& c = *inst.children[j];
                const float d = std::fabs(c.relativeTransform.m02 - cc.relativeTransform.m02) +
                                std::fabs(c.relativeTransform.m12 - cc.relativeTransform.m12);
                if (best < 0 || d < bestD) { best = (int)j; bestD = d; }
            }
            if (best < 0) return nullptr;
            instUsed[best] = 1;
            return inst.children[(size_t)best].get();
        };
        for (size_t i = 0; i < canon.children.size(); ++i) {
            const Node& cc = *canon.children[i];
            std::string fb = "Node" + std::to_string(i);
            std::string base = sanitizeName(cc.name, fb.c_str());
            std::string uniq = base;
            int k = 2;
            while (used.count(uniq)) uniq = base + "_" + std::to_string(k++);
            used[uniq] = 1;
            Node* icp = matchInst(cc);
            if (!icp) {
                // Canon carries this child, this instance/state doesn't -> hide it.
                // (If the canon child is already hidden there's nothing to do.)
                if (cc.visible) {
                    body += "\n[node name=\"" + uniq + "\" parent=\"" + parentPath + "\"]\n";
                    body += "visible = false\n";
                }
                continue;
            }
            Node& ic = *icp;
            const bool posDiff =
                std::lround(cc.width) != std::lround(ic.width) ||
                std::lround(cc.height) != std::lround(ic.height) ||
                std::lround(cc.relativeTransform.m02) != std::lround(ic.relativeTransform.m02) ||
                std::lround(cc.relativeTransform.m12) != std::lround(ic.relativeTransform.m12);
            if (cc.type == NodeType::Text) {
                const auto *fc = solidFill(cc), *fi = solidFill(ic);
                const bool colDiff = fc && fi && colorNe(fc->color, fi->color);
                if (posDiff || cc.characters != ic.characters || colDiff) {
                    body += "\n[node name=\"" + uniq + "\" parent=\"" + parentPath + "\"]\n";
                    emitOffsets(ic);
                    body += "text = \"" + escapeStr(ic.characters) + "\"\n";
                    if (fi) body += "theme_override_colors/font_color = " + paintLit(*fi) + "\n";
                }
            } else if (needsBake(cc)) {
                // A baked leaf (glow/gradient/clip) is placed at its SPRITE bounds,
                // which a glow makes larger than the node box. The canon emits at
                // those bounds, so an instance must too — re-bake it and place at
                // the re-baked bounds, else inheriting the canon sprite into the
                // node box squashes it (e.g. a slider fill's glow collapsing to a
                // few px, the reported "progress bar not restored").
                Baked bc = bake(cc, superScale), bi = bake(ic, superScale);
                if (bi.ok && (bc.hash != bi.hash || posDiff)) {
                    const float pax = ic.parent ? ic.parent->absoluteTransform.m02 : 0.f;
                    const float pay = ic.parent ? ic.parent->absoluteTransform.m12 : 0.f;
                    body += "\n[node name=\"" + uniq + "\" parent=\"" + parentPath + "\"]\n";
                    body += "offset_left = " + num(bi.x - pax) + "\n";
                    body += "offset_top = " + num(bi.y - pay) + "\n";
                    body += "offset_right = " + num(bi.x - pax + bi.w) + "\n";
                    body += "offset_bottom = " + num(bi.y - pay + bi.h) + "\n";
                    if (bc.hash != bi.hash) body += "texture = ExtResource(\"" + useTexture(bi.hash) + "\")\n";
                }
                emitInstanceOverrides(cc, ic, parentPath + "/" + uniq);
            } else {
                const std::string ci = imageRefOf(cc), ii = imageRefOf(ic);
                const bool imgDiff = !ii.empty() && ii != ci;
                const auto *lc = solidFill(cc), *li = solidFill(ic);
                const bool leafColDiff = cc.children.empty() && lc && li && colorNe(lc->color, li->color);
                if (posDiff || imgDiff || leafColDiff) {
                    body += "\n[node name=\"" + uniq + "\" parent=\"" + parentPath + "\"]\n";
                    if (posDiff) emitOffsets(ic);
                    if (imgDiff) { Baked b = bake(ic, superScale); if (b.ok) body += "texture = ExtResource(\"" + useTexture(b.hash) + "\")\n"; }
                    if (leafColDiff) body += "color = " + paintLit(*li) + "\n";
                }
                emitInstanceOverrides(cc, ic, parentPath + "/" + uniq);
            }
        }

        // Phase 2 — instance-only slots: children THIS state has that the superset
        // canon lacks (an "eliminated" overlay / a slider track in a row whose
        // canon is the toggle variant). Phase 1 dropped them; emit each now as an
        // ADDED node under this instance via the normal emit() path. We're inside a
        // FRAME render (convertFrame already rendered inst's frame), so the node's
        // absoluteTransform/fills bake correctly — unlike grafting it into the
        // component scene, which renders a different frame. Names are deduped
        // against the canon children at this level (and emit()'s reserved __bg) so
        // the added node can't collide with — and thus silently re-target — an
        // existing prefab node. pw/ph = inst's box, since inst is these nodes' parent.
        for (size_t j = 0; j < inst.children.size(); ++j) {
            if (instUsed[j]) continue;
            Node& extra = *inst.children[j];
            if (!extra.visible) continue;  // a hidden-in-source extra adds nothing
            std::string fb = "Add" + std::to_string(j);
            std::string base = sanitizeName(extra.name, fb.c_str());
            std::string uniq = base;
            int k = 2;
            while (used.count(uniq)) uniq = base + "_" + std::to_string(k++);
            used[uniq] = 1;
            emit(extra, parentPath, uniq, inst.width, inst.height);
        }
    }

    // Emit a component's PackedScene (components/<name>.tscn), self-contained.
    void emitComponentScene(Comp& comp) {
        ui->setViewport((uint32_t)std::ceil(std::max(1.0f, comp.frame->width)),
                        (uint32_t)std::ceil(std::max(1.0f, comp.frame->height)));
        curW = (uint32_t)std::ceil(std::max(1.0f, comp.frame->width));
        curH = (uint32_t)std::ceil(std::max(1.0f, comp.frame->height));
        ui->selectFrame(comp.frame->name);
        ui->render();

        body.clear(); frameExtId.clear(); frameExt.clear(); frameSub.clear();
        subId = 0; frameNodes = json::array();
        inComponent = true;
        const uint32_t w = (uint32_t)std::ceil(std::max(1.0f, comp.canon->width));
        const uint32_t h = (uint32_t)std::ceil(std::max(1.0f, comp.canon->height));
        emit(*const_cast<Node*>(comp.canon), "", comp.name, (float)w, (float)h);
        inComponent = false;

        std::string head = "[gd_scene load_steps=" + std::to_string(frameExt.size() + frameSub.size() + 1) + " format=3]\n";
        for (auto& e : frameExt)
            head += "\n[ext_resource type=\"" + e.type + "\" path=\"" + e.path + "\" id=\"" + e.id + "\"]\n";
        for (auto& s : frameSub) head += "\n" + s;
        fs::create_directories(outDir / "components");
        std::ofstream f(outDir / "components" / (comp.name + ".tscn"), std::ios::binary);
        f << head << body;
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
        frameSub.clear();
        subId = 0;
        frameNodes = json::array();

        std::string rootName = sanitizeName(frame->name, "Frame");
        emit(*frame, "", rootName, (float)curW, (float)curH);

        std::string head = "[gd_scene load_steps=" +
                           std::to_string(frameExt.size() + frameSub.size() + 1) + " format=3]\n";
        for (auto& e : frameExt)
            head += "\n[ext_resource type=\"" + e.type + "\" path=\"" + e.path +
                    "\" id=\"" + e.id + "\"]\n";
        for (auto& s : frameSub) head += "\n" + s;

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
         "config/name=\"figo2godot export\"\n"
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
    for (const auto& de : fs::recursive_directory_iterator(fontsSrc, ec)) {
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
        std::printf("usage: figo2godot <input> [outDir] [--frame NAME] [--fonts DIR] "
                    "[--scale N] [--prefabs] [--prefab-anon] [--no-prefab T1,T2]\n");
        return 2;
    }
    const std::string input = argv[1];
    fs::path outDir = "godot_out";
    std::string onlyFrame;
    fs::path fontsDir;
    int scale = 2;
    bool prefabs = false;
    bool prefabAnon = false;
    std::set<std::string> noPrefabArg;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frame" && i + 1 < argc)
            onlyFrame = argv[++i];
        else if (a == "--fonts" && i + 1 < argc)
            fontsDir = argv[++i];
        else if (a == "--scale" && i + 1 < argc)
            scale = std::max(1, atoi(argv[++i]));
        else if (a == "--prefabs")
            prefabs = true;
        else if (a == "--prefab-anon")  // also extract repeated anonymous containers
            prefabs = prefabAnon = true;
        else if (a == "--no-prefab" && i + 1 < argc) {
            std::string list = argv[++i], tok;
            for (char c : list) {
                if (c == ',') { if (!tok.empty()) noPrefabArg.insert(tok); tok.clear(); }
                else tok.push_back(c);
            }
            if (!tok.empty()) noPrefabArg.insert(tok);
        }
        else if (!a.empty() && a[0] != '-')
            outDir = a;
    }
    // Default: a "fonts" directory next to the input file.
    if (fontsDir.empty()) {
        fs::path sib = fs::path(input).parent_path() / "fonts";
        if (fs::exists(sib)) fontsDir = sib;
    }

    std::printf("loading %s\n", input.c_str());
    std::unique_ptr<figo::FigmaUI> ui;
    try {
        ui = figo::FigmaUI::fromFile(input);
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
    cv.prefabAnon = prefabAnon;  // read by scanComponents (pre-pass) below
    cv.noPrefab = noPrefabArg;
    cv.outDir = outDir;
    cv.spritesDir = spritesDir;
    cv.superScale = scale;
    cv.fonts = loadFonts(fontsDir, outDir / "fonts");
    std::printf("fonts: %zu file(s) from %s\n", cv.fonts.size(),
                fontsDir.empty() ? "(none)" : fontsDir.string().c_str());

    // Prefab pre-pass: find repeated components, emit their PackedScenes, then
    // the frame loop instances them.
    if (prefabs) {
        for (Node* frame : frames) {
            if (!onlyFrame.empty() && frame->name != onlyFrame) continue;
            cv.scanComponents(frame);
        }
        // Partition each group into structural clusters. Order instances richest-
        // first so each new cluster's canon is the SUPERSET of its members; an
        // instance joins the first cluster whose canon it FITS (poorFit==false),
        // else starts a new one. So incompatible variants become SEVERAL prefabs.
        for (auto& kv : cv.compBySig) {
            Converter::Comp& group = kv.second;
            if (group.insts.empty()) continue;
            std::sort(group.insts.begin(), group.insts.end(),
                      [](const auto& a, const auto& b) {
                          return Converter::descendants(*a.first) > Converter::descendants(*b.first);
                      });
            std::vector<Converter::Comp*> clusters;
            for (auto& inst : group.insts) {
                Converter::Comp* fit = nullptr;
                for (Converter::Comp* c : clusters)
                    if (!cv.poorFit(*c->canon, *inst.first)) { fit = c; break; }
                if (!fit) {
                    cv.variants.push_back(std::make_unique<Converter::Comp>());
                    fit = cv.variants.back().get();
                    fit->canon = inst.first;
                    fit->frame = inst.second;
                    fit->name = group.name;
                    clusters.push_back(fit);
                }
                fit->count++;
                cv.nodeComp[inst.first] = fit;
            }
        }
        // Extract every cluster repeated >=2× (skip screen-sized containers and
        // user-suppressed wrapper types); give each a unique scene name.
        std::set<std::string> usedNames;
        int compCount = 0;
        for (auto& up : cv.variants) {
            Converter::Comp& comp = *up;
            if (comp.count < 2) continue;
            // Screen-level guard: a component covering ~the whole frame is a SCREEN
            // container (Lobby, HudC…), not a reusable widget — never extract it.
            if (comp.canon->width >= comp.frame->width * 0.9f &&
                comp.canon->height >= comp.frame->height * 0.9f) continue;
            // User-declared generic wrappers (--no-prefab HPanel,HRow) never extract.
            if (cv.noPrefab.count(comp.canon->compType)) continue;
            std::string base;
            for (char c : sanitizeName(comp.name, "Comp"))
                base.push_back((std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_');
            if (base.empty()) base = "Comp";
            std::string uniq = base;
            int k = 2;
            while (usedNames.count(uniq)) uniq = base + "_" + std::to_string(k++);
            usedNames.insert(uniq);
            comp.name = uniq;
            comp.extracted = true;
            ++compCount;
        }
        cv.prefabs = true;
        std::printf("prefabs: %d candidate cluster(s)\n", compCount);
    }

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
    // Emit only the component scenes a frame actually instanced. A comp used
    // solely nested inside another comp was inlined there (inComponent disables
    // nested instancing), so its .tscn would be a dead orphan — skip it. (Done
    // after the frame loop, once usedComps is fully populated.)
    if (prefabs) {
        int emitted = 0, extracted = 0;
        for (auto& up : cv.variants) {
            if (!up->extracted) continue;
            ++extracted;
            if (cv.usedComps.count(up->name)) { cv.emitComponentScene(*up); ++emitted; }
        }
        std::printf("prefabs: %d scene(s) emitted (%d orphan candidate(s) pruned)\n",
                    emitted, extracted - emitted);
    }
    cv.writeManifest();
    writeProjectGodot(outDir, baseW ? baseW : 390, baseH ? baseH : 844);

    std::printf("RESULT: OK, %d scene(s), %zu unique sprite(s)\n", n, cv.all.size());
    return 0;
}
