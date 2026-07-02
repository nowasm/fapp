// Responsive layout: Figma constraints + auto-layout reflow.
//
// Model: every node carries an authored snapshot (base{Transform,Width,Height})
// taken at parse time. layoutFrame() restores the snapshot, imposes the new
// frame size, then walks down — auto-layout containers run a flexbox-like
// pass, everything else repositions children by their constraints relative to
// the parent's size delta. Sizes assigned to a child recurse into its subtree.
//
// Known simplifications:
//  - BASELINE alignment approximates the baseline from the text style (no
//    font metrics here): half-leading + ~0.8 em ascent; non-text children
//    hang from their bottom edge
//  - text nodes keep their box height when rewrapped by a width change
//  - groups resize by scaling child positions/sizes proportionally
//  - hug measurement of a WRAP container uses its no-wrap extent

#include "figo/layout.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace figo {

namespace {

TextMeasurer g_textMeasurer;

// Measured natural size of an auto-resize text node, or nullopt when the
// node keeps its authored box (fixed size, no measurer, missing font).
// HEIGHT boxes wrap at `width`; WIDTH_AND_HEIGHT boxes never wrap.
std::optional<std::pair<float, float>> textNaturalSize(const Node& n, float width) {
    if (n.type != NodeType::Text || !g_textMeasurer || n.characters.empty()) {
        return std::nullopt;
    }
    const std::string& ar = n.textStyle.autoResize;
    float w = 0, h = 0;
    if (ar == "WIDTH_AND_HEIGHT") {
        if (g_textMeasurer(n, 0, w, h)) return std::make_pair(w, h);
    } else if (ar == "HEIGHT") {
        if (width > 0 && g_textMeasurer(n, width, w, h)) {
            return std::make_pair(width, h);
        }
    }
    return std::nullopt;
}

bool inFlow(const Node& c) {
    // effectivelyVisible: a script-hidden child (runtimeVisible=0) gives up
    // its auto-layout slot, same as an authored-invisible one.
    return c.effectivelyVisible() && !c.layoutAbsolute && c.type != NodeType::Slice;
}

struct Size {
    float w = 0, h = 0;
};

float mainOf(const AutoLayout& al, const Size& s) {
    return al.mode == AutoLayout::Mode::Horizontal ? s.w : s.h;
}
float crossOf(const AutoLayout& al, const Size& s) {
    return al.mode == AutoLayout::Mode::Horizontal ? s.h : s.w;
}

// Natural ("hug") size, bottom-up: fixed axes report the authored size, hug
// axes recompute from children so deeper reflow propagates upward.
Size measure(const Node& n) {
    const AutoLayout& al = n.autoLayout;
    if (!al.enabled()) {
        if (const auto ts = textNaturalSize(n, n.baseWidth)) {
            return {ts->first, ts->second};
        }
        return {n.baseWidth, n.baseHeight};
    }

    float main = 0, cross = 0, anyCross = 0;
    int count = 0;
    for (const auto& cp : n.children) {
        if (!inFlow(*cp)) continue;
        const Size cs = measure(*cp);
        main += mainOf(al, cs);
        // Stretch children adapt to the container, so they don't drive hug
        // size — unless nothing else does.
        if (!cp->layoutAlignStretch) cross = std::max(cross, crossOf(al, cs));
        anyCross = std::max(anyCross, crossOf(al, cs));
        ++count;
    }
    if (cross <= 0) cross = anyCross;
    if (count > 1) main += al.itemSpacing * static_cast<float>(count - 1);

    const bool horiz = al.mode == AutoLayout::Mode::Horizontal;
    Size out{n.baseWidth, n.baseHeight};
    if (al.primarySizing == AutoLayout::Sizing::Hug) {
        const float v = main + (horiz ? al.paddingLeft + al.paddingRight
                                      : al.paddingTop + al.paddingBottom);
        (horiz ? out.w : out.h) = v;
    }
    if (al.counterSizing == AutoLayout::Sizing::Hug) {
        const float v = cross + (horiz ? al.paddingTop + al.paddingBottom
                                       : al.paddingLeft + al.paddingRight);
        (horiz ? out.h : out.w) = v;
    }
    return out;
}

void layoutChildren(Node& n);

float clampAxis(float v, float lo, float hi) {
    if (lo > 0 && v < lo) v = lo;
    if (hi > 0 && v > hi) v = hi;
    return std::max(0.0f, v);
}

// Assign a size (honoring the node's min/max limits) and propagate the
// change into the subtree.
void setSize(Node& n, float w, float h) {
    n.width = clampAxis(w, n.minWidth, n.maxWidth);
    n.height = clampAxis(h, n.minHeight, n.maxHeight);
    // Auto-height text re-wraps at its assigned width: the box height follows
    // the measured content (e.g. after setText or a stretch-driven re-wrap).
    if (const auto ts = textNaturalSize(n, n.width)) {
        if (n.textStyle.autoResize == "HEIGHT") {
            n.height = clampAxis(ts->second, n.minHeight, n.maxHeight);
        }
    }
    layoutChildren(n);
}

// Place the child at (x, y) keeping the authored rotation/scale part.
void place(Node& c, float x, float y) {
    c.relativeTransform = c.baseTransform;
    c.relativeTransform.m02 = x;
    c.relativeTransform.m12 = y;
}

// Reposition/resize a child by its constraints for the parent's size delta.
void applyConstraints(const Node& parent, Node& c) {
    const float dw = parent.width - parent.baseWidth;
    const float dh = parent.height - parent.baseHeight;
    const float sx = parent.baseWidth > 0 ? parent.width / parent.baseWidth : 1.0f;
    const float sy = parent.baseHeight > 0 ? parent.height / parent.baseHeight : 1.0f;

    float x = c.baseTransform.m02, y = c.baseTransform.m12;
    float w = c.baseWidth, h = c.baseHeight;

    switch (c.constraintH) {
    case Constraint::Min: break;
    case Constraint::Center: x += dw * 0.5f; break;
    case Constraint::Max: x += dw; break;
    case Constraint::Stretch: w += dw; break;
    case Constraint::Scale: x *= sx; w *= sx; break;
    }
    switch (c.constraintV) {
    case Constraint::Min: break;
    case Constraint::Center: y += dh * 0.5f; break;
    case Constraint::Max: y += dh; break;
    case Constraint::Stretch: h += dh; break;
    case Constraint::Scale: y *= sy; h *= sy; break;
    }

    // Hug auto-layout containers size themselves from content even under a
    // plain (constraint) parent — runtime child changes (bindList) must be
    // able to grow the box.
    if (c.autoLayout.enabled()) {
        const Size nat = measure(c);
        const bool horiz = c.autoLayout.mode == AutoLayout::Mode::Horizontal;
        if (c.autoLayout.primarySizing == AutoLayout::Sizing::Hug) {
            (horiz ? w : h) = horiz ? nat.w : nat.h;
        }
        if (c.autoLayout.counterSizing == AutoLayout::Sizing::Hug) {
            (horiz ? h : w) = horiz ? nat.h : nat.w;
        }
    }

    place(c, x, y);
    setSize(c, w, h);
}

// Approximate distance from a child's top edge to its alignment baseline.
// Text: half-leading plus ~0.8 em ascent (no font metrics in the layout
// pass); anything else hangs from its bottom edge, matching Figma.
float baselineOf(const Node& c, float crossLen) {
    if (c.type == NodeType::Text && c.textStyle.fontSize > 0) {
        const float fs = c.textStyle.fontSize;
        const float lineH = c.textStyle.lineHeightPx > 0 ? c.textStyle.lineHeightPx : fs * 1.2f;
        return (lineH - fs) * 0.5f + fs * 0.8f;
    }
    return crossLen;
}

// Flexbox-like pass over an auto-layout container whose size is already set.
void stackLayout(Node& n) {
    const AutoLayout& al = n.autoLayout;
    const bool horiz = al.mode == AutoLayout::Mode::Horizontal;
    const float padMainStart = horiz ? al.paddingLeft : al.paddingTop;
    const float padMainEnd = horiz ? al.paddingRight : al.paddingBottom;
    const float padCrossStart = horiz ? al.paddingTop : al.paddingLeft;
    const float padCrossEnd = horiz ? al.paddingBottom : al.paddingRight;
    const float innerMain = (horiz ? n.width : n.height) - padMainStart - padMainEnd;
    const float innerCross = (horiz ? n.height : n.width) - padCrossStart - padCrossEnd;

    std::vector<Node*> flow;
    std::vector<Size> natural;
    for (auto& cp : n.children) {
        Node& c = *cp;
        if (!inFlow(c)) {
            // Ignored by the stack: absolute children follow their constraints.
            if (c.visible && c.layoutAbsolute) applyConstraints(n, c);
            continue;
        }
        flow.push_back(&c);
        natural.push_back(measure(c));
    }
    if (flow.empty()) return;

    // Break the flow into lines: a single one normally, greedy fill when the
    // stack wraps.
    std::vector<std::pair<size_t, size_t>> lines;  // [begin, end)
    if (al.wrap) {
        size_t begin = 0;
        float acc = 0;
        for (size_t i = 0; i < flow.size(); ++i) {
            const float m = mainOf(al, natural[i]);
            if (i > begin && acc + al.itemSpacing + m > innerMain + 0.01f) {
                lines.emplace_back(begin, i);
                begin = i;
                acc = m;
            } else {
                acc += (i > begin ? al.itemSpacing : 0.0f) + m;
            }
        }
        lines.emplace_back(begin, flow.size());
    } else {
        lines.emplace_back(0, flow.size());
    }

    // Cross extent of each line. A single line owns the whole inner cross
    // axis; wrapped lines are as tall/wide as their largest item.
    const float lineGap = al.counterSpacing > 0 ? al.counterSpacing : al.itemSpacing;
    std::vector<float> lineCross(lines.size(), innerCross);
    float crossCursor = padCrossStart;
    if (lines.size() > 1) {
        float total = 0;
        for (size_t li = 0; li < lines.size(); ++li) {
            float m = 0;
            for (size_t i = lines[li].first; i < lines[li].second; ++i) {
                m = std::max(m, crossOf(al, natural[i]));
            }
            lineCross[li] = m;
            total += m;
        }
        total += lineGap * static_cast<float>(lines.size() - 1);
        const float leftoverCross = innerCross - total;
        if (al.counterAlign == AutoLayout::Align::Center) crossCursor += leftoverCross * 0.5f;
        else if (al.counterAlign == AutoLayout::Align::Max) crossCursor += leftoverCross;
    }

    for (size_t li = 0; li < lines.size(); ++li) {
        const size_t b = lines[li].first, e = lines[li].second;
        const float count = static_cast<float>(e - b);

        std::vector<float> mains(e - b);
        float totalMain = 0, totalGrow = 0;
        for (size_t i = b; i < e; ++i) {
            mains[i - b] = mainOf(al, natural[i]);
            totalMain += mains[i - b];
            totalGrow += std::max(0.0f, flow[i]->layoutGrow);
        }
        float leftover = innerMain - totalMain - al.itemSpacing * (count - 1);

        // Grow children absorb the leftover main-axis space (also shrink when
        // the container got smaller — Figma clamps at zero).
        if (totalGrow > 0 && leftover != 0) {
            for (size_t i = b; i < e; ++i) {
                const float g = std::max(0.0f, flow[i]->layoutGrow);
                if (g > 0) mains[i - b] = std::max(0.0f, mains[i - b] + leftover * g / totalGrow);
            }
            leftover = 0;
        }

        float gap = al.itemSpacing;
        float cursor = padMainStart;
        if (al.primaryAlign == AutoLayout::Align::SpaceBetween && count > 1) {
            gap += leftover / (count - 1);
        } else if (al.primaryAlign == AutoLayout::Align::Center) {
            cursor += leftover * 0.5f;
        } else if (al.primaryAlign == AutoLayout::Align::Max) {
            cursor += leftover;
        }

        // Baseline alignment (horizontal stacks): find the deepest baseline.
        float maxBaseline = 0;
        if (horiz && al.counterAlign == AutoLayout::Align::Baseline) {
            for (size_t i = b; i < e; ++i) {
                if (flow[i]->layoutAlignStretch) continue;
                maxBaseline = std::max(maxBaseline,
                                       baselineOf(*flow[i], crossOf(al, natural[i])));
            }
        }

        for (size_t i = b; i < e; ++i) {
            Node& c = *flow[i];
            const float crossLen =
                c.layoutAlignStretch ? lineCross[li] : crossOf(al, natural[i]);
            float crossPos = crossCursor;
            if (!c.layoutAlignStretch) {
                if (horiz && al.counterAlign == AutoLayout::Align::Baseline) {
                    crossPos += maxBaseline - baselineOf(c, crossLen);
                } else if (al.counterAlign == AutoLayout::Align::Center) {
                    crossPos += (lineCross[li] - crossLen) * 0.5f;
                } else if (al.counterAlign == AutoLayout::Align::Max) {
                    crossPos += lineCross[li] - crossLen;
                }
            }
            place(c, horiz ? cursor : crossPos, horiz ? crossPos : cursor);
            setSize(c, horiz ? mains[i - b] : crossLen, horiz ? crossLen : mains[i - b]);
            cursor += mains[i - b] + gap;
        }
        crossCursor += lineCross[li] + lineGap;
    }
}

void layoutChildren(Node& n) {
    if (n.autoLayout.enabled()) {
        stackLayout(n);
        return;
    }

    const bool resized = n.width != n.baseWidth || n.height != n.baseHeight;
    if (!resized) return;  // subtree is already at its authored geometry

    // Groups have no layout semantics of their own; resizing one scales its
    // contents proportionally (matching Figma's group resize behavior).
    if (n.type == NodeType::Group || n.type == NodeType::BooleanOperation) {
        const float sx = n.baseWidth > 0 ? n.width / n.baseWidth : 1.0f;
        const float sy = n.baseHeight > 0 ? n.height / n.baseHeight : 1.0f;
        for (auto& cp : n.children) {
            Node& c = *cp;
            place(c, c.baseTransform.m02 * sx, c.baseTransform.m12 * sy);
            setSize(c, c.baseWidth * sx, c.baseHeight * sy);
        }
        return;
    }

    for (auto& cp : n.children) applyConstraints(n, *cp);
}

}  // namespace

void resetLayout(Node& root) {
    root.visit([](Node& n) {
        n.relativeTransform = n.baseTransform;
        n.width = n.baseWidth;
        n.height = n.baseHeight;
        return true;
    });
}

void setTextMeasurer(TextMeasurer fn) { g_textMeasurer = std::move(fn); }

void relayoutNode(Node& n) {
    const Size nat = measure(n);
    float w = n.width, h = n.height;
    const AutoLayout& al = n.autoLayout;
    if (al.enabled()) {
        const bool horiz = al.mode == AutoLayout::Mode::Horizontal;
        if (al.primarySizing == AutoLayout::Sizing::Hug) (horiz ? w : h) = horiz ? nat.w : nat.h;
        if (al.counterSizing == AutoLayout::Sizing::Hug) (horiz ? h : w) = horiz ? nat.h : nat.w;
    }
    n.width = clampAxis(w, n.minWidth, n.maxWidth);
    n.height = clampAxis(h, n.minHeight, n.maxHeight);
    layoutChildren(n);
}

void layoutFrame(Node& frame, float width, float height) {
    if (width <= 0 || height <= 0) return;
    if (frame.baseWidth <= 0 || frame.baseHeight <= 0) return;
    resetLayout(frame);
    frame.width = width;
    frame.height = height;
    layoutChildren(frame);
}

}  // namespace figo
