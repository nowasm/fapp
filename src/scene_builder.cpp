#include "scene_builder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "svg_path.h"

namespace figo {

namespace {

constexpr float kPi = 3.14159265358979323846f;

tvg::Matrix toTvg(const Mat23& m) {
    return {m.m00, m.m01, m.m02, m.m10, m.m11, m.m12, 0, 0, 1};
}

uint8_t channel(float v) {
    return static_cast<uint8_t>(std::lround(std::fmax(0.0f, std::fmin(1.0f, v)) * 255.0f));
}

// Rounded rectangle with independent corner radii (TL, TR, BR, BL), using
// the circle-arc cubic approximation constant.
void appendRoundedRect(tvg::Shape& shape, float w, float h, float tl, float tr, float br,
                       float bl) {
    const float maxR = std::fmin(w, h) * 0.5f;
    tl = std::fmin(tl, maxR);
    tr = std::fmin(tr, maxR);
    br = std::fmin(br, maxR);
    bl = std::fmin(bl, maxR);

    if (tl <= 0 && tr <= 0 && br <= 0 && bl <= 0) {
        shape.appendRect(0, 0, w, h);
        return;
    }
    constexpr float k = 0.5522847498f;  // 4/3 * (sqrt(2) - 1)
    shape.moveTo(tl, 0);
    shape.lineTo(w - tr, 0);
    if (tr > 0) shape.cubicTo(w - tr + k * tr, 0, w, tr - k * tr, w, tr);
    shape.lineTo(w, h - br);
    if (br > 0) shape.cubicTo(w, h - br + k * br, w - br + k * br, h, w - br, h);
    shape.lineTo(bl, h);
    if (bl > 0) shape.cubicTo(bl - k * bl, h, 0, h - bl + k * bl, 0, h - bl);
    shape.lineTo(0, tl);
    if (tl > 0) shape.cubicTo(0, tl - k * tl, tl - k * tl, 0, tl, 0);
    shape.close();
}

void cornerRadii(const Node& n, float& tl, float& tr, float& br, float& bl) {
    if (n.rectangleCornerRadii) {
        tl = (*n.rectangleCornerRadii)[0];
        tr = (*n.rectangleCornerRadii)[1];
        br = (*n.rectangleCornerRadii)[2];
        bl = (*n.rectangleCornerRadii)[3];
    } else {
        tl = tr = br = bl = n.cornerRadius;
    }
}

// Responsive layout can change a node's size away from the authored one, but
// fillGeometry/strokeGeometry paths stay in authored local coordinates. This
// reports the scale to apply to stored path data so it tracks the live size.
void geometryScale(const Node& n, float& sx, float& sy) {
    sx = (n.baseWidth > 0 && n.width != n.baseWidth) ? n.width / n.baseWidth : 1.0f;
    sy = (n.baseHeight > 0 && n.height != n.baseHeight) ? n.height / n.baseHeight : 1.0f;
}

// Primitive-shaped nodes are regenerated from width/height when resized
// (keeps corner radii crisp instead of stretching the stored path).
bool regenerateAsPrimitive(const Node& n) {
    if (n.width == n.baseWidth && n.height == n.baseHeight) return false;
    switch (n.type) {
    case NodeType::Rectangle:
    case NodeType::Ellipse:
    case NodeType::Line:
    case NodeType::Frame:
    case NodeType::Component:
    case NodeType::Instance:
    case NodeType::Section:
        return true;
    default:
        return false;
    }
}

void appendPrimitive(tvg::Shape& shape, const Node& n);

// Node outline in live coordinates: the stored fill geometry (scaled to the
// current size) unless the node regenerates as a primitive.
void appendOutline(tvg::Shape& shape, const Node& n) {
    float gsx, gsy;
    geometryScale(n, gsx, gsy);
    if (!n.fillGeometry.empty() && !regenerateAsPrimitive(n)) {
        for (const auto& geom : n.fillGeometry) appendSvgPath(shape, geom.path.c_str(), gsx, gsy);
    } else {
        appendPrimitive(shape, n);
    }
}

// Appends the node's outline (primitive form) onto a shape.
void appendPrimitive(tvg::Shape& shape, const Node& n) {
    switch (n.type) {
    case NodeType::Ellipse:
        shape.appendCircle(n.width * 0.5f, n.height * 0.5f, n.width * 0.5f, n.height * 0.5f);
        break;
    case NodeType::Line:
        shape.moveTo(0, 0);
        shape.lineTo(n.width, n.height);
        break;
    default: {
        float tl, tr, br, bl;
        cornerRadii(n, tl, tr, br, bl);
        appendRoundedRect(shape, n.width, n.height, tl, tr, br, bl);
        break;
    }
    }
}

// Angular (conic) and diamond gradients have no ThorVG primitive. Rasterize
// the gradient field into an RGBA buffer in the basis spanned by the Figma
// handles (handle0 = center, handle1 = 0° axis, handle2 = 90° axis) and fill
// the node with it as a picture; the caller clips it to the node outline.
tvg::Picture* makeProceduralGradient(const Paint& p, const Node& n) {
    if (p.stops.empty() || n.width < 1 || n.height < 1) return nullptr;

    const float cx = p.handle0[0], cy = p.handle0[1];
    const float ux = p.handle1[0] - cx, uy = p.handle1[1] - cy;
    const float vx = p.handle2[0] - cx, vy = p.handle2[1] - cy;
    const float det = ux * vy - vx * uy;
    if (std::fabs(det) < 1e-6f) return nullptr;  // degenerate handles → radial fallback

    // 256-entry color LUT over t ∈ [0,1] (straight alpha, memory order RGBA).
    uint32_t lut[256];
    {
        auto pack = [&](const Color& c) {
            const uint8_t a = static_cast<uint8_t>(channel(c.a) * p.opacity);
            return static_cast<uint32_t>(channel(c.r)) |
                   static_cast<uint32_t>(channel(c.g)) << 8 |
                   static_cast<uint32_t>(channel(c.b)) << 16 | static_cast<uint32_t>(a) << 24;
        };
        size_t si = 0;
        for (int i = 0; i < 256; ++i) {
            const float t = i / 255.0f;
            while (si + 1 < p.stops.size() && p.stops[si + 1].position < t) ++si;
            if (t <= p.stops.front().position) {
                lut[i] = pack(p.stops.front().color);
            } else if (si + 1 >= p.stops.size() || t >= p.stops.back().position) {
                lut[i] = pack(p.stops.back().color);
            } else {
                const auto& a = p.stops[si];
                const auto& b = p.stops[si + 1];
                const float span = b.position - a.position;
                const float f = span > 0 ? (t - a.position) / span : 0;
                Color c;
                c.r = a.color.r + (b.color.r - a.color.r) * f;
                c.g = a.color.g + (b.color.g - a.color.g) * f;
                c.b = a.color.b + (b.color.b - a.color.b) * f;
                c.a = a.color.a + (b.color.a - a.color.a) * f;
                lut[i] = pack(c);
            }
        }
    }

    constexpr float kTau = 6.28318530717958647692f;
    const uint32_t w = static_cast<uint32_t>(std::fmin(1024.0f, std::fmax(2.0f, n.width)));
    const uint32_t h = static_cast<uint32_t>(std::fmin(1024.0f, std::fmax(2.0f, n.height)));
    std::vector<uint32_t> buf(static_cast<size_t>(w) * h);
    const bool angular = p.type == PaintType::GradientAngular;
    for (uint32_t y = 0; y < h; ++y) {
        const float py = (y + 0.5f) / h - cy;
        for (uint32_t x = 0; x < w; ++x) {
            const float px = (x + 0.5f) / w - cx;
            // Coordinates in the handle basis.
            const float u = (vy * px - vx * py) / det;
            const float v = (-uy * px + ux * py) / det;
            float t;
            if (angular) {
                t = std::atan2(v, u) / kTau;
                t -= std::floor(t);
            } else {
                t = std::fmin(1.0f, std::fabs(u) + std::fabs(v));
            }
            buf[static_cast<size_t>(y) * w + x] =
                lut[static_cast<int>(t * 255.0f + 0.5f)];
        }
    }

    auto* pic = tvg::Picture::gen();
    if (pic->load(buf.data(), w, h, tvg::ColorSpace::ABGR8888S, true /*copy*/) !=
        tvg::Result::Success) {
        tvg::Paint::rel(pic);
        return nullptr;
    }
    pic->size(n.width, n.height);
    return pic;
}

tvg::Fill* makeGradient(const Paint& p, float w, float h) {
    const float x0 = p.handle0[0] * w, y0 = p.handle0[1] * h;
    const float x1 = p.handle1[0] * w, y1 = p.handle1[1] * h;

    tvg::Fill* fill = nullptr;
    if (p.type == PaintType::GradientLinear) {
        auto* g = tvg::LinearGradient::gen();
        g->linear(x0, y0, x1, y1);
        fill = g;
    } else {
        // Radial — also the fallback for angular/diamond gradients.
        auto* g = tvg::RadialGradient::gen();
        const float r = std::hypot(x1 - x0, y1 - y0);
        g->radial(x0, y0, std::fmax(r, 0.01f), x0, y0, 0);
        fill = g;
    }

    std::vector<tvg::Fill::ColorStop> stops;
    stops.reserve(p.stops.size());
    for (const auto& s : p.stops) {
        stops.push_back({s.position, channel(s.color.r), channel(s.color.g),
                         channel(s.color.b),
                         static_cast<uint8_t>(channel(s.color.a) * p.opacity)});
    }
    if (!stops.empty()) fill->colorStops(stops.data(), static_cast<uint32_t>(stops.size()));
    return fill;
}

void applyFill(tvg::Shape& shape, const Paint& p, const Node& n) {
    if (p.type == PaintType::Solid) {
        shape.fill(channel(p.color.r), channel(p.color.g), channel(p.color.b),
                   static_cast<uint8_t>(channel(p.color.a) * p.opacity));
    } else {
        shape.fill(makeGradient(p, n.width, n.height));
    }
}

void applyStrokeStyle(tvg::Shape& shape, const Node& n) {
    shape.strokeWidth(n.strokeWeight);
    if (!n.strokeDashes.empty()) {
        shape.strokeDash(n.strokeDashes.data(), static_cast<uint32_t>(n.strokeDashes.size()));
    }
    shape.strokeCap(n.strokeCap == "ROUND"    ? tvg::StrokeCap::Round
                    : n.strokeCap == "SQUARE" ? tvg::StrokeCap::Square
                                              : tvg::StrokeCap::Butt);
    shape.strokeJoin(n.strokeJoin == "ROUND"   ? tvg::StrokeJoin::Round
                     : n.strokeJoin == "BEVEL" ? tvg::StrokeJoin::Bevel
                                               : tvg::StrokeJoin::Miter);
}

tvg::Picture* makeImage(const Paint& p, const Node& n, const BuildContext& ctx) {
    if (ctx.imageDir.empty() || p.imageRef.empty()) return nullptr;

    auto* pic = tvg::Picture::gen();
    bool ok = false;
    const bool hasExt = p.imageRef.find('.') != std::string::npos;
    if (hasExt) {
        ok = pic->load((ctx.imageDir + "/" + p.imageRef).c_str()) == tvg::Result::Success;
    } else {
        for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"}) {
            if (pic->load((ctx.imageDir + "/" + p.imageRef + ext).c_str()) ==
                tvg::Result::Success) {
                ok = true;
                break;
            }
        }
    }
    if (!ok) {
        tvg::Paint::rel(pic);
        return nullptr;
    }

    float iw = 0, ih = 0;
    pic->size(&iw, &ih);
    if (iw <= 0 || ih <= 0) {
        tvg::Paint::rel(pic);
        return nullptr;
    }

    if (p.imageScaleMode == "FIT") {
        const float s = std::fmin(n.width / iw, n.height / ih);
        pic->size(iw * s, ih * s);
        pic->translate((n.width - iw * s) * 0.5f, (n.height - ih * s) * 0.5f);
    } else if (p.imageScaleMode == "STRETCH" || p.imageScaleMode == "TILE") {
        pic->size(n.width, n.height);  // TILE approximated as stretch
    } else {  // FILL (default): cover and center-crop
        const float s = std::fmax(n.width / iw, n.height / ih);
        pic->size(iw * s, ih * s);
        pic->translate((n.width - iw * s) * 0.5f, (n.height - ih * s) * 0.5f);
    }
    return pic;
}

// Shapes for one paint over the node's fill geometry (or primitive outline).
void pushFillPaint(tvg::Scene& scene, const Paint& p, const Node& n,
                   const BuildContext& ctx) {
    if (!p.visible || p.opacity <= 0) return;

    if (p.type == PaintType::Image) {
        if (auto* pic = makeImage(p, n, ctx)) {
            // Clip the image to the node outline.
            auto* clip = tvg::Shape::gen();
            appendPrimitive(*clip, n);
            pic->clip(clip);
            pic->opacity(static_cast<uint8_t>(std::lround(p.opacity * 255)));
            scene.add(pic);
        }
        return;
    }

    // Angular/diamond gradients: procedural raster clipped to the outline
    // (no ThorVG primitive); on failure fall through to the radial fallback.
    if (p.type == PaintType::GradientAngular || p.type == PaintType::GradientDiamond) {
        if (auto* pic = makeProceduralGradient(p, n)) {
            auto* clip = tvg::Shape::gen();
            appendOutline(*clip, n);
            pic->clip(clip);
            scene.add(pic);
            return;
        }
    }

    if (!n.fillGeometry.empty() && !regenerateAsPrimitive(n)) {
        float gsx, gsy;
        geometryScale(n, gsx, gsy);
        for (const auto& geom : n.fillGeometry) {
            auto* shape = tvg::Shape::gen();
            if (!appendSvgPath(*shape, geom.path.c_str(), gsx, gsy)) {
                tvg::Paint::rel(shape);
                continue;
            }
            shape->fillRule(geom.evenOdd ? tvg::FillRule::EvenOdd : tvg::FillRule::NonZero);
            applyFill(*shape, p, n);
            scene.add(shape);
        }
    } else if (n.width > 0 || n.height > 0) {
        auto* shape = tvg::Shape::gen();
        appendPrimitive(*shape, n);
        applyFill(*shape, p, n);
        scene.add(shape);
    }
}

void pushStrokePaint(tvg::Scene& scene, const Paint& p, const Node& n) {
    if (!p.visible || p.opacity <= 0 || p.type == PaintType::Image) return;

    const bool resized = n.width != n.baseWidth || n.height != n.baseHeight;
    float gsx, gsy;
    geometryScale(n, gsx, gsy);

    // Pre-outlined stroke paths bake the stroke width into the geometry, so
    // scaling them would distort the weight — fall back to live stroking when
    // the node was resized by layout.
    if (!n.strokeGeometry.empty() && !resized) {
        // Figma pre-outlines strokes (handles inside/outside alignment); fill them.
        for (const auto& geom : n.strokeGeometry) {
            auto* shape = tvg::Shape::gen();
            if (!appendSvgPath(*shape, geom.path.c_str())) {
                tvg::Paint::rel(shape);
                continue;
            }
            shape->fillRule(geom.evenOdd ? tvg::FillRule::EvenOdd : tvg::FillRule::NonZero);
            applyFill(*shape, p, n);
            scene.add(shape);
        }
        return;
    }

    if (n.strokeWeight <= 0) return;

    // Open paths (no Z command anywhere) have no meaningful inside/outside —
    // Figma strokes them centered regardless of strokeAlign, and the clip
    // trick would cut the stroke along the chord-closed region.
    bool openPath = !n.fillGeometry.empty();
    for (const auto& geom : n.fillGeometry) {
        if (geom.path.find('Z') != std::string::npos ||
            geom.path.find('z') != std::string::npos) {
            openPath = false;
            break;
        }
    }
    const StrokeAlign align = openPath ? StrokeAlign::Center : n.strokeAlign;

    // No outline geometry: stroke the node outline. ThorVG strokes are
    // centered, so emulate Figma's alignment: INSIDE = double width clipped
    // to the outline, OUTSIDE = double width with the outline masked away.
    auto outlineShape = [&]() {
        auto* s = tvg::Shape::gen();
        if (!n.fillGeometry.empty() && !regenerateAsPrimitive(n)) {
            for (const auto& geom : n.fillGeometry)
                appendSvgPath(*s, geom.path.c_str(), gsx, gsy);
        } else {
            appendPrimitive(*s, n);
        }
        return s;
    };

    auto addStroked = [&](tvg::Shape* shape) {
        applyStrokeStyle(*shape, n);
        if (p.type == PaintType::Solid) {
            shape->strokeFill(channel(p.color.r), channel(p.color.g), channel(p.color.b),
                              static_cast<uint8_t>(channel(p.color.a) * p.opacity));
        } else {
            shape->strokeFill(makeGradient(p, n.width, n.height));
        }
        if (align == StrokeAlign::Inside) {
            shape->strokeWidth(n.strokeWeight * 2.0f);
            shape->clip(outlineShape());
        } else if (align == StrokeAlign::Outside) {
            shape->strokeWidth(n.strokeWeight * 2.0f);
            auto* mask = outlineShape();
            mask->fill(255, 255, 255, 255);
            shape->mask(mask, tvg::MaskMethod::InvAlpha);
        }
        scene.add(shape);
    };

    if (!n.fillGeometry.empty() && !regenerateAsPrimitive(n)) {
        for (const auto& geom : n.fillGeometry) {
            auto* shape = tvg::Shape::gen();
            if (!appendSvgPath(*shape, geom.path.c_str(), gsx, gsy)) {
                tvg::Paint::rel(shape);
                continue;
            }
            addStroked(shape);
        }
    } else {
        auto* shape = tvg::Shape::gen();
        appendPrimitive(*shape, n);
        addStroked(shape);
    }
}

// UTF-8 byte length of the codepoint starting at s[i].
size_t cpLen(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
}

// Sum of glyph advances for a UTF-8 string at the text's current font/size.
// Unlike ink bounds, this keeps trailing-space widths, which matters when
// laying rich-text runs end to end.
float measureAdvance(tvg::Text& text, const std::string& s, int& glyphs) {
    float total = 0;
    glyphs = 0;
    char one[5] = {0};
    for (size_t i = 0; i < s.size();) {
        const size_t len = std::min(cpLen(s, i), s.size() - i);
        if (s[i] == '\n') {
            ++i;
            continue;
        }
        std::memcpy(one, s.data() + i, len);
        one[len] = 0;
        tvg::GlyphMetrics gm{};
        if (text.metrics(one, gm) == tvg::Result::Success && gm.advance > 0) {
            total += gm.advance;
            ++glyphs;
        }
        i += len;
    }
    return total;
}

// Picks a font key able to render `s`: the requested key unless a non-ASCII
// codepoint has no glyph in it and a system fallback covers that codepoint —
// then the whole string switches to the fallback (which also carries Latin).
std::string keyForText(BuildContext& ctx, const std::string& key, const std::string& s,
                       int weight, bool italic) {
    for (size_t i = 0; i < s.size();) {
        const size_t len = std::min(cpLen(s, i), s.size() - i);
        if (len > 1) {
            unsigned long cp = static_cast<unsigned char>(s[i]) &
                               (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            for (size_t k = 1; k < len; ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            if (!ctx.fonts->hasGlyph(key, cp)) {
                const std::string fb = ctx.fonts->fallbackFontFor(cp, weight, italic);
                if (!fb.empty()) return fb;
                return key;
            }
        }
        i += len;
    }
    return key;
}

// One wrap-atomic token of flowed text: a word, a run of spaces, a single
// CJK character, or a forced newline.
struct TextPiece {
    tvg::Text* text = nullptr;  // null → spaces (advance only) or newline
    float width = 0;
    float ascent = 0, fontH = 0, natLineH = 0;
    bool space = false, newline = false;
    // Source range in Node::characters plus the resolved font, so caret
    // placement can re-measure a prefix of the token.
    int srcStart = -1, srcEnd = -1;
    std::string fontKey;
    float fontPt = 0;          // tvg point size (px × 72/96)
    float letterFactor = 1.0f;
};

// Result of tokenizing + measuring + greedily wrapping a text node. Shared
// by rendering (makeRichText) and pure measurement (measureTextNode), so the
// box a layout pass computes always matches what gets drawn.
struct TextFlow {
    std::vector<TextPiece> pieces;
    struct Line {
        std::vector<TextPiece*> ps;
        float width = 0;
        int srcStart = -1, srcEnd = -1;  // source byte span (incl. trailing spaces)
    };
    std::vector<Line> lines;
    std::vector<float> lineHs;  // line-box heights (Figma line-box model)
    float contentH = 0;         // sum of the line boxes
    float maxLineW = 0;         // widest line extent

    void release() {
        for (auto& p : pieces) tvg::Paint::rel(p.text);
        pieces.clear();
    }
};

// Tokenizes the node's text runs (or a synthetic run with the base style when
// the node has none), measures every token, and flows them into lines wrapped
// at maxW (<= 0 → never wrap). With keepTexts the non-space pieces carry
// ready-to-place tvg::Text objects, otherwise only metrics survive. Returns
// false when a needed font is missing.
bool buildTextFlow(const Node& n, float maxW, BuildContext& ctx, bool keepTexts,
                   TextFlow& flow) {
    if (!ctx.fonts || n.characters.empty()) return false;
    const TextStyle& base = n.textStyle;

    // Node-level fill as the default run color.
    Color baseColor{0, 0, 0, 1};
    for (const auto& p : n.fills) {
        if (p.visible && p.opacity > 0 && p.type == PaintType::Solid) {
            baseColor = p.color;
            baseColor.a *= p.opacity;
            break;
        }
    }

    std::vector<TextRun> synth;
    const std::vector<TextRun>* runs = &n.textRuns;
    if (runs->empty()) {
        TextRun r;
        r.start = 0;
        r.end = static_cast<int>(n.characters.size());
        r.style = base;
        synth.push_back(r);
        runs = &synth;
    }

    using Piece = TextPiece;
    auto& pieces = flow.pieces;
    auto bail = [&]() -> bool {
        flow.release();
        return false;
    };

    // CJK and fullwidth ranges break per character.
    auto cjk = [](unsigned long cp) {
        return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
               (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF);
    };
    auto decode = [](const std::string& s, size_t i, size_t len) -> unsigned long {
        unsigned long cp = static_cast<unsigned char>(s[i]);
        if (len == 2) cp &= 0x1F;
        else if (len == 3) cp &= 0x0F;
        else if (len == 4) cp &= 0x07;
        for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        return cp;
    };

    // Tokenize each run and measure every token with its own styled Text.
    for (const auto& run : *runs) {
        const std::string s =
            n.characters.substr(run.start, static_cast<size_t>(run.end - run.start));
        if (s.empty()) continue;
        const std::string key = ctx.fonts->fontKeyFor(run.style.fontFamily,
                                                      run.style.fontWeight, run.style.italic);
        if (key.empty()) return bail();

        auto makeToken = [&](const std::string& token, bool isSpace, size_t srcOff) -> bool {
            const std::string tokenKey =
                keyForText(ctx, key, token, run.style.fontWeight, run.style.italic);
            auto* t = tvg::Text::gen();
            if (t->font(tokenKey.c_str()) != tvg::Result::Success) {
                tvg::Paint::rel(t);
                return false;
            }
            t->size(run.style.fontSize * 72.0f / 96.0f);
            t->text(token.c_str());
            if (run.style.italic) t->italic();

            tvg::TextMetrics m{};
            Piece p;
            if (t->metrics(m) == tvg::Result::Success) {
                p.ascent = m.ascent;
                p.fontH = m.ascent - m.descent;
                p.natLineH = m.advance > 0 ? m.advance : p.fontH;
            }
            int glyphs = 0;
            float width = measureAdvance(*t, token, glyphs);
            float letterFactor = 1.0f;
            if (run.style.letterSpacing != 0 && glyphs > 0 && width > 0) {
                letterFactor = std::fmax(
                    0.0f,
                    1.0f + run.style.letterSpacing / (width / static_cast<float>(glyphs)));
                t->spacing(letterFactor, 1.0f);
            }
            p.width = width * letterFactor;
            p.space = isSpace;
            p.srcStart = run.start + static_cast<int>(srcOff);
            p.srcEnd = p.srcStart + static_cast<int>(token.size());
            p.fontKey = tokenKey;
            p.fontPt = run.style.fontSize * 72.0f / 96.0f;
            p.letterFactor = letterFactor;
            if (isSpace) {
                tvg::Paint::rel(t);  // spaces only contribute advance
            } else {
                if (glyphs == 0) {
                    tvg::Paint::rel(t);
                    return false;
                }
                if (keepTexts) {
                    const Color c = run.color ? *run.color : baseColor;
                    t->fill(channel(c.r), channel(c.g), channel(c.b));
                    t->opacity(channel(c.a));
                    p.text = t;
                } else {
                    tvg::Paint::rel(t);  // measurement only needs the metrics
                }
            }
            pieces.push_back(p);
            return true;
        };

        size_t i = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '\n') {
                Piece nl;
                nl.newline = true;
                nl.srcStart = run.start + static_cast<int>(i);
                nl.srcEnd = nl.srcStart + 1;
                pieces.push_back(nl);
                ++i;
                continue;
            }
            if (c == ' ') {
                size_t e = i;
                while (e < s.size() && s[e] == ' ') ++e;
                if (!makeToken(s.substr(i, e - i), true, i)) return bail();
                i = e;
                continue;
            }
            const size_t len = std::min(cpLen(s, i), s.size() - i);
            const unsigned long cp = decode(s, i, len);
            if (cjk(cp)) {
                if (!makeToken(s.substr(i, len), false, i)) return bail();
                i += len;
                continue;
            }
            // Word: consume until space, newline, or CJK.
            size_t e = i;
            while (e < s.size() && s[e] != ' ' && s[e] != '\n') {
                const size_t l = std::min(cpLen(s, e), s.size() - e);
                if (cjk(decode(s, e, l))) break;
                e += l;
            }
            if (!makeToken(s.substr(i, e - i), false, i)) return bail();
            i = e;
        }
    }
    if (pieces.empty()) return false;

    // Greedy line flow.
    using Line = TextFlow::Line;
    auto& lines = flow.lines;
    lines.resize(1);
    auto lineEnd = [&](Line& ln) {  // drop trailing spaces from the extent
        while (!ln.ps.empty() && ln.ps.back()->space) {
            ln.width -= ln.ps.back()->width;
            ln.ps.pop_back();
        }
    };
    // Source coverage per line (drives caret placement; includes pieces that
    // don't render, like dropped or trailing spaces).
    auto cover = [](Line& ln, const Piece& p) {
        if (ln.srcStart < 0) ln.srcStart = p.srcStart;
        ln.srcEnd = std::max(ln.srcEnd, p.srcEnd);
    };
    bool wrapped = false;  // last break was a wrap (drop the leading spaces)
    for (auto& p : pieces) {
        Line* cur = &lines.back();
        if (p.newline) {
            cover(*cur, p);  // caret right before the \n sits on this line
            lineEnd(*cur);
            lines.emplace_back();
            lines.back().srcStart = lines.back().srcEnd = p.srcEnd;
            wrapped = false;
            continue;
        }
        if (p.space) {
            cover(*cur, p);
            if (wrapped && cur->ps.empty()) continue;
            cur->ps.push_back(&p);
            cur->width += p.width;
            continue;
        }
        if (maxW > 0 && !cur->ps.empty() && cur->width + p.width > maxW + 0.5f) {
            lineEnd(*cur);
            lines.emplace_back();
            cur = &lines.back();
            wrapped = true;
        }
        cover(*cur, p);
        cur->ps.push_back(&p);
        cur->width += p.width;
    }
    lineEnd(lines.back());

    // Vertical extent: Figma's line-box model. With an explicit lineHeight
    // every line box has that height; otherwise each line uses the largest
    // natural line advance it contains.
    flow.lineHs.resize(lines.size());
    for (size_t li = 0; li < lines.size(); ++li) {
        float nat = base.fontSize * 1.2f;
        for (Piece* p : lines[li].ps) nat = std::fmax(nat, p->natLineH);
        flow.lineHs[li] = base.lineHeightPx > 0 ? base.lineHeightPx : nat;
        flow.contentH += flow.lineHs[li];
        flow.maxLineW = std::fmax(flow.maxLineW, lines[li].width);
    }
    return true;
}

// Multi-style rich text. Runs are tokenized into wrap-atomic pieces (words,
// space gaps, individual CJK characters), greedily flowed into lines (an
// explicit \n forces a break), and rendered as one tvg::Text per piece with
// baselines aligned per line. Returns nullptr when a font is missing —
// caller falls back to the uniform base-style path.
tvg::Paint* makeRichText(const Node& n, BuildContext& ctx) {
    if (n.textRuns.empty() || !ctx.fonts) return nullptr;
    const TextStyle& base = n.textStyle;

    // Auto-width boxes never wrap (they hug their text).
    const bool noWrap = base.autoResize == "WIDTH_AND_HEIGHT" || n.width <= 0;
    TextFlow flow;
    if (!buildTextFlow(n, noWrap ? 0 : n.width, ctx, true /*keepTexts*/, flow)) {
        return nullptr;
    }
    auto& lines = flow.lines;
    auto& lineHs = flow.lineHs;
    const float contentH = flow.contentH;
    using Piece = TextPiece;
    using Line = TextFlow::Line;

    float top = 0;
    if (base.alignV == TextStyle::AlignV::Center) top = (n.height - contentH) * 0.5f;
    else if (base.alignV == TextStyle::AlignV::Bottom) top = n.height - contentH;

    auto* scene = tvg::Scene::gen();
    for (size_t li = 0; li < lines.size(); ++li) {
        const Line& ln = lines[li];
        if (!ln.ps.empty()) {
            float maxAscent = 0, maxFontH = 0;
            for (Piece* p : ln.ps) {
                maxAscent = std::fmax(maxAscent, p->ascent);
                maxFontH = std::fmax(maxFontH, p->fontH);
            }
            const float baselineY = top + (lineHs[li] - maxFontH) * 0.5f + maxAscent;
            float x = base.alignH == TextStyle::AlignH::Center  ? (n.width - ln.width) * 0.5f
                      : base.alignH == TextStyle::AlignH::Right ? n.width - ln.width
                                                                : 0.0f;
            for (Piece* p : ln.ps) {
                if (p->text) {
                    p->text->translate(x, baselineY - p->ascent);
                    scene->add(p->text);
                    p->text = nullptr;  // owned by the scene now
                }
                x += p->width;
            }
        }
        top += lineHs[li];
    }
    // Release any pieces that never made it into the scene (trailing spaces).
    flow.release();
    return scene;
}

tvg::Text* makeText(const Node& n, BuildContext& ctx) {
    if (n.characters.empty() || !ctx.fonts) return nullptr;

    const TextStyle& ts = n.textStyle;
    std::string key = ctx.fonts->fontKeyFor(ts.fontFamily, ts.fontWeight, ts.italic);
    if (key.empty()) return nullptr;
    key = keyForText(ctx, key, n.characters, ts.fontWeight, ts.italic);

    auto* text = tvg::Text::gen();
    if (text->font(key.c_str()) != tvg::Result::Success) {
        tvg::Paint::rel(text);
        return nullptr;
    }
    // ThorVG sizes text in points and rasterizes at px = pt * 96/72; Figma
    // fontSize is the em size in pixels, so feed points to land on px.
    text->size(ts.fontSize * 72.0f / 96.0f);
    text->text(n.characters.c_str());
    if (ts.italic) text->italic();

    tvg::TextMetrics m{};
    const bool hasMetrics = text->metrics(m) == tvg::Result::Success && m.advance > 0;

    // ThorVG spacing() takes scale factors on the glyph/line advance, not
    // pixels. Convert Figma's pixel letterSpacing using the mean glyph
    // advance of this very string (fallback: 0.5em); the line factor maps
    // the font's natural advance onto Figma's lineHeight.
    float letterFactor = 1.0f, lineFactor = 1.0f;
    if (ts.letterSpacing != 0 && ts.fontSize > 0) {
        float meanAdvance = ts.fontSize * 0.5f;
        float total = 0;
        int count = 0;
        char one[2] = {0, 0};
        for (const char c : n.characters) {
            if (static_cast<unsigned char>(c) >= 0x80 || c == '\n') continue;  // ASCII only
            one[0] = c;
            tvg::GlyphMetrics gm{};
            if (text->metrics(one, gm) == tvg::Result::Success && gm.advance > 0) {
                total += gm.advance;
                ++count;
            }
        }
        if (count > 0) meanAdvance = total / static_cast<float>(count);
        letterFactor = std::fmax(0.0f, 1.0f + ts.letterSpacing / meanAdvance);
    }
    const float lineH = ts.lineHeightPx > 0 ? ts.lineHeightPx
                        : hasMetrics        ? m.advance
                                            : ts.fontSize * 1.2f;
    if (hasMetrics) lineFactor = lineH / m.advance;
    if (letterFactor != 1.0f || lineFactor != 1.0f) text->spacing(letterFactor, lineFactor);

    // Horizontal alignment is ThorVG's job (within the box width). Vertical
    // placement follows Figma's line-box model instead of ThorVG's font-box
    // alignment: content height = lines × lineHeight, and glyphs sit in each
    // line box with half-leading above and below.
    text->layout(n.width, 0);
    // Auto-width boxes hug their text; never wrap them (a substituted font
    // could exceed the stored width and break the layout). Ellipsis covers
    // Figma's TRUNCATE resize mode and textTruncation: ENDING labels — but
    // ThorVG's ellipsis cuts ~3 advances early, so skip it when the whole
    // string measurably fits the box.
    // WIDTH_AND_HEIGHT boxes grow with their text, so the stored width is
    // never a truncation constraint.
    bool ellipsis = ts.autoResize == "TRUNCATE" ||
                    (ts.truncateEnding && ts.maxLines <= 1 &&
                     ts.autoResize != "WIDTH_AND_HEIGHT");
    if (ellipsis && n.width > 0) {
        int glyphs = 0;
        const float w = measureAdvance(*text, n.characters, glyphs);
        if (glyphs > 0 && w <= n.width * 1.02f) ellipsis = false;
    }
    text->wrap(ellipsis                              ? tvg::TextWrap::Ellipsis
               : ts.autoResize == "WIDTH_AND_HEIGHT" ? tvg::TextWrap::None
                                                     : tvg::TextWrap::Word);
    const float ax = ts.alignH == TextStyle::AlignH::Center  ? 0.5f
                     : ts.alignH == TextStyle::AlignH::Right ? 1.0f
                                                             : 0.0f;
    text->align(ax, 0);

    if (hasMetrics) {
        const uint32_t lines = std::max(1u, text->lines());
        const float contentH = lines * lineH;
        const float fontH = m.ascent - m.descent;  // descent is negative (TTF)
        float top = 0;
        if (ts.alignV == TextStyle::AlignV::Center) top = (n.height - contentH) * 0.5f;
        else if (ts.alignV == TextStyle::AlignV::Bottom) top = n.height - contentH;
        const float ty = top + (lineH - fontH) * 0.5f;  // half-leading
        if (ty != 0) text->translate(0, ty);
    }

    // First visible paint colors the text.
    for (const auto& p : n.fills) {
        if (!p.visible || p.opacity <= 0) continue;
        if (p.type == PaintType::Solid) {
            text->fill(channel(p.color.r), channel(p.color.g), channel(p.color.b));
            text->opacity(static_cast<uint8_t>(channel(p.color.a) * p.opacity));
        } else if (p.type != PaintType::Image) {
            text->fill(makeGradient(p, n.width, n.height));
        }
        break;
    }
    return text;
}

// INNER_SHADOW has no ThorVG counterpart: emulate it with the blurred
// inverse of the node outline (a padded rect minus the outline, even-odd
// fill), offset and clipped back to the outline. Drawn above fills and
// children, matching Figma. The spread parameter is not reproduced.
void addInnerShadows(tvg::Scene& scene, const Node& n) {
    for (const auto& fx : n.effects) {
        if (fx.type != Effect::Type::InnerShadow || !fx.visible) continue;
        if (n.width <= 0 || n.height <= 0) continue;

        const float pad =
            fx.radius * 2.0f + std::fabs(fx.offsetX) + std::fabs(fx.offsetY) + 8.0f;
        auto* inv = tvg::Shape::gen();
        inv->appendRect(-pad, -pad, n.width + 2 * pad, n.height + 2 * pad);
        appendOutline(*inv, n);
        inv->fillRule(tvg::FillRule::EvenOdd);  // outline becomes a hole
        inv->fill(channel(fx.color.r), channel(fx.color.g), channel(fx.color.b),
                  channel(fx.color.a));
        inv->translate(fx.offsetX, fx.offsetY);

        auto* blurred = tvg::Scene::gen();
        blurred->add(inv);
        if (fx.radius > 0) {
            blurred->add(tvg::SceneEffect::GaussianBlur,
                         static_cast<double>(std::fmax(fx.radius * 0.5f, 0.01f)), 0, 0, 90);
        }
        // Confine the shadow with an alpha MASK, not a clip: ThorVG pushes
        // clips down to shape rasterization (before scene post-effects), so
        // a clipped blur would bleed outside the outline. Masks composite
        // after the scene's effects.
        auto* mask = tvg::Shape::gen();
        appendOutline(*mask, n);
        mask->fill(255, 255, 255, 255);
        blurred->mask(mask, tvg::MaskMethod::Alpha);
        scene.add(blurred);
    }
}

void applyEffects(tvg::Scene& scene, const Node& n) {
    for (const auto& fx : n.effects) {
        if (!fx.visible) continue;
        switch (fx.type) {
        case Effect::Type::DropShadow: {
            const double angle = std::atan2(fx.offsetY, fx.offsetX) * 180.0 / kPi;
            const double distance = std::hypot(fx.offsetX, fx.offsetY);
            const double sigma = std::fmax(fx.radius * 0.5f, 0.01f);
            scene.add(tvg::SceneEffect::DropShadow, static_cast<int>(channel(fx.color.r)),
                      static_cast<int>(channel(fx.color.g)),
                      static_cast<int>(channel(fx.color.b)),
                      static_cast<int>(channel(fx.color.a)), angle, distance, sigma, 90);
            break;
        }
        case Effect::Type::LayerBlur:
            scene.add(tvg::SceneEffect::GaussianBlur,
                      static_cast<double>(std::fmax(fx.radius * 0.5f, 0.01f)), 0, 0, 90);
            break;
        default:
            break;  // InnerShadow → addInnerShadows; BackgroundBlur unsupported
        }
    }
}

// Boolean op without a precomputed outline: approximate with a UNION over
// the children's PAINTED REGIONS, drawn with the boolean node's own paints
// (Figma ignores the children's colors). A child's painted region is its
// fill interior ∪ its stroke outline — stroke-only children (icon line art
// like Figma's icon-16-pen) contribute a stroked outline, not their path
// interior. Children carrying their own combined geometry (nested booleans
// baked by Figma) are leaves: recursing into their inputs would repaint the
// source shapes solid. Painted into one scene so group opacity composites
// without darkening overlaps.
tvg::Scene* buildBooleanApprox(Node& n) {
    auto* scene = tvg::Scene::gen();

    const Paint* fill = nullptr;
    for (const auto& p : n.fills) {
        if (p.visible && p.opacity > 0 && p.type != PaintType::Image) {
            fill = &p;
            break;
        }
    }
    const Paint* stroke = nullptr;
    for (const auto& p : n.strokes) {
        if (p.visible && p.opacity > 0 && p.type != PaintType::Image) {
            stroke = &p;
            break;
        }
    }
    if (!fill && !stroke) return scene;

    // Solid tint for child stroke outlines (gradient fills use the first stop).
    Color fillTint{0, 0, 0, 1};
    float fillTintOpacity = 1.0f;
    if (fill) {
        fillTint = fill->type == PaintType::Solid || fill->stops.empty()
                       ? fill->color
                       : fill->stops.front().color;
        fillTintOpacity = fill->opacity;
    }
    const auto anyVisible = [](const std::vector<Paint>& paints) {
        for (const auto& p : paints)
            if (p.visible && p.opacity > 0) return true;
        return false;
    };

    std::function<void(Node&, const Mat23&)> rec = [&](Node& d, const Mat23& acc) {
        for (auto& childPtr : d.children) {
            Node& c = *childPtr;
            if (!c.effectivelyVisible()) continue;
            const Mat23 m = acc * c.relativeTransform;
            const bool childFill = anyVisible(c.fills);
            const bool childStroke = c.strokeWeight > 0 && anyVisible(c.strokes);
            // Interior also serves as the fallback for children without any
            // paints of their own (REST trees often strip them).
            const bool wantInterior = childFill || !childStroke;

            auto buildPath = [&](tvg::Shape& shape, const PathGeometry& geom) {
                if (!appendSvgPath(shape, geom.path.c_str())) return false;
                shape.fillRule(geom.evenOdd ? tvg::FillRule::EvenOdd
                                            : tvg::FillRule::NonZero);
                return true;
            };
            auto place = [&](tvg::Shape* shape) {
                shape->transform({m.m00, m.m01, m.m02, m.m10, m.m11, m.m12, 0, 0, 1});
                scene->add(shape);
            };
            auto emitInterior = [&](tvg::Shape* shape) {
                if (fill) applyFill(*shape, *fill, n);
                if (stroke && n.strokeWeight > 0) {
                    applyStrokeStyle(*shape, n);
                    shape->strokeFill(channel(stroke->color.r), channel(stroke->color.g),
                                      channel(stroke->color.b),
                                      static_cast<uint8_t>(channel(stroke->color.a) *
                                                           stroke->opacity));
                }
                place(shape);
            };
            // Stroke outline region: the child's width/cap/join/dashes with
            // the boolean's color. (INSIDE/OUTSIDE alignment approximated as
            // CENTER — boolean inputs are almost always center-stroked.)
            auto emitStrokeRegion = [&](tvg::Shape* shape) {
                applyStrokeStyle(*shape, c);
                shape->strokeFill(channel(fillTint.r), channel(fillTint.g),
                                  channel(fillTint.b),
                                  static_cast<uint8_t>(channel(fillTint.a) *
                                                       fillTintOpacity));
                place(shape);
            };

            bool leaf = false;
            if (!c.fillGeometry.empty()) {
                for (const auto& geom : c.fillGeometry) {
                    if (wantInterior) {
                        auto* shape = tvg::Shape::gen();
                        if (buildPath(*shape, geom)) emitInterior(shape);
                        else tvg::Paint::rel(shape);
                    }
                    if (childStroke && c.strokeGeometry.empty()) {
                        auto* shape = tvg::Shape::gen();
                        if (buildPath(*shape, geom)) emitStrokeRegion(shape);
                        else tvg::Paint::rel(shape);
                    }
                }
                leaf = true;
            } else if (c.children.empty() && (c.width > 0 || c.height > 0)) {
                if (wantInterior) {
                    auto* shape = tvg::Shape::gen();
                    appendPrimitive(*shape, c);
                    emitInterior(shape);
                }
                if (childStroke && c.strokeGeometry.empty()) {
                    auto* shape = tvg::Shape::gen();
                    appendPrimitive(*shape, c);
                    emitStrokeRegion(shape);
                }
                leaf = true;
            }
            // Baked stroke outlines (REST geometry=paths): exact region, fill it.
            if (childStroke && !c.strokeGeometry.empty()) {
                for (const auto& geom : c.strokeGeometry) {
                    auto* shape = tvg::Shape::gen();
                    if (buildPath(*shape, geom) && fill) {
                        applyFill(*shape, *fill, n);
                        place(shape);
                    } else {
                        tvg::Paint::rel(shape);
                    }
                }
                leaf = true;
            }
            if (!leaf) rec(c, m);
        }
    };
    rec(n, Mat23::identity());
    return scene;
}

// First visible solid fill — used as the caret color so it matches the text.
Color textInkColor(const Node& n) {
    for (const auto& p : n.fills) {
        if (p.visible && p.opacity > 0 && p.type == PaintType::Solid) {
            Color c = p.color;
            c.a *= p.opacity;
            return c;
        }
    }
    return {0, 0, 0, 1};
}

// Next UTF-8 code-point boundary at or after i+1.
size_t nextCpBoundary(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

// ---- Password mask (Node::passwordMask) ----
// The DISPLAY text replaces every code point (newlines included) with
// U+2022 "•" — exactly 3 UTF-8 bytes — so real-byte ↔ display-byte offsets
// map through the code-point index. All text entry points below lay out the
// display twin, keeping caret/selection/click metrics and pixels in sync,
// while Node::characters stays the real plaintext.
constexpr char kMaskBullet[] = "\xE2\x80\xA2";  // U+2022 BULLET

// Real byte offset -> display byte offset (code points before it × 3).
int maskDisplayByte(const std::string& real, int byte) {
    const size_t b = std::min(static_cast<size_t>(std::max(byte, 0)), real.size());
    int cps = 0;
    for (size_t i = 0; i < b; i = nextCpBoundary(real, i)) ++cps;
    return cps * 3;
}

// Display byte offset -> real byte offset (start of code point #db/3).
int maskRealByte(const std::string& real, int displayByte) {
    if (displayByte < 0) return displayByte;
    int cps = displayByte / 3;
    size_t i = 0;
    while (cps-- > 0 && i < real.size()) i = nextCpBoundary(real, i);
    return static_cast<int>(i);
}

// Display twin of a passwordMask TEXT node: same box/base style, characters
// swapped for bullets, runs reduced to one base-style run (per-run styling
// is moot under the mask; a run keeps rendering on the flow path so glyphs
// and caret math share the same layout), caret/selection remapped.
void makeMaskTwin(const Node& src, Node& out) {
    static_cast<NodeData&>(out) = src;
    out.passwordMask = false;  // the twin lays out literally — no recursion
    std::string masked;
    masked.reserve(src.characters.size() * 3);
    for (size_t i = 0; i < src.characters.size(); i = nextCpBoundary(src.characters, i)) {
        masked += kMaskBullet;
    }
    out.characters = std::move(masked);
    out.textRuns.clear();
    if (!out.characters.empty()) {
        TextRun run;
        run.start = 0;
        run.end = static_cast<int>(out.characters.size());
        run.style = src.textStyle;
        out.textRuns.push_back(run);
    }
    if (src.caretByte >= 0) {
        out.caretByte = maskDisplayByte(src.characters, src.caretByte);
    }
    if (src.selAnchorByte >= 0) {
        out.selAnchorByte = maskDisplayByte(src.characters, src.selAnchorByte);
    }
}

// X position on a flowed line for a byte offset (clamped to the line span),
// starting from the line's aligned origin `startX`.
float lineCaretX(const Node& n, const TextFlow::Line& ln, float startX, int byte) {
    float x = startX;
    for (const TextPiece* p : ln.ps) {
        if (byte >= p->srcEnd) {
            x += p->width;
            continue;
        }
        if (byte <= p->srcStart) break;
        // Inside the token: prefix advance with the token's resolved font.
        auto* t = tvg::Text::gen();
        if (t->font(p->fontKey.c_str()) == tvg::Result::Success) {
            t->size(p->fontPt);
            int glyphs = 0;
            const std::string prefix = n.characters.substr(
                p->srcStart, static_cast<size_t>(byte - p->srcStart));
            x += measureAdvance(*t, prefix, glyphs) * p->letterFactor;
        }
        tvg::Paint::rel(t);
        break;
    }
    return x;
}

float alignedLineX(const Node& n, float lineW) {
    const TextStyle& base = n.textStyle;
    return base.alignH == TextStyle::AlignH::Center  ? (n.width - lineW) * 0.5f
           : base.alignH == TextStyle::AlignH::Right ? n.width - lineW
                                                     : 0.0f;
}

float alignedTop(const Node& n, float contentH) {
    const TextStyle& base = n.textStyle;
    if (base.alignV == TextStyle::AlignV::Center) return (n.height - contentH) * 0.5f;
    if (base.alignV == TextStyle::AlignV::Bottom) return n.height - contentH;
    return 0;
}

// Caret rectangle in node-local coordinates for a UTF-8 byte offset into
// Node::characters (clamped). Uses the same flow as rendering, so the caret
// lands exactly between the drawn glyphs.
bool textCaretRect(const Node& n, int caretByte, BuildContext& ctx, float& outX,
                   float& outY, float& outH) {
    const TextStyle& base = n.textStyle;
    const float fallbackH =
        base.lineHeightPx > 0 ? base.lineHeightPx : base.fontSize * 1.2f;

    TextFlow flow;
    const bool noWrap = base.autoResize == "WIDTH_AND_HEIGHT" || n.width <= 0;
    if (n.characters.empty() ||
        !buildTextFlow(n, noWrap ? 0 : n.width, ctx, false, flow)) {
        // Empty box (or missing fonts): caret at the alignment origin.
        outX = alignedLineX(n, 0);
        outY = alignedTop(n, fallbackH);
        outH = fallbackH;
        return true;
    }

    float top = alignedTop(n, flow.contentH);
    const int caret = std::clamp(caretByte, 0, static_cast<int>(n.characters.size()));
    for (size_t li = 0; li < flow.lines.size(); ++li) {
        const auto& ln = flow.lines[li];
        if (li + 1 < flow.lines.size() && caret > ln.srcEnd) {
            top += flow.lineHs[li];
            continue;
        }
        outX = lineCaretX(n, ln, alignedLineX(n, ln.width), caret);
        outY = top;
        outH = flow.lineHs[li];
        return true;
    }
    return false;
}

}  // namespace

bool measureTextNode(const Node& n, float maxWidth, BuildContext& ctx,
                     float& outWidth, float& outHeight) {
    if (n.passwordMask) {  // measure what is actually drawn: the bullets
        Node twin;
        makeMaskTwin(n, twin);
        return measureTextNode(twin, maxWidth, ctx, outWidth, outHeight);
    }
    TextFlow flow;
    if (!buildTextFlow(n, maxWidth, ctx, false /*keepTexts*/, flow)) return false;
    outWidth = flow.maxLineW;
    outHeight = flow.contentH;
    return true;
}

int textByteFromPoint(const Node& n, BuildContext& ctx, float x, float y) {
    if (n.passwordMask) {  // hit the bullets, answer in plaintext bytes
        Node twin;
        makeMaskTwin(n, twin);
        const int db = textByteFromPoint(twin, ctx, x, y);
        return db < 0 ? db : maskRealByte(n.characters, db);
    }
    if (n.characters.empty()) return 0;
    const TextStyle& base = n.textStyle;
    TextFlow flow;
    const bool noWrap = base.autoResize == "WIDTH_AND_HEIGHT" || n.width <= 0;
    if (!buildTextFlow(n, noWrap ? 0 : n.width, ctx, false, flow)) return -1;

    // Pick the line by y (clamped to first/last), then walk its pieces.
    float top = alignedTop(n, flow.contentH);
    size_t li = 0;
    while (li + 1 < flow.lines.size() && y >= top + flow.lineHs[li]) {
        top += flow.lineHs[li];
        ++li;
    }
    const auto& ln = flow.lines[li];

    float px = alignedLineX(n, ln.width);
    if (x <= px) return ln.srcStart;
    for (const TextPiece* p : ln.ps) {
        if (p->newline || p->srcStart < 0) break;
        if (x >= px + p->width) {
            px += p->width;
            continue;
        }
        // Inside this token: snap to the nearest code-point boundary.
        if (!p->text) {  // spaces carry no glyphs — distribute evenly
            int cps = 0;
            for (size_t b = p->srcStart; b < static_cast<size_t>(p->srcEnd);
                 b = nextCpBoundary(n.characters, b)) {
                ++cps;
            }
            const float w = cps > 0 ? p->width / cps : p->width;
            size_t b = p->srcStart;
            float bx = px;
            while (b < static_cast<size_t>(p->srcEnd) && x > bx + w * 0.5f) {
                bx += w;
                b = nextCpBoundary(n.characters, b);
            }
            return static_cast<int>(b);
        }
        auto* t = tvg::Text::gen();
        int result = p->srcEnd;
        if (t->font(p->fontKey.c_str()) == tvg::Result::Success) {
            t->size(p->fontPt);
            float prev = 0;
            size_t b = p->srcStart;
            while (b < static_cast<size_t>(p->srcEnd)) {
                const size_t nb = nextCpBoundary(n.characters, b);
                int glyphs = 0;
                const std::string prefix =
                    n.characters.substr(p->srcStart, nb - p->srcStart);
                const float adv = measureAdvance(*t, prefix, glyphs) * p->letterFactor;
                if (x < px + (prev + adv) * 0.5f) {  // nearer the left boundary
                    result = static_cast<int>(b);
                    break;
                }
                prev = adv;
                b = nb;
                result = static_cast<int>(b);
            }
        }
        tvg::Paint::rel(t);
        return result;
    }
    // Past the last glyph: line end, but in front of a hard line break.
    int e = ln.srcEnd;
    if (e > ln.srcStart && e > 0 && n.characters[e - 1] == '\n') --e;
    return e;
}

bool textSelectionRects(const Node& n, int a, int b, BuildContext& ctx,
                        std::vector<std::array<float, 4>>& outRects) {
    if (n.passwordMask) {  // highlight spans measured on the bullets
        Node twin;
        makeMaskTwin(n, twin);
        return textSelectionRects(twin, maskDisplayByte(n.characters, a),
                                  maskDisplayByte(n.characters, b), ctx, outRects);
    }
    if (n.characters.empty() || a >= b) return false;
    const TextStyle& base = n.textStyle;
    TextFlow flow;
    const bool noWrap = base.autoResize == "WIDTH_AND_HEIGHT" || n.width <= 0;
    if (!buildTextFlow(n, noWrap ? 0 : n.width, ctx, false, flow)) return false;

    const int len = static_cast<int>(n.characters.size());
    a = std::clamp(a, 0, len);
    b = std::clamp(b, 0, len);
    float top = alignedTop(n, flow.contentH);
    for (size_t li = 0; li < flow.lines.size(); ++li) {
        const auto& ln = flow.lines[li];
        const float lineH = flow.lineHs[li];
        const int lo = std::max(a, ln.srcStart);
        const int hi = std::min(b, ln.srcEnd);
        if (hi > lo) {
            const float sx = alignedLineX(n, ln.width);
            const float x0 = lineCaretX(n, ln, sx, lo);
            float x1;
            if (b > ln.srcEnd) {  // selection continues past this line
                float w = 0;
                for (const TextPiece* p : ln.ps) w += p->width;
                x1 = sx + w;
            } else {
                x1 = lineCaretX(n, ln, sx, hi);
            }
            if (x1 > x0) outRects.push_back({x0, top, x1 - x0, lineH});
        }
        top += lineH;
    }
    return !outRects.empty();
}

tvg::Scene* buildNodeScene(Node& node, const Mat23& parentAbs, BuildContext& ctx,
                           bool isRoot) {
    if (!node.effectivelyVisible() || node.renderSuppressed) return nullptr;
    if (node.type == NodeType::Slice) return nullptr;

    Mat23 local = isRoot ? Mat23::identity() : node.relativeTransform;
    const Mat23 baseLocal = local;
    // Children of a scrolling frame shift by the frame's scroll offset;
    // scrollFixed children (sticky headers/navbars) stay put. Folding the
    // offset into `local` keeps absoluteTransform — and therefore hit
    // testing — consistent with what is drawn.
    bool scrolled = false;
    if (!isRoot && node.parent && node.parent->scrolls() && !node.scrollFixed) {
        Mat23 shift;
        shift.m02 = -node.parent->scrollX;
        shift.m12 = -node.parent->scrollY;
        local = shift * local;
        scrolled = true;
    }
    node.absoluteTransform = parentAbs * local;

    auto* scene = tvg::Scene::gen();
    scene->transform(toTvg(local));
    scene->opacity(static_cast<uint8_t>(std::lround(node.effectiveOpacity() * 255)));

    bool skipChildren = false;
    if (node.type == NodeType::Text) {
        // passwordMask: rasterize (and place caret/selection on) the bullet
        // display twin — the plaintext never reaches the raster.
        Node twin;
        if (node.passwordMask) makeMaskTwin(node, twin);
        const Node& tn = node.passwordMask ? twin : node;
        // Selection highlight first, so it sits behind the glyphs.
        if (tn.caretByte >= 0 && tn.selAnchorByte >= 0 &&
            tn.selAnchorByte != tn.caretByte) {
            const int a = std::min(tn.caretByte, tn.selAnchorByte);
            const int b = std::max(tn.caretByte, tn.selAnchorByte);
            std::vector<std::array<float, 4>> rects;
            if (textSelectionRects(tn, a, b, ctx, rects)) {
                auto* sel = tvg::Shape::gen();
                for (const auto& r : rects) sel->appendRect(r[0], r[1], r[2], r[3], 2, 2);
                sel->fill(64, 140, 255, 105);
                scene->add(sel);
            }
        }
        if (auto* rich = makeRichText(tn, ctx)) scene->add(rich);
        else if (auto* text = makeText(tn, ctx)) scene->add(text);
        if (tn.caretByte >= 0) {  // focused for editing: draw the caret
            float cx = 0, cy = 0, ch = 0;
            if (textCaretRect(tn, tn.caretByte, ctx, cx, cy, ch)) {
                auto* caret = tvg::Shape::gen();
                caret->appendRect(cx - 0.75f, cy + ch * 0.08f, 1.5f, ch * 0.84f, 0.75f,
                                  0.75f);
                const Color c = textInkColor(node);
                caret->fill(channel(c.r), channel(c.g), channel(c.b), channel(c.a));
                scene->add(caret);
            }
        }
    } else if (node.type == NodeType::BooleanOperation && node.fillGeometry.empty() &&
               !node.children.empty()) {
        // No precomputed outline: UNION approximation over the source shapes.
        scene->add(buildBooleanApprox(node));
        skipChildren = true;
    } else {
        for (const auto& paint : node.fills) pushFillPaint(*scene, paint, node, ctx);
        for (const auto& paint : node.strokes) pushStrokePaint(*scene, paint, node);
    }

    if (!skipChildren) {
        for (auto& child : node.children) {
            if (auto* cs = buildNodeScene(*child, node.absoluteTransform, ctx)) scene->add(cs);
        }
    }

    addInnerShadows(*scene, node);

    // Scrolling frames always clip — content outside the viewport box must
    // not bleed even when the author left "clip content" off.
    tvg::Shape* clipShape = nullptr;
    if ((node.clipsContent || node.scrolls()) && node.width > 0 && node.height > 0) {
        clipShape = tvg::Shape::gen();
        appendPrimitive(*clipShape, node);
        // A clipper attaches to the clipped paint's PARENT, so it inherits
        // ancestor transforms but not this scene's own local transform —
        // apply it explicitly or the clip lands at the parent's origin.
        clipShape->transform(toTvg(local));
        scene->clip(clipShape);
    }

    if (scrolled && ctx.scrollBindings) {
        ctx.scrollBindings->push_back({node.parent, &node, scene, clipShape, baseLocal});
    }

    applyEffects(*scene, node);
    return scene;
}

}  // namespace figo
