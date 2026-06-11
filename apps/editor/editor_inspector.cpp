// Inspector panel — layout mirrors Figma's Design panel: alignment row,
// position/size, radius + opacity, Fill list, Stroke list (weight/align),
// Text properties and Effects toggles.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "editor.h"

#include <raygui.h>

namespace figmaedit {

namespace {

constexpr ::Color kPanelBg = kThemePanel;
constexpr ::Color kPanelEdge = kThemeEdge;
constexpr ::Color kTextCol = kThemeText;
constexpr ::Color kTextDim = kThemeDim;
constexpr ::Color kAccent = kThemeAccent;

template <typename Fn>
void commitEdit(EditorState& ed, Node* n, Fn&& fn) {
    std::vector<NodeProps> before{NodeProps::capture(n)};
    fn(*n);
    ed.pushPropsUndo(std::move(before));
    ed.bumpNode(n);
    ed.docDirty = true;
}

// Resize that respects baked vector geometry (same rule as canvas resize).
void setNodeSize(Node& n, float w, float h) {
    w = std::max(1.0f, w);
    h = std::max(1.0f, h);
    const bool hasGeometry = !n.fillGeometry.empty() || !n.strokeGeometry.empty();
    if (hasGeometry && n.width > 0 && n.height > 0) {
        n.relativeTransform.m00 *= w / n.width;
        n.relativeTransform.m11 *= h / n.height;
    } else {
        n.width = w;
        n.height = h;
    }
}

std::string colorToHex(const figmalib::Color& c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02X%02X%02X",
                  static_cast<int>(std::lround(c.r * 255)),
                  static_cast<int>(std::lround(c.g * 255)),
                  static_cast<int>(std::lround(c.b * 255)));
    return buf;
}

bool hexToColor(const char* s, figmalib::Color& out) {
    if (*s == '#') ++s;
    if (std::strlen(s) != 6) return false;
    auto nib = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = nib(s[i]);
        if (v[i] < 0) return false;
    }
    out.r = (v[0] * 16 + v[1]) / 255.0f;
    out.g = (v[2] * 16 + v[3]) / 255.0f;
    out.b = (v[4] * 16 + v[5]) / 255.0f;
    return true;
}

::Color toRl(const figmalib::Color& c) {
    return {static_cast<unsigned char>(c.r * 255), static_cast<unsigned char>(c.g * 255),
            static_cast<unsigned char>(c.b * 255), 255};
}

// Alignment glyphs drawn over plain buttons (0..5 = L, HC, R, T, VC, B).
void drawAlignGlyph(Rectangle r, int op) {
    const ::Color g{210, 210, 210, 255};
    const float cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    const float L = ui(7), S = ui(4.5f);
    const float bar = std::max(1.5f, ui(1.2f));
    switch (op) {
    case 0:
        DrawRectangleRec({r.x + ui(7), cy - L, bar, L * 2}, g);
        DrawRectangleRec({r.x + ui(10), cy - ui(4.5f), L * 1.5f, ui(3)}, g);
        DrawRectangleRec({r.x + ui(10), cy + ui(1.5f), L, ui(3)}, g);
        break;
    case 1:
        DrawRectangleRec({cx - bar / 2, cy - L, bar, L * 2}, g);
        DrawRectangleRec({cx - L, cy - ui(4.5f), L * 2, ui(3)}, g);
        DrawRectangleRec({cx - S, cy + ui(1.5f), S * 2, ui(3)}, g);
        break;
    case 2:
        DrawRectangleRec({r.x + r.width - ui(7) - bar, cy - L, bar, L * 2}, g);
        DrawRectangleRec({r.x + r.width - ui(10) - L * 1.5f, cy - ui(4.5f), L * 1.5f, ui(3)}, g);
        DrawRectangleRec({r.x + r.width - ui(10) - L, cy + ui(1.5f), L, ui(3)}, g);
        break;
    case 3:
        DrawRectangleRec({cx - L, r.y + ui(7), L * 2, bar}, g);
        DrawRectangleRec({cx - ui(4.5f), r.y + ui(10), ui(3), L * 1.5f}, g);
        DrawRectangleRec({cx + ui(1.5f), r.y + ui(10), ui(3), L}, g);
        break;
    case 4:
        DrawRectangleRec({cx - L, cy - bar / 2, L * 2, bar}, g);
        DrawRectangleRec({cx - ui(4.5f), cy - L, ui(3), L * 2}, g);
        DrawRectangleRec({cx + ui(1.5f), cy - S, ui(3), S * 2}, g);
        break;
    case 5:
        DrawRectangleRec({cx - L, r.y + r.height - ui(7) - bar, L * 2, bar}, g);
        DrawRectangleRec({cx - ui(4.5f), r.y + r.height - ui(10) - L * 1.5f, ui(3), L * 1.5f},
                         g);
        DrawRectangleRec({cx + ui(1.5f), r.y + r.height - ui(10) - L, ui(3), L}, g);
        break;
    }
}

void sectionHeader(float fx, float& y, const char* title) {
    DrawRectangle(static_cast<int>(fx - ui(6)), static_cast<int>(y),
                  kInspectorW - static_cast<int>(ui(16)), 1, kThemeLine);
    y += ui(10);
    uiText(title, fx, y, fontS(), {235, 235, 235, 255});
    y += ui(24);
}

// ---- Figma-style numeric field ----------------------------------------------
// Supports: click to edit (with math expressions: + - * / ^ parentheses),
// Enter/Tab commit (Tab focuses the next field), Esc cancel, Up/Down step
// (Shift = 10), drag-scrubbing on the whole field, "Mixed" display for
// multi-selection.

// Tiny recursive-descent expression evaluator.
struct ExprParser {
    const char* p;
    bool ok = true;

    double parse() {
        const double v = expr();
        skip();
        if (*p != 0) ok = false;
        return v;
    }
    void skip() {
        while (*p == ' ') ++p;
    }
    double expr() {  // + -
        double v = term();
        for (;;) {
            skip();
            if (*p == '+') { ++p; v += term(); }
            else if (*p == '-') { ++p; v -= term(); }
            else return v;
        }
    }
    double term() {  // * /
        double v = power();
        for (;;) {
            skip();
            if (*p == '*') { ++p; v *= power(); }
            else if (*p == '/') {
                ++p;
                const double d = power();
                if (d == 0) { ok = false; return 0; }
                v /= d;
            } else {
                return v;
            }
        }
    }
    double power() {  // ^ (right associative)
        const double base = unary();
        skip();
        if (*p == '^') {
            ++p;
            return std::pow(base, power());
        }
        return base;
    }
    double unary() {
        skip();
        if (*p == '-') { ++p; return -unary(); }
        if (*p == '+') { ++p; return unary(); }
        if (*p == '(') {
            ++p;
            const double v = expr();
            skip();
            if (*p == ')') ++p;
            else ok = false;
            return v;
        }
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) { ok = false; return 0; }
        p = end;
        return v;
    }
};

bool evalExpr(const char* s, float* out) {
    ExprParser ep{s};
    const double v = ep.parse();
    if (!ep.ok || !std::isfinite(v)) return false;
    *out = static_cast<float>(v);
    return true;
}

// Shared focus/scrub state across all numeric fields.
struct NumFieldGlobal {
    int focusId = -1;        // field being text-edited
    int pendingFocus = -1;   // Tab target
    char buf[64] = {};
    int scrubId = -1;        // field being scrubbed
    bool scrubMoved = false;
    float scrubStart = 0;    // value at scrub begin
    float scrubAccum = 0;
    bool armed = false;      // mouse down on field, deciding click vs scrub
    int armedId = -1;
    Vector2 armedPos{};
};
NumFieldGlobal& nf() {
    static NumFieldGlobal s;
    return s;
}

struct NumFieldOpts {
    float step = 1.0f;
    float minV = -1e9f;
    float maxV = 1e9f;
    bool mixed = false;      // multi-selection with differing values
    int decimals = 0;
};

// Returns true on commit; *outValue receives the new value. `current` is the
// node's live value (display + scrub baseline). beginScrub/endScrub bracket
// undo capture; onScrub applies live values during the drag.
template <typename BeginFn, typename ScrubFn, typename EndFn>
bool numField(EditorState& ed, int id, Rectangle r, const char* label, float current,
              float* outValue, const NumFieldOpts& opts, BeginFn&& beginScrub,
              ScrubFn&& onScrub, EndFn&& endScrub) {
    NumFieldGlobal& g = nf();
    const Vector2 mouse = GetMousePosition();
    const bool hover = CheckCollisionPointRec(mouse, r);
    const bool editing = g.focusId == id;
    bool committed = false;

    // Tab handoff: a previous field requested focus on this one.
    if (g.pendingFocus == id) {
        g.pendingFocus = -1;
        g.focusId = id;
        std::snprintf(g.buf, sizeof(g.buf), "%.*f", opts.decimals, current);
    }

    // ---- visuals ----
    const ::Color border = editing ? kThemeAccent
                           : hover ? ::Color{90, 160, 230, 255}
                                   : ::Color{58, 58, 58, 255};
    DrawRectangleRounded(r, 0.25f, 4, kThemeField);
    DrawRectangleRoundedLinesEx(r, 0.25f, 4, 1, border);
    if (label && *label) {
        uiText(label, r.x + ui(7), r.y + (r.height - fontS()) / 2, fontS(), kTextDim);
    }
    const float textX = r.x + (label && *label ? ui(24) : ui(8));

    if (editing) {
        ed.textEditActive = true;
        // text input
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            const size_t len = std::strlen(g.buf);
            if (ch >= 32 && ch < 127 && len + 1 < sizeof(g.buf)) {
                g.buf[len] = static_cast<char>(ch);
                g.buf[len + 1] = 0;
            }
        }
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) &&
            g.buf[0] != 0) {
            g.buf[std::strlen(g.buf) - 1] = 0;
        }

        const bool stepUp = IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP);
        const bool stepDown = IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
        if (stepUp || stepDown) {
            const float mult = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
                                   ? 10.0f
                                   : 1.0f;
            float v = current;
            evalExpr(g.buf, &v);
            v += (stepUp ? 1 : -1) * opts.step * mult;
            v = std::max(opts.minV, std::min(opts.maxV, v));
            std::snprintf(g.buf, sizeof(g.buf), "%.*f", opts.decimals, v);
            *outValue = v;
            committed = true;  // live-apply while stepping (undo handled by caller)
        }

        const bool tab = IsKeyPressed(KEY_TAB);
        const bool enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
        const bool clickAway = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !hover;
        if (enter || tab || clickAway) {
            float v;
            if (evalExpr(g.buf, &v)) {
                v = std::max(opts.minV, std::min(opts.maxV, v));
                if (v != current) {
                    *outValue = v;
                    committed = true;
                }
            }
            g.focusId = -1;
            if (tab) g.pendingFocus = id + 1;  // fields are numbered in tab order
        }
        if (IsKeyPressed(KEY_ESCAPE)) g.focusId = -1;

        // caret + text
        uiText(g.buf, textX, r.y + (r.height - fontM()) / 2, fontM(), kTextCol);
        if (std::fmod(GetTime(), 1.0) < 0.55) {
            const float cw = uiMeasure(g.buf, fontM());
            DrawRectangle(static_cast<int>(textX + cw + 1),
                          static_cast<int>(r.y + ui(5)), static_cast<int>(ui(1.5f)),
                          static_cast<int>(r.height - ui(10)), kThemeAccent);
        }
        return committed;
    }

    // ---- display ----
    char disp[32];
    if (opts.mixed) std::snprintf(disp, sizeof(disp), "Mixed");
    else std::snprintf(disp, sizeof(disp), "%.*f", opts.decimals, current);
    uiText(disp, textX, r.y + (r.height - fontM()) / 2, fontM(),
           opts.mixed ? kTextDim : kTextCol);

    // ---- click vs scrub ----
    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g.armed = true;
        g.armedId = id;
        g.armedPos = mouse;
    }
    if (g.armed && g.armedId == id) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            if (g.scrubId != id && std::fabs(mouse.x - g.armedPos.x) > 3) {
                g.scrubId = id;  // crossed the drag threshold → scrub
                g.scrubMoved = false;
                g.scrubAccum = opts.mixed ? 0 : current;
                beginScrub();
            }
        } else {
            // released
            if (g.scrubId == id) {
                endScrub(g.scrubMoved);
                g.scrubId = -1;
            } else {
                g.focusId = id;  // plain click → edit
                if (opts.mixed) g.buf[0] = 0;
                else std::snprintf(g.buf, sizeof(g.buf), "%.*f", opts.decimals, current);
            }
            g.armed = false;
            g.armedId = -1;
        }
    }
    if (g.scrubId == id && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        const float dx = GetMouseDelta().x;
        if (dx != 0) {
            const float mult = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
                                   ? 10.0f
                                   : 1.0f;
            g.scrubAccum += dx * opts.step * mult * 0.5f;
            const float v =
                std::max(opts.minV, std::min(opts.maxV, std::round(g.scrubAccum)));
            g.scrubMoved = true;
            onScrub(v);
        }
        SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
    }
    return committed;
}

struct HexState {
    bool edit = false;
    char buf[16] = {};
};
std::unordered_map<int, HexState>& hexStates() {
    static std::unordered_map<int, HexState> s;
    return s;
}

}  // namespace

void drawInspector(EditorState& ed) {
    const int sw = GetScreenWidth(), sh = GetScreenHeight();
    const int px = sw - kInspectorW;
    DrawRectangle(px, kToolbarH, kInspectorW, sh - kToolbarH, kPanelBg);
    DrawLine(px, kToolbarH, px, sh, kPanelEdge);

    ed.textEditActive = false;

    const Vector2 mouse = GetMousePosition();
    const bool mouseInPanel = mouse.x >= px && mouse.y > kToolbarH;
    if (mouseInPanel) ed.inspectorScroll -= GetMouseWheelMove() * ui(44);
    ed.inspectorScroll = std::max(0.0f, ed.inspectorScroll);

    static Node* lastNode = nullptr;
    Node* n = ed.selection.size() == 1 ? ed.selection[0] : nullptr;
    static int pickerSection = -1, pickerIndex = -1;  // 0 = fills, 1 = strokes
    static bool pickerDragging = false;
    static std::vector<NodeProps> pickerBefore;
    if (lastNode != n) {
        lastNode = n;
        nf().focusId = -1;
        nf().pendingFocus = -1;
        nf().scrubId = -1;
        nf().armed = false;
        hexStates().clear();
        pickerSection = pickerIndex = -1;
        pickerDragging = false;
        ed.inspectorScroll = 0;
    }
    const int pendingAtStart = nf().pendingFocus;

    BeginScissorMode(px, kToolbarH, kInspectorW, sh - kToolbarH);
    float y = kToolbarH + ui(12) - ed.inspectorScroll;
    const float fx = px + ui(14);

    // ---- alignment row ----
    {
        const float bw = ui(32), bh2 = ui(26), gap = ui(4);
        float ax = fx;
        const EditorState::Align ops[6] = {
            EditorState::Align::Left,    EditorState::Align::HCenter,
            EditorState::Align::Right,   EditorState::Align::Top,
            EditorState::Align::VCenter, EditorState::Align::Bottom};
        for (int i = 0; i < 6; ++i) {
            const Rectangle r{ax, y, bw, bh2};
            if (GuiButton(r, "") && !ed.selection.empty()) ed.alignSelection(ops[i]);
            drawAlignGlyph(r, i);
            ax += bw + gap + (i == 2 ? ui(10) : 0);
        }
        y += bh2 + ui(12);
    }

    if (ed.selection.empty()) {
        uiText("No selection", fx, y, fontS(), kTextDim);
        EndScissorMode();
        return;
    }
    auto& sel = ed.selection;

    // Multi-select aware property field: shows "Mixed" when values differ,
    // edits/scrubs apply to every selected node, one undo entry per commit.
    static std::vector<NodeProps> scrubBefore;
    auto propField = [&](int id, Rectangle r, const char* label, auto getter, auto setter,
                         NumFieldOpts opts) {
        const float first = getter(*sel[0]);
        bool mixed = false;
        for (Node* m : sel) {
            if (std::fabs(getter(*m) - first) > 0.01f) {
                mixed = true;
                break;
            }
        }
        opts.mixed = mixed && sel.size() > 1;
        auto applyAll = [&](float v) {
            for (Node* m : sel) {
                setter(*m, v);
                ed.bumpNode(m);
            }
            ed.docDirty = true;
        };
        float out;
        const bool committed = numField(
            ed, id, r, label, first, &out, opts,
            [&] {
                scrubBefore.clear();
                for (Node* m : sel) scrubBefore.push_back(NodeProps::capture(m));
            },
            applyAll,
            [&](bool moved) {
                if (moved && !scrubBefore.empty()) ed.pushPropsUndo(std::move(scrubBefore));
                scrubBefore.clear();
            });
        if (committed) {
            std::vector<NodeProps> before;
            for (Node* m : sel) before.push_back(NodeProps::capture(m));
            applyAll(out);
            ed.pushPropsUndo(std::move(before));
        }
    };

    if (n) {
        uiText(n->name.c_str(), fx, y, fontM(), kTextCol);
    } else {
        char msg[32];
        std::snprintf(msg, sizeof(msg), "%zu selected", sel.size());
        uiText(msg, fx, y, fontM(), kTextCol);
    }
    y += ui(28);

    // ---- position / size (multi-select aware) ----
    {
        NumFieldOpts iOpts;
        propField(100, {fx, y, ui(104), ui(26)}, "X",
                  [](Node& m) { return m.relativeTransform.m02; },
                  [](Node& m, float v) { m.relativeTransform.m02 = std::round(v); }, iOpts);
        propField(101, {fx + ui(112), y, ui(104), ui(26)}, "Y",
                  [](Node& m) { return m.relativeTransform.m12; },
                  [](Node& m, float v) { m.relativeTransform.m12 = std::round(v); }, iOpts);
        y += ui(32);
        NumFieldOpts sizeOpts;
        sizeOpts.minV = 1;
        propField(102, {fx, y, ui(104), ui(26)}, "W",
                  [](Node& m) { return worldBounds(m).w(); },
                  [](Node& m, float v) { setNodeSize(m, std::round(v), worldBounds(m).h()); },
                  sizeOpts);
        propField(103, {fx + ui(112), y, ui(104), ui(26)}, "H",
                  [](Node& m) { return worldBounds(m).h(); },
                  [](Node& m, float v) { setNodeSize(m, worldBounds(m).w(), std::round(v)); },
                  sizeOpts);
        y += ui(32);
        NumFieldOpts rOpts;
        rOpts.minV = 0;
        propField(104, {fx, y, ui(104), ui(26)}, "R", [](Node& m) { return m.cornerRadius; },
                  [](Node& m, float v) {
                      m.cornerRadius = std::max(0.0f, std::round(v));
                      m.rectangleCornerRadii.reset();
                  },
                  rOpts);
        NumFieldOpts opOpts;
        opOpts.minV = 0;
        opOpts.maxV = 100;
        propField(105, {fx + ui(112), y, ui(104), ui(26)}, "O",
                  [](Node& m) { return m.opacity * 100.0f; },
                  [](Node& m, float v) {
                      m.opacity = std::max(0.0f, std::min(100.0f, v)) / 100.0f;
                  },
                  opOpts);
        y += ui(38);
    }

    if (!n) {  // multi-selection: paint/text sections need a single node
        EndScissorMode();
        const float contentBottomM = y + ed.inspectorScroll;
        const float maxScrollM =
            std::max(0.0f, contentBottomM - static_cast<float>(sh) + ui(20));
        ed.inspectorScroll = std::min(ed.inspectorScroll, maxScrollM);
        return;
    }

    // Single-node numeric field with scrub + expression support.
    static std::vector<NodeProps> sfBefore;
    auto simpleField = [&](int id, Rectangle r, const char* label, float current,
                           NumFieldOpts opts, auto apply) {
        auto applyOne = [&](float v) {
            apply(v);
            ed.bumpNode(n);
            ed.docDirty = true;
        };
        float out;
        const bool committed = numField(
            ed, id, r, label, current, &out, opts,
            [&] { sfBefore = {NodeProps::capture(n)}; }, applyOne,
            [&](bool moved) {
                if (moved && !sfBefore.empty()) ed.pushPropsUndo(std::move(sfBefore));
                sfBefore.clear();
            });
        if (committed) {
            std::vector<NodeProps> before{NodeProps::capture(n)};
            applyOne(out);
            ed.pushPropsUndo(std::move(before));
        }
    };

    // ---- Fill / Stroke lists ----
    auto paintSection = [&](const char* title, int sectionId) -> bool {
        sectionHeader(fx, y, title);
        if (GuiLabelButton({static_cast<float>(px) + kInspectorW - ui(34), y - ui(28),
                            ui(20), ui(20)},
                           "+")) {
            commitEdit(ed, n, [&](Node& node) {
                figmalib::Paint p;
                p.type = figmalib::PaintType::Solid;
                p.color = {0.8f, 0.8f, 0.8f, 1.0f};
                (sectionId == 0 ? node.fills : node.strokes).push_back(p);
            });
        }
        auto& paints = sectionId == 0 ? n->fills : n->strokes;
        for (size_t i = 0; i < paints.size(); ++i) {
            figmalib::Paint& p = paints[i];
            const int rowId = 200 + sectionId * 60 + static_cast<int>(i) * 3;

            if (p.type != figmalib::PaintType::Solid) {
                uiText(p.type == figmalib::PaintType::Image ? "Image" : "Gradient", fx,
                       y + ui(4), fontS(), kTextDim);
                if (GuiLabelButton({fx + ui(168), y, ui(20), ui(24)},
                                   p.visible ? "o" : "-")) {
                    commitEdit(ed, n, [&](Node& node) {
                        auto& list = sectionId == 0 ? node.fills : node.strokes;
                        if (i < list.size()) list[i].visible = !list[i].visible;
                    });
                }
                y += ui(28);
                continue;
            }

            const bool open =
                pickerSection == sectionId && pickerIndex == static_cast<int>(i);

            // swatch
            const Rectangle swr{fx, y + ui(2), ui(20), ui(20)};
            DrawRectangleRec(swr, toRl(p.color));
            DrawRectangleLinesEx(swr, 1, open ? kAccent : ::Color{90, 90, 90, 255});
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                CheckCollisionPointRec(mouse, swr)) {
                pickerSection = open ? -1 : sectionId;
                pickerIndex = open ? -1 : static_cast<int>(i);
            }

            // hex
            HexState& hx = hexStates()[rowId];
            if (!hx.edit)
                std::snprintf(hx.buf, sizeof(hx.buf), "%s", colorToHex(p.color).c_str());
            if (GuiTextBox({fx + ui(28), y, ui(76), ui(24)}, hx.buf, sizeof(hx.buf),
                           hx.edit)) {
                if (hx.edit) {
                    figmalib::Color c = p.color;
                    if (hexToColor(hx.buf, c)) {
                        commitEdit(ed, n, [&](Node& node) {
                            auto& list = sectionId == 0 ? node.fills : node.strokes;
                            if (i < list.size()) {
                                list[i].color.r = c.r;
                                list[i].color.g = c.g;
                                list[i].color.b = c.b;
                            }
                        });
                    }
                }
                hx.edit = !hx.edit;
            }
            if (hx.edit) ed.textEditActive = true;

            // opacity %
            NumFieldOpts pctOpts;
            pctOpts.minV = 0;
            pctOpts.maxV = 100;
            simpleField(rowId + 1, {fx + ui(110), y, ui(52), ui(24)}, nullptr,
                        std::lround(p.opacity * 100) * 1.0f, pctOpts, [&](float v) {
                            auto& list = sectionId == 0 ? n->fills : n->strokes;
                            if (i < list.size()) list[i].opacity = v / 100.0f;
                        });

            // visibility / remove
            if (GuiLabelButton({fx + ui(168), y, ui(20), ui(24)}, p.visible ? "o" : "-")) {
                commitEdit(ed, n, [&](Node& node) {
                    auto& list = sectionId == 0 ? node.fills : node.strokes;
                    if (i < list.size()) list[i].visible = !list[i].visible;
                });
            }
            if (GuiLabelButton({fx + ui(192), y, ui(20), ui(24)}, "x")) {
                commitEdit(ed, n, [&](Node& node) {
                    auto& list = sectionId == 0 ? node.fills : node.strokes;
                    if (i < list.size())
                        list.erase(list.begin() + static_cast<long long>(i));
                });
                pickerSection = -1;
                pickerIndex = -1;
                EndScissorMode();
                return false;  // list mutated — redraw next frame
            }
            y += ui(28);

            // inline color picker for the open row
            if (open) {
                ::Color c = toRl(p.color);
                GuiColorPicker({fx, y, ui(180), ui(150)}, nullptr, &c);
                const Rectangle pickAll{fx, y, ui(214), ui(150)};
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                    CheckCollisionPointRec(mouse, pickAll)) {
                    pickerDragging = true;
                    pickerBefore = {NodeProps::capture(n)};
                }
                const figmalib::Color nc{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
                                         p.color.a};
                if (pickerDragging &&
                    (nc.r != p.color.r || nc.g != p.color.g || nc.b != p.color.b)) {
                    p.color = nc;
                    ed.bumpNode(n);
                    ed.docDirty = true;
                }
                if (pickerDragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    pickerDragging = false;
                    if (!pickerBefore.empty()) ed.pushPropsUndo(std::move(pickerBefore));
                    pickerBefore.clear();
                }
                y += ui(158);
            }
        }
        if (paints.empty()) y += ui(2);
        y += ui(6);
        return true;
    };

    if (!paintSection("Fill", 0)) return;
    if (!paintSection("Stroke", 1)) return;

    // stroke weight + align
    if (!n->strokes.empty()) {
        uiText("Weight", fx, y + ui(6), fontS(), kTextDim);
        NumFieldOpts wOpts;
        wOpts.minV = 0;
        simpleField(300, {fx + ui(52), y, ui(52), ui(26)}, nullptr, n->strokeWeight, wOpts,
                    [&](float v) { n->strokeWeight = std::max(0.0f, v); });
        int alignIdx = n->strokeAlign == figmalib::StrokeAlign::Inside   ? 0
                       : n->strokeAlign == figmalib::StrokeAlign::Center ? 1
                                                                         : 2;
        const int prev = alignIdx;
        GuiComboBox({fx + ui(112), y, ui(100), ui(26)}, "Inside;Center;Outside", &alignIdx);
        if (alignIdx != prev) {
            commitEdit(ed, n, [&](Node& node) {
                node.strokeAlign = alignIdx == 0   ? figmalib::StrokeAlign::Inside
                                   : alignIdx == 1 ? figmalib::StrokeAlign::Center
                                                   : figmalib::StrokeAlign::Outside;
            });
        }
        y += ui(34);
    }

    // ---- Text ----
    if (n->type == figmalib::NodeType::Text) {
        sectionHeader(fx, y, "Text");
        uiText("Size", fx, y + ui(6), fontS(), kTextDim);
        NumFieldOpts tOpts;
        tOpts.minV = 1;
        simpleField(400, {fx + ui(36), y, ui(56), ui(26)}, nullptr, n->textStyle.fontSize,
                    tOpts, [&](float v) { n->textStyle.fontSize = std::max(1.0f, v); });
        y += ui(32);

        using AH = figmalib::TextStyle::AlignH;
        using AV = figmalib::TextStyle::AlignV;
        const char* hLabels[3] = {"L", "C", "R"};
        const AH hVals[3] = {AH::Left, AH::Center, AH::Right};
        const char* vLabels[3] = {"T", "M", "B"};
        const AV vVals[3] = {AV::Top, AV::Center, AV::Bottom};
        for (int i = 0; i < 3; ++i) {
            const Rectangle r{fx + i * ui(34), y, ui(30), ui(26)};
            if (n->textStyle.alignH == hVals[i]) DrawRectangleRec(r, {13, 153, 255, 70});
            if (GuiButton(r, hLabels[i])) {
                const AH v = hVals[i];
                commitEdit(ed, n, [&](Node& node) { node.textStyle.alignH = v; });
            }
        }
        for (int i = 0; i < 3; ++i) {
            const Rectangle r{fx + ui(120) + i * ui(34), y, ui(30), ui(26)};
            if (n->textStyle.alignV == vVals[i]) DrawRectangleRec(r, {13, 153, 255, 70});
            if (GuiButton(r, vLabels[i])) {
                const AV v = vVals[i];
                commitEdit(ed, n, [&](Node& node) { node.textStyle.alignV = v; });
            }
        }
        y += ui(34);

        static char buf[512] = {};
        static bool editText = false;
        static Node* lastTextNode = nullptr;
        if (lastTextNode != n) {
            lastTextNode = n;
            std::snprintf(buf, sizeof(buf), "%s", n->characters.c_str());
            editText = false;
        }
        if (!editText) std::snprintf(buf, sizeof(buf), "%s", n->characters.c_str());
        if (GuiTextBox({fx, y, ui(214), ui(28)}, buf, sizeof(buf), editText)) {
            if (editText && n->characters != buf) {
                const std::string text = buf;
                commitEdit(ed, n, [&](Node& node) {
                    node.characters = text;
                    node.textRuns.clear();
                });
            }
            editText = !editText;
        }
        if (editText) ed.textEditActive = true;
        y += ui(36);
    }

    // ---- Effects ----
    if (!n->effects.empty()) {
        sectionHeader(fx, y, "Effects");
        for (size_t i = 0; i < n->effects.size(); ++i) {
            const figmalib::Effect& fxE = n->effects[i];
            const char* name =
                fxE.type == figmalib::Effect::Type::DropShadow    ? "Drop shadow"
                : fxE.type == figmalib::Effect::Type::InnerShadow ? "Inner shadow"
                : fxE.type == figmalib::Effect::Type::LayerBlur   ? "Layer blur"
                                                                  : "Background blur";
            uiText(name, fx, y + ui(3), fontS(), kTextCol);
            if (GuiLabelButton({fx + ui(168), y, ui(20), ui(22)},
                               fxE.visible ? "o" : "-")) {
                commitEdit(ed, n, [&](Node& node) {
                    if (i < node.effects.size())
                        node.effects[i].visible = !node.effects[i].visible;
                });
            }
            y += ui(26);
        }
        y += ui(8);
    }

    EndScissorMode();

    // Tab landed past the last field this frame → drop the dangling request.
    if (nf().pendingFocus != -1 && nf().pendingFocus == pendingAtStart) {
        nf().pendingFocus = -1;
    }

    // clamp scroll to content height
    const float contentBottom = y + ed.inspectorScroll;
    const float maxScroll = std::max(0.0f, contentBottom - static_cast<float>(sh) + ui(20));
    ed.inspectorScroll = std::min(ed.inspectorScroll, maxScroll);
}

}  // namespace figmaedit
