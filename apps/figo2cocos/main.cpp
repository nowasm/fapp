// figo2cocos — convert a figo design (.fig / canvas.json / REST JSON) into
// Cocos Creator 3.x assets: one .prefab per top-level frame + a shared textures/
// folder of deduplicated PNG sprites (each with a .meta sprite-frame). The
// design's own ThorVG rasterizer bakes the sprites, so the textures are
// pixel-identical to the figo runtime.
//
// A .prefab is a JSON array of serialized engine objects (cc.Prefab, cc.Node,
// cc.UITransform, cc.Sprite, cc.Label, …) wired by {"__id__": <arrayIndex>}
// references — the format ported from psd2prefab (Cocos Creator 3.4.2+).
//
// Node mapping:
//   TEXT                                   -> cc.Node + cc.Label (system font)
//   plain solid rect / container bg        -> cc.Node + cc.Sprite (1x1 white, tinted)
//   ellipse / vector / gradient / image /
//     stroke / effect / rounded panel      -> cc.Node + cc.Sprite (baked PNG; 9-slice)
//   container                              -> cc.Node holding a bg sprite + children
//   anything empty                         -> cc.Node (passthrough)
//
// Coordinate flip: every node uses anchor (0,1) (top-left), _lpos = (relX, -relY)
// and a cc.UITransform with the figo box size — a clean Figma(Y-down) -> Cocos
// (Y-up) mapping that composes correctly down the tree.
//
// Scope (v1): self-contained prefab per frame (no nested-prefab extraction),
// system fonts, axis-aligned (no rotation) — matching psd2prefab's output shape.

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
#include <random>
#include <string>
#include <vector>

#include <figo/figo.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using figo::Node;
using figo::NodeType;
using nlohmann::json;

// ===================== PNG encoder (RGBA8, self-contained) ==================
// Minimal valid PNG: stored (uncompressed) zlib stream. Larger than a real
// deflate, but Cocos reads them fine and there are zero dependencies.

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
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
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

// Filesystem-safe stem (alnum/-/_), capped — for texture/prefab filenames.
static std::string fileStem(const std::string& name, const char* fallback) {
    std::string stem;
    for (char c : sanitizeName(name, fallback))
        stem.push_back((std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_');
    if (stem.empty()) stem = fallback;
    if (stem.size() > 40) stem.resize(40);
    return stem;
}

static double rnd(float v) {  // round to 0.01 so JSON stays compact
    double r = std::round(v * 100.0) / 100.0;
    return r == 0.0 ? 0.0 : r;  // normalize -0
}

// A figo Color [0,1] -> Cocos cc.Color {r,g,b,a} 0..255 ints, folding the
// paint's separate opacity into alpha.
static int u8(float v) { return std::max(0, std::min(255, (int)std::lround(v * 255.0f))); }
static json ccColor(const figo::Color& c) {
    return json{{"__type__", "cc.Color"}, {"r", u8(c.r)}, {"g", u8(c.g)}, {"b", u8(c.b)}, {"a", u8(c.a)}};
}
static json ccPaintColor(const figo::Paint& p) {
    figo::Color c = p.color;
    c.a *= p.opacity;
    return ccColor(c);
}
static json ccSize(float w, float h) {
    return json{{"__type__", "cc.Size"}, {"width", rnd(w)}, {"height", rnd(h)}};
}
static json ccVec2(float x, float y) {
    return json{{"__type__", "cc.Vec2"}, {"x", rnd(x)}, {"y", rnd(y)}};
}
static json ccVec3(float x, float y, float z) {
    return json{{"__type__", "cc.Vec3"}, {"x", rnd(x)}, {"y", rnd(y)}, {"z", rnd(z)}};
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

// A multi-part vector icon (or boolean op) is best rasterized as one flat image.
static bool isVectorIcon(const Node& n) {
    if (n.type == NodeType::BooleanOperation) return true;
    if (n.children.empty()) return false;
    const float maxDim = std::max(n.width, n.height);
    if (maxDim > 128.0f) return false;
    if (subtreeHasText(n)) return false;
    return subtreeHasVector(n);
}

// A node whose appearance a flat tinted quad can't reproduce -> must be baked.
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

// ===================== Cocos UUIDs ==========================================

// 36-char hyphenated v4-shaped uuid from a 64-bit seed (deterministic, so the
// same content yields the same uuid across runs and dedups cleanly).
static std::string makeUuid(uint64_t seed) {
    std::mt19937_64 rng(seed ? seed : 0x9E3779B97F4A7C15ull);
    uint8_t b[16];
    for (int i = 0; i < 16; i += 8) {
        uint64_t v = rng();
        for (int k = 0; k < 8; ++k) b[i + k] = (uint8_t)(v >> (k * 8));
    }
    b[6] = (b[6] & 0x0f) | 0x40;  // version 4
    b[8] = (b[8] & 0x3f) | 0x80;  // variant
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(hex[b[i] >> 4]);
        s.push_back(hex[b[i] & 0xf]);
    }
    return s;
}

// Compress a 36-char hyphenated uuid to Cocos' 22-char fileId form (2 head hex
// chars kept, the remaining 30 packed 3-hex -> 2-base64). Ported from
// psd2prefab's UuidUtils.compressHex (reserved head = 2).
static std::string compressUuid(const std::string& uuid) {
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string hexStr;
    for (char c : uuid)
        if (c != '-') hexStr.push_back(c);
    if (hexStr.size() != 32) return uuid;
    auto hv = [&](size_t i) { char c = hexStr[i]; return c <= '9' ? c - '0' : (std::tolower(c) - 'a' + 10); };
    std::string out;
    out.push_back(hexStr[0]);
    out.push_back(hexStr[1]);
    for (size_t i = 2; i + 2 < 32; i += 3) {
        int h1 = hv(i), h2 = hv(i + 1), h3 = hv(i + 2);
        out.push_back(B64[(h1 << 2) | (h2 >> 2)]);
        out.push_back(B64[((h2 & 3) << 4) | h3]);
    }
    return out;
}

// FNV-1a 64 over a string (stable uuid seeds from names/content).
static uint64_t fnv1a(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// ===================== meta templates (Cocos 3.4.2+) ========================

static const char* kSpriteFrameMeta = R"META({
  "ver": "1.0.22",
  "importer": "image",
  "imported": true,
  "uuid": "$TEX",
  "files": [".png", ".json"],
  "subMetas": {
    "6c48a": {
      "importer": "texture",
      "uuid": "$TEX@6c48a",
      "displayName": "$NAME",
      "id": "6c48a",
      "name": "texture",
      "userData": {
        "wrapModeS": "clamp-to-edge",
        "wrapModeT": "clamp-to-edge",
        "imageUuidOrDatabaseUri": "$TEX",
        "minfilter": "linear",
        "magfilter": "linear",
        "mipfilter": "none",
        "anisotropy": 0,
        "isUuid": true
      },
      "ver": "1.0.21",
      "imported": true,
      "files": [".json"],
      "subMetas": {}
    },
    "f9941": {
      "importer": "sprite-frame",
      "uuid": "$TEX@f9941",
      "displayName": "$NAME",
      "id": "f9941",
      "name": "spriteFrame",
      "userData": {
        "trimType": "auto",
        "trimThreshold": 1,
        "rotated": false,
        "offsetX": 0,
        "offsetY": 0,
        "trimX": 0,
        "trimY": 0,
        "width": $W,
        "height": $H,
        "rawWidth": $W,
        "rawHeight": $H,
        "borderTop": $BT,
        "borderBottom": $BB,
        "borderLeft": $BL,
        "borderRight": $BR,
        "packable": true,
        "isUuid": true,
        "imageUuidOrDatabaseUri": "$TEX@6c48a",
        "atlasUuid": ""
      },
      "ver": "1.0.9",
      "imported": true,
      "files": [".json"],
      "subMetas": {}
    }
  },
  "userData": {
    "type": "sprite-frame",
    "hasAlpha": true,
    "redirect": "$TEX@f9941"
  }
})META";

static const char* kPrefabMeta = R"META({
  "ver": "1.1.35",
  "importer": "prefab",
  "imported": true,
  "uuid": "$UUID",
  "files": [".json"],
  "subMetas": {},
  "userData": {
    "syncNodeName": "$NAME"
  }
})META";

static std::string subst(std::string tpl, const std::vector<std::pair<std::string, std::string>>& kv) {
    for (const auto& [k, v] : kv) {
        size_t p = 0;
        while ((p = tpl.find(k, p)) != std::string::npos) { tpl.replace(p, k.size(), v); p += v.size(); }
    }
    return tpl;
}

// ===================== converter ============================================

struct Converter {
    figo::FigmaUI* ui = nullptr;
    fs::path outDir, texDir;
    int superScale = 2;
    uint32_t curW = 0, curH = 0;

    struct Sprite {
        std::string file;  // basename, e.g. "icon_ab12cd34.png"
        std::string uuid;  // 36-char texture uuid (sprite-frame is uuid@f9941)
        int w = 0, h = 0;
        bool nine = false;
        int ml = 0, mr = 0, mt = 0, mb = 0;  // 9-slice borders (logical px)
    };
    std::map<uint64_t, Sprite> sprites;  // content hash -> sprite (global dedup)
    std::string whiteUuid;               // shared 1x1 white sprite-frame uuid

    // per-prefab object array + a unique-fileId counter
    json arr = json::array();
    uint64_t fileIdSeed = 0;
    int push(json obj) {
        int idx = (int)arr.size();
        arr.push_back(std::move(obj));
        return idx;
    }
    std::string nextFileId() { return compressUuid(makeUuid(fileIdSeed++)); }

    // ---- 9-slice shrink (identical to figo2godot) ----
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
        int cs = 0, ce = 0, s = 0;
        for (int x = 1; x < w; ++x) { if (colEq(x, x - 1)) { if (x - s > ce - cs) { cs = s; ce = x; } } else s = x; }
        int rs = 0, re = 0; s = 0;
        for (int y = 1; y < h; ++y) { if (rowEq(y, y - 1)) { if (y - s > re - rs) { rs = s; re = y; } } else s = y; }
        const bool cw9 = ce - cs >= scale, ch9 = re - rs >= scale;
        if (!cw9 && !ch9) return false;
        ml = cs / scale; mr = (w - 1 - ce) / scale; mt = rs / scale; mb = (h - 1 - re) / scale;
        const int nw = ml + 1 + mr, nh = mt + 1 + mb;
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

    struct Baked {
        bool ok = false;
        uint64_t hash = 0;
        float x = 0, y = 0;  // frame-absolute top-left of painted region (logical)
        int w = 0, h = 0;
        bool nine = false;
        int ml = 0, mr = 0, mt = 0, mb = 0;
    };

    // Bake a node to a deduped PNG; writes textures/<name>.png(+.meta) once per
    // unique content. Mirrors figo2godot::bake.
    Baked bake(const Node& n, int scale, bool flatten = false, bool tryNine = false, bool nineOnly = false) {
        Baked out;
        ui->setViewport(curW * scale, curH * scale);
        auto clone = figo::cloneNode(n, nullptr);
        if (!flatten) clone->children.clear();
        clone->opacity = 1.0f;
        clone->runtimeOpacity = -1.0f;
        clone->runtimeVisible = -1;
        clone->relativeTransform = n.absoluteTransform;

        std::vector<uint32_t> buf;
        uint32_t bw = 0, bh = 0;
        std::vector<Node*> one{clone.get()};
        if (!ui->renderer().renderOverlay(one, 0.0f, buf, bw, bh)) return out;

        int x0 = bw, y0 = bh, x1 = -1, y1 = -1;
        for (uint32_t y = 0; y < bh; ++y)
            for (uint32_t x = 0; x < bw; ++x)
                if ((buf[(size_t)y * bw + x] >> 24) & 0xff) {
                    if ((int)x < x0) x0 = x;
                    if ((int)x > x1) x1 = x;
                    if ((int)y < y0) y0 = y;
                    if ((int)y > y1) y1 = y;
                }
        if (x1 < x0 || y1 < y0) return out;
        int cw = x1 - x0 + 1, ch = y1 - y0 + 1;

        std::vector<uint32_t> crop((size_t)cw * ch);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                crop[(size_t)y * cw + x] = buf[(size_t)(y0 + y) * bw + x0 + x];

        if (tryNine && nineShrink(crop, cw, ch, scale, out.ml, out.mr, out.mt, out.mb)) out.nine = true;
        else if (nineOnly) return out;

        uint64_t h = 1469598103934665603ull;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(crop.data());
        for (size_t i = 0; i < crop.size() * 4; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }

        auto it = sprites.find(h);
        if (it == sprites.end()) {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "_%08x", (unsigned)(h & 0xffffffffu));
            std::string name = fileStem(n.name, "sprite") + suffix + ".png";
            if (!writePng(texDir / name, crop.data(), cw, ch)) return out;
            Sprite sp;
            sp.file = name;
            sp.uuid = makeUuid(h);
            sp.w = cw;
            sp.h = ch;
            sp.nine = out.nine;
            sp.ml = out.ml; sp.mr = out.mr; sp.mt = out.mt; sp.mb = out.mb;
            writeSpriteMeta(sp);
            it = sprites.emplace(h, sp).first;
        }

        const float sc = (float)scale;
        out.x = x0 / sc;
        out.y = y0 / sc;
        out.w = (int)std::lround(cw / sc);
        out.h = (int)std::lround(ch / sc);
        out.ok = true;
        out.hash = h;
        return out;
    }

    void writeSpriteMeta(const Sprite& sp) {
        std::string meta = subst(kSpriteFrameMeta, {
            {"$TEX", sp.uuid}, {"$NAME", sp.file.substr(0, sp.file.find('.'))},
            {"$W", std::to_string(sp.w)}, {"$H", std::to_string(sp.h)},
            {"$BT", std::to_string(sp.mt)}, {"$BB", std::to_string(sp.mb)},
            {"$BL", std::to_string(sp.ml)}, {"$BR", std::to_string(sp.mr)},
        });
        std::ofstream f(texDir / (sp.file + ".meta"), std::ios::binary);
        f << meta;
    }

    // The shared 1x1 white sprite-frame that all solid quads tint.
    void ensureWhite() {
        if (!whiteUuid.empty()) return;
        uint32_t px = 0xFFFFFFFFu;
        writePng(texDir / "white.png", &px, 1, 1);
        Sprite sp;
        sp.file = "white.png";
        sp.uuid = makeUuid(fnv1a("figo2cocos/white"));
        sp.w = sp.h = 1;
        writeSpriteMeta(sp);
        whiteUuid = sp.uuid;
    }

    // ---- component builders (Cocos 3.4.2+ field sets, from psd2prefab) ----
    json compBase(const char* type, int nodeIdx) {
        return json{
            {"__type__", type}, {"_name", ""}, {"_objFlags", 0},
            {"_enabled", true}, {"node", {{"__id__", nodeIdx}}}, {"_id", ""},
            {"__prefab", nullptr},  // patched to its CompPrefabInfo by addComp
        };
    }
    // Push a component, attach it to nodeIdx, and give it a CompPrefabInfo.
    void addComp(int nodeIdx, json comp) {
        int compIdx = push(std::move(comp));
        arr[nodeIdx]["_components"].push_back({{"__id__", compIdx}});
        int cpiIdx = push(json{{"__type__", "cc.CompPrefabInfo"}, {"fileId", nextFileId()}});
        arr[compIdx]["__prefab"] = {{"__id__", cpiIdx}};
    }

    json uiTransform(int nodeIdx, float w, float h) {
        json c = compBase("cc.UITransform", nodeIdx);
        c["_contentSize"] = ccSize(w, h);
        c["_anchorPoint"] = ccVec2(0.0f, 1.0f);  // top-left
        return c;
    }
    json spriteComp(int nodeIdx, const std::string& frameUuid, int type, int sizeMode, const json& color) {
        json c = compBase("cc.Sprite", nodeIdx);
        c["_srcBlendFactor"] = 2;
        c["_dstBlendFactor"] = 4;
        c["_spriteFrame"] = {{"__uuid__", frameUuid + "@f9941"}, {"__expectedType__", "cc.SpriteFrame"}};
        c["_type"] = type;          // 0 simple, 1 sliced
        c["_sizeMode"] = sizeMode;  // 0 custom, 1 trimmed
        c["_fillType"] = 0;
        c["_fillCenter"] = ccVec2(0, 0);
        c["_fillStart"] = 0;
        c["_fillRange"] = 0;
        c["_isTrimmedMode"] = true;
        c["_atlas"] = nullptr;
        c["_visFlags"] = 0;
        c["_customMaterial"] = nullptr;
        c["_color"] = color;
        c["_useGrayscale"] = false;
        return c;
    }

    void addLabel(int nodeIdx, const Node& n) {
        json c = compBase("cc.Label", nodeIdx);
        c["_srcBlendFactor"] = 2;
        c["_dstBlendFactor"] = 4;
        c["_string"] = n.characters;
        c["_fontSize"] = (int)(n.textStyle.fontSize + 0.5f);
        const float lineH = n.textStyle.lineHeightPx > 0 ? n.textStyle.lineHeightPx : n.textStyle.fontSize * 1.2f;
        c["_lineHeight"] = rnd(lineH);
        // Map figo autoResize -> Cocos Label overflow. NONE (0) makes the Label
        // shrink the node down to the text, which DISCARDS the authored box and
        // breaks alignment — a centered title collapses to the box's top-left
        // corner (anchor 0,1), so text no longer sits where the design put it.
        // Keep the box so _horizontalAlign/_verticalAlign place text as designed:
        //   WIDTH_AND_HEIGHT (hug both) -> NONE          (node == text)
        //   HEIGHT (fixed width, grow)  -> RESIZE_HEIGHT (keep width, wrap+grow)
        //   NONE / TRUNCATE (fixed box) -> CLAMP         (keep box, align, clip)
        const bool multiline = lineH > 0 && n.height > lineH * 1.8f;
        const std::string& ar = n.textStyle.autoResize;
        int overflow;
        bool wrap;
        if (ar == "WIDTH_AND_HEIGHT") { overflow = 0; wrap = false; }
        else if (ar == "HEIGHT")      { overflow = 3; wrap = true; }
        else                          { overflow = 1; wrap = multiline; }
        c["_enableWrapText"] = wrap;
        c["_isSystemFontUsed"] = true;
        c["_spacingX"] = 0;
        c["_underlineHeight"] = 0;
        c["_visFlags"] = 0;
        c["_customMaterial"] = nullptr;
        figo::Color col{0, 0, 0, 1};
        if (const auto* f = solidFill(n)) { col = f->color; col.a *= f->opacity; }
        c["_color"] = ccColor(col);
        c["_overflow"] = overflow;
        c["_cacheMode"] = 0;
        int ha = 1;  // CENTER
        switch (n.textStyle.alignH) {
            case figo::TextStyle::AlignH::Left: ha = 0; break;
            case figo::TextStyle::AlignH::Right: ha = 2; break;
            case figo::TextStyle::AlignH::Justified: ha = 0; break;
            default: ha = 1; break;
        }
        int va = 1;  // CENTER
        switch (n.textStyle.alignV) {
            case figo::TextStyle::AlignV::Top: va = 0; break;
            case figo::TextStyle::AlignV::Bottom: va = 2; break;
            default: va = 1; break;
        }
        c["_horizontalAlign"] = ha;
        c["_verticalAlign"] = va;
        c["_actualFontSize"] = (int)(n.textStyle.fontSize + 0.5f);
        c["_isItalic"] = n.textStyle.italic;
        c["_isBold"] = n.textStyle.fontWeight >= 600;
        c["_isUnderline"] = false;
        addComp(nodeIdx, std::move(c));
    }

    // Build a cc.Node skeleton (no components/children yet) at local (lx, ly)
    // top-left relative to parent top-left, with the given content size.
    int newNode(const std::string& name, int parentIdx, float lx, float ly, float w, float h, const Node* src) {
        json node = {
            {"__type__", "cc.Node"}, {"_name", name}, {"_objFlags", 0},
            {"_parent", parentIdx < 0 ? json(nullptr) : json{{"__id__", parentIdx}}},
            {"_children", json::array()}, {"_active", true},
            {"_components", json::array()}, {"_prefab", nullptr}, {"_id", ""},
            {"_lpos", ccVec3(lx, -ly, 0)}, {"_lrot", ccVec3(0, 0, 0)},
            {"_lscale", ccVec3(1, 1, 1)}, {"_euler", ccVec3(0, 0, 0)},
            {"_layer", 33554432},  // UI_2D
        };
        if (src && !src->visible) node["_active"] = false;
        int idx = push(std::move(node));
        addComp(idx, uiTransform(idx, w, h));  // every UI node carries a UITransform
        if (src && src->opacity < 0.999f) {
            json op = compBase("cc.UIOpacity", idx);
            op["_opacity"] = u8(src->opacity);
            addComp(idx, std::move(op));
        }
        return idx;
    }

    void finishNode(int idx) {
        // Each node gets a PrefabInfo pointing back at the prefab root (idx 1).
        int piIdx = push(json{
            {"__type__", "cc.PrefabInfo"}, {"root", {{"__id__", 1}}},
            {"asset", {{"__id__", 0}}}, {"fileId", nextFileId()}, {"sync", false},
        });
        arr[idx]["_prefab"] = {{"__id__", piIdx}};
    }

    // Emit node `n` under parentIdx; returns its array index. pax/pay = parent's
    // absolute top-left (for placing baked sprites in parent-local coords).
    int emit(Node& n, int parentIdx, const std::string& name, float pax, float pay) {
        const bool isRoot = parentIdx < 0;
        const float lx = isRoot ? 0.0f : n.relativeTransform.m02;
        const float ly = isRoot ? 0.0f : n.relativeTransform.m12;

        // Place baked leaves at their painted bounds; everything else at its box.
        auto emitBakedLeaf = [&](const Baked& b) -> int {
            int idx = newNode(name, parentIdx, b.x - pax, b.y - pay, (float)b.w, (float)b.h, &n);
            const Sprite& sp = sprites[b.hash];
            addComp(idx, spriteComp(idx, sp.uuid, b.nine ? 1 : 0, b.nine ? 0 : 1, ccColor({1, 1, 1, 1})));
            finishNode(idx);
            return idx;
        };

        if (n.type == NodeType::Text) {
            int idx = newNode(name, parentIdx, lx, ly, n.width, n.height, &n);
            addLabel(idx, n);
            finishNode(idx);
            return idx;
        }

        if (!isRoot && isVectorIcon(n)) {
            Baked b = bake(n, superScale, /*flatten=*/true);
            if (b.ok) return emitBakedLeaf(b);
            int idx = newNode(name, parentIdx, lx, ly, n.width, n.height, &n);
            finishNode(idx);
            return idx;  // no recurse into the (failed) flatten
        }

        const bool isContainer = !n.children.empty();
        if (!isContainer && needsBake(n)) {
            Baked b;
            if (nineCandidate(n)) b = bake(n, 1, false, true, true);
            if (!b.ok) b = bake(n, superScale);
            if (b.ok) return emitBakedLeaf(b);
            int idx = newNode(name, parentIdx, lx, ly, n.width, n.height, &n);
            finishNode(idx);
            return idx;
        }

        // Container or solid leaf: a node at its own box, optional bg + children.
        int idx = newNode(name, parentIdx, lx, ly, n.width, n.height, &n);
        if (needsBake(n)) {
            // Complex background: a baked sprite child filling (or overhanging) the box.
            Baked b;
            if (nineCandidate(n)) b = bake(n, 1, false, true, true);
            if (!b.ok) b = bake(n, superScale);
            if (b.ok) {
                const Sprite& sp = sprites[b.hash];
                if (b.nine) {
                    // Stretchable bg: sprite on the node itself, sliced to the box.
                    addComp(idx, spriteComp(idx, sp.uuid, 1, 0, ccColor({1, 1, 1, 1})));
                } else {
                    // Fixed bg (may overhang via glow): a sized __bg child.
                    float bx = b.x - n.absoluteTransform.m02, by = b.y - n.absoluteTransform.m12;
                    int bg = newNode("__bg", idx, bx, by, (float)b.w, (float)b.h, nullptr);
                    addComp(bg, spriteComp(bg, sp.uuid, 0, 1, ccColor({1, 1, 1, 1})));
                    finishNode(bg);
                    arr[idx]["_children"].push_back({{"__id__", bg}});
                }
            }
        } else if (const auto* f = solidFill(n)) {
            ensureWhite();
            addComp(idx, spriteComp(idx, whiteUuid, 0, 0, ccPaintColor(*f)));
        }

        // Children, names unique among siblings.
        std::map<std::string, int> used;
        used["__bg"] = 1;
        int ci = 0;
        for (auto& child : n.children) {
            std::string fb = "Node" + std::to_string(ci++);
            std::string base = sanitizeName(child->name, fb.c_str());
            std::string uniq = base;
            int k = 2;
            while (used.count(uniq)) uniq = base + "_" + std::to_string(k++);
            used[uniq] = 1;
            int childIdx = emit(*child, idx, uniq, n.absoluteTransform.m02, n.absoluteTransform.m12);
            arr[idx]["_children"].push_back({{"__id__", childIdx}});
        }
        finishNode(idx);
        return idx;
    }

    static constexpr float kNineMinArea = 40000.0f;  // ~200x200 logical
    bool nineCandidate(const Node& n) {
        if (!isRectish(n.type)) return false;
        if (hasVisibleEffect(n)) return false;
        if (n.width * n.height < kNineMinArea) return false;
        return true;
    }

    void convertFrame(Node* frame) {
        curW = (uint32_t)std::ceil(std::max(1.0f, frame->width));
        curH = (uint32_t)std::ceil(std::max(1.0f, frame->height));
        ui->setViewport(curW, curH);
        if (!ui->selectFrame(frame->name)) {
            std::fprintf(stderr, "  WARN: selectFrame(%s) failed\n", frame->name.c_str());
            return;
        }
        ui->render();  // computes absoluteTransform

        arr = json::array();
        fileIdSeed = fnv1a(frame->name) | 1;
        std::string stem = fileStem(frame->name, "Frame");

        // idx 0 = cc.Prefab (data -> root node at idx 1).
        push(json{
            {"__type__", "cc.Prefab"}, {"_name", ""}, {"_objFlags", 0},
            {"_native", ""}, {"data", {{"__id__", 1}}},
            {"optimizationPolicy", 0}, {"asyncLoadAssets", false}, {"persistent", false},
        });
        emit(*frame, -1, stem, 0.0f, 0.0f);  // root node pushed at idx 1

        std::ofstream pf(outDir / (stem + ".prefab"), std::ios::binary);
        pf << arr.dump(2);
        std::ofstream mf(outDir / (stem + ".prefab.meta"), std::ios::binary);
        mf << subst(kPrefabMeta, {{"$UUID", makeUuid(fnv1a("prefab/" + frame->name))}, {"$NAME", stem}});
        std::printf("  %s -> %s.prefab (%zu objects)\n", frame->name.c_str(), stem.c_str(), arr.size());
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: figo2cocos <input.fig|canvas.json> [outDir] [--frame NAME] [--scale N]\n");
        return 2;
    }
    const std::string input = argv[1];
    fs::path outDir = "cocos_out";
    std::string onlyFrame;
    int scale = 2;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frame" && i + 1 < argc) onlyFrame = argv[++i];
        else if (a == "--scale" && i + 1 < argc) scale = std::max(1, atoi(argv[++i]));
        else if (!a.empty() && a[0] != '-') outDir = a;
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
    fs::path texDir = outDir / "textures";
    fs::create_directories(texDir, ec);

    auto frames = ui->document().topLevelFrames();
    if (frames.empty()) {
        std::fprintf(stderr, "FAIL: no top-level frames\n");
        return 1;
    }

    Converter cv;
    cv.ui = ui.get();
    cv.outDir = outDir;
    cv.texDir = texDir;
    cv.superScale = scale;

    int n = 0;
    for (Node* frame : frames) {
        if (!onlyFrame.empty() && frame->name != onlyFrame) continue;
        cv.convertFrame(frame);
        ++n;
    }

    std::printf("RESULT: OK, %d prefab(s), %zu unique sprite(s)\n", n, cv.sprites.size());
    return 0;
}
