#include "scene_builder.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "svg_path.h"

namespace figmalib {

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

    if (!n.fillGeometry.empty()) {
        for (const auto& geom : n.fillGeometry) {
            auto* shape = tvg::Shape::gen();
            if (!appendSvgPath(*shape, geom.path.c_str())) {
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

    if (!n.strokeGeometry.empty()) {
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
        if (!n.fillGeometry.empty()) {
            for (const auto& geom : n.fillGeometry) appendSvgPath(*s, geom.path.c_str());
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

    if (!n.fillGeometry.empty()) {
        for (const auto& geom : n.fillGeometry) {
            auto* shape = tvg::Shape::gen();
            if (!appendSvgPath(*shape, geom.path.c_str())) {
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

// Sum of glyph advances for an ASCII string at the text's current font/size.
// Unlike ink bounds, this keeps trailing-space widths, which matters when
// laying rich-text runs end to end.
float measureAdvance(tvg::Text& text, const std::string& s, int& glyphs) {
    float total = 0;
    glyphs = 0;
    char one[2] = {0, 0};
    for (const char c : s) {
        if (static_cast<unsigned char>(c) >= 0x80) continue;
        one[0] = c;
        tvg::GlyphMetrics gm{};
        if (text.metrics(one, gm) == tvg::Result::Success && gm.advance > 0) {
            total += gm.advance;
            ++glyphs;
        }
    }
    return total;
}

// Multi-style rich text, MVP scope: single-line, uniform font size. Runs are
// rendered as separate Text paints laid end to end by advance width. Returns
// nullptr when the node is out of scope — caller falls back to base style.
tvg::Paint* makeRichText(const Node& n, BuildContext& ctx) {
    if (n.textRuns.empty() || !ctx.fonts) return nullptr;
    const TextStyle& base = n.textStyle;
    if (n.characters.find('\n') != std::string::npos) return nullptr;
    for (const auto& run : n.textRuns) {
        if (run.style.fontSize != base.fontSize) return nullptr;
    }
    const float lineH = base.lineHeightPx > 0 ? base.lineHeightPx : base.fontSize * 1.2f;
    // Multi-line boxes need wrap-aware run layout — out of MVP scope.
    if (n.height >= lineH * 1.9f && base.autoResize != "WIDTH_AND_HEIGHT") return nullptr;

    // Node-level fill as the default run color.
    Color baseColor{0, 0, 0, 1};
    for (const auto& p : n.fills) {
        if (p.visible && p.opacity > 0 && p.type == PaintType::Solid) {
            baseColor = p.color;
            baseColor.a *= p.opacity;
            break;
        }
    }

    struct Piece {
        tvg::Text* text;   // null → whitespace-only run (advance only)
        float width;       // full run advance, including surrounding spaces
        float leadOffset;  // advance of stripped leading spaces
    };
    std::vector<Piece> pieces;
    auto bail = [&]() -> tvg::Paint* {
        for (auto& p : pieces) tvg::Paint::rel(p.text);
        return nullptr;
    };

    float fontH = 0;
    for (const auto& run : n.textRuns) {
        const std::string s =
            n.characters.substr(run.start, static_cast<size_t>(run.end - run.start));
        if (s.empty()) continue;
        const std::string key = ctx.fonts->fontKeyFor(run.style.fontFamily,
                                                      run.style.fontWeight, run.style.italic);
        if (key.empty()) return bail();
        auto* t = tvg::Text::gen();
        if (t->font(key.c_str()) != tvg::Result::Success) {
            tvg::Paint::rel(t);
            return bail();
        }
        t->size(run.style.fontSize * 72.0f / 96.0f);
        // ThorVG mispositions glyphs after a leading space (kerning against
        // the empty glyph) — strip surrounding spaces from the rendered text
        // and keep their advance in the piece width for x stepping.
        const size_t lead = s.find_first_not_of(' ');
        const std::string trimmed =
            lead == std::string::npos ? std::string()
                                      : s.substr(lead, s.find_last_not_of(' ') - lead + 1);
        t->text(trimmed.c_str());
        if (run.style.italic) t->italic();

        tvg::TextMetrics m{};
        if (fontH <= 0 && t->metrics(m) == tvg::Result::Success) fontH = m.ascent - m.descent;

        int glyphs = 0;
        float width = measureAdvance(*t, s, glyphs);  // full run incl. spaces
        int leadGlyphs = 0;
        const float leadWidth =
            lead == std::string::npos || lead == 0
                ? 0.0f
                : measureAdvance(*t, s.substr(0, lead), leadGlyphs);
        if (glyphs == 0) {
            tvg::Paint::rel(t);
            return bail();
        }
        float letterFactor = 1.0f;
        if (run.style.letterSpacing != 0 && glyphs > 0) {
            letterFactor = std::fmax(
                0.0f, 1.0f + run.style.letterSpacing / (width / static_cast<float>(glyphs)));
            t->spacing(letterFactor, 1.0f);
        }
        width *= letterFactor;

        const Color c = run.color ? *run.color : baseColor;
        t->fill(channel(c.r), channel(c.g), channel(c.b));
        t->opacity(channel(c.a));
        if (trimmed.empty()) {
            tvg::Paint::rel(t);
            t = nullptr;
        }
        pieces.push_back({t, width, leadWidth * letterFactor});
    }
    if (pieces.empty()) return nullptr;

    float total = 0;
    for (const auto& p : pieces) total += p.width;
    float x = base.alignH == TextStyle::AlignH::Center  ? (n.width - total) * 0.5f
              : base.alignH == TextStyle::AlignH::Right ? n.width - total
                                                        : 0.0f;
    // Same line-box vertical model as the uniform-style path.
    float top = 0;
    if (base.alignV == TextStyle::AlignV::Center) top = (n.height - lineH) * 0.5f;
    else if (base.alignV == TextStyle::AlignV::Bottom) top = n.height - lineH;
    const float ty = top + (lineH - fontH) * 0.5f;

    auto* scene = tvg::Scene::gen();
    for (auto& p : pieces) {
        if (p.text) {
            p.text->translate(x + p.leadOffset, ty);
            scene->add(p.text);
        }
        x += p.width;
    }
    return scene;
}

tvg::Text* makeText(const Node& n, BuildContext& ctx) {
    if (n.characters.empty() || !ctx.fonts) return nullptr;

    const TextStyle& ts = n.textStyle;
    const std::string key = ctx.fonts->fontKeyFor(ts.fontFamily, ts.fontWeight, ts.italic);
    if (key.empty()) return nullptr;

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
            break;  // InnerShadow / BackgroundBlur: not supported yet
        }
    }
}

// Boolean op without a precomputed outline: approximate with a UNION — draw
// every descendant outline with the boolean node's own paints (the children
// carry no fills of their own; Figma puts them on the boolean node). Painted
// into one scene so group opacity composites without darkening overlaps.
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

    std::function<void(Node&, const Mat23&)> rec = [&](Node& d, const Mat23& acc) {
        for (auto& childPtr : d.children) {
            Node& c = *childPtr;
            if (!c.effectivelyVisible()) continue;
            const Mat23 m = acc * c.relativeTransform;
            auto emit = [&](tvg::Shape* shape) {
                shape->transform({m.m00, m.m01, m.m02, m.m10, m.m11, m.m12, 0, 0, 1});
                if (fill) applyFill(*shape, *fill, n);
                if (stroke && n.strokeWeight > 0) {
                    applyStrokeStyle(*shape, n);
                    shape->strokeFill(channel(stroke->color.r), channel(stroke->color.g),
                                      channel(stroke->color.b),
                                      static_cast<uint8_t>(channel(stroke->color.a) *
                                                           stroke->opacity));
                }
                scene->add(shape);
            };
            if (!c.fillGeometry.empty()) {
                for (const auto& geom : c.fillGeometry) {
                    auto* shape = tvg::Shape::gen();
                    if (!appendSvgPath(*shape, geom.path.c_str())) {
                        tvg::Paint::rel(shape);
                        continue;
                    }
                    shape->fillRule(geom.evenOdd ? tvg::FillRule::EvenOdd
                                                 : tvg::FillRule::NonZero);
                    emit(shape);
                }
            } else if (c.children.empty() && (c.width > 0 || c.height > 0)) {
                auto* shape = tvg::Shape::gen();
                appendPrimitive(*shape, c);
                emit(shape);
            }
            rec(c, m);
        }
    };
    rec(n, Mat23::identity());
    return scene;
}

}  // namespace

tvg::Scene* buildNodeScene(Node& node, const Mat23& parentAbs, BuildContext& ctx,
                           bool isRoot) {
    if (!node.effectivelyVisible()) return nullptr;
    if (node.type == NodeType::Slice) return nullptr;

    const Mat23 local = isRoot ? Mat23::identity() : node.relativeTransform;
    node.absoluteTransform = parentAbs * local;

    auto* scene = tvg::Scene::gen();
    scene->transform(toTvg(local));
    scene->opacity(static_cast<uint8_t>(std::lround(node.effectiveOpacity() * 255)));

    bool skipChildren = false;
    if (node.type == NodeType::Text) {
        if (auto* rich = makeRichText(node, ctx)) scene->add(rich);
        else if (auto* text = makeText(node, ctx)) scene->add(text);
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

    if (node.clipsContent && node.width > 0 && node.height > 0) {
        auto* clip = tvg::Shape::gen();
        appendPrimitive(*clip, node);
        // A clipper attaches to the clipped paint's PARENT, so it inherits
        // ancestor transforms but not this scene's own local transform —
        // apply it explicitly or the clip lands at the parent's origin.
        clip->transform(toTvg(local));
        scene->clip(clip);
    }

    applyEffects(*scene, node);
    return scene;
}

}  // namespace figmalib
