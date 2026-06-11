// Inspector panel — layout mirrors Figma's Design panel: alignment row,
// position/size, radius + opacity, Fill list, Stroke list (weight/align),
// Text properties and Effects toggles.

#include <cmath>
#include <cstdio>
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

// Editable integer field with deferred commit; id must be stable across frames.
struct FieldState {
    bool edit = false;
    int val = 0;
};
std::unordered_map<int, FieldState>& fieldStates() {
    static std::unordered_map<int, FieldState> s;
    return s;
}

bool intField(EditorState& ed, int id, Rectangle r, int current, int* out) {
    FieldState& f = fieldStates()[id];
    if (!f.edit) f.val = current;
    bool committed = false;
    if (GuiValueBox(r, nullptr, &f.val, -1000000, 1000000, f.edit)) {
        if (f.edit && f.val != current) {
            *out = f.val;
            committed = true;
        }
        f.edit = !f.edit;
    }
    if (f.edit) ed.textEditActive = true;
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
        fieldStates().clear();
        hexStates().clear();
        pickerSection = pickerIndex = -1;
        pickerDragging = false;
        ed.inspectorScroll = 0;
    }

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

    if (!n) {
        const char* msg = ed.selection.empty() ? "No selection" : "Multiple selection";
        uiText(msg, fx, y, fontS(), kTextDim);
        EndScissorMode();
        return;
    }

    uiText(n->name.c_str(), fx, y, fontM(), kTextCol);
    y += ui(28);

    // ---- position / size / radius / opacity ----
    {
        const WorldRect wr = worldBounds(*n);
        const char* labels[4] = {"X", "Y", "W", "H"};
        const int current[4] = {
            static_cast<int>(std::lround(n->relativeTransform.m02)),
            static_cast<int>(std::lround(n->relativeTransform.m12)),
            static_cast<int>(std::lround(wr.w())),
            static_cast<int>(std::lround(wr.h())),
        };
        for (int i = 0; i < 4; ++i) {
            const float bx = fx + (i % 2) * ui(120);
            const float byy = y + (i / 2) * ui(32);
            uiText(labels[i], bx, byy + ui(6), fontS(), kTextDim);
            int nv;
            if (intField(ed, 100 + i, {bx + ui(20), byy, ui(88), ui(26)}, current[i], &nv)) {
                const int which = i;
                commitEdit(ed, n, [&](Node& node) {
                    switch (which) {
                    case 0: node.relativeTransform.m02 = static_cast<float>(nv); break;
                    case 1: node.relativeTransform.m12 = static_cast<float>(nv); break;
                    case 2:
                        setNodeSize(node, static_cast<float>(nv), worldBounds(node).h());
                        break;
                    case 3:
                        setNodeSize(node, worldBounds(node).w(), static_cast<float>(nv));
                        break;
                    }
                });
            }
        }
        y += ui(64);

        uiText("R", fx, y + ui(6), fontS(), kTextDim);
        int nv;
        if (intField(ed, 110, {fx + ui(20), y, ui(88), ui(26)},
                     static_cast<int>(std::lround(n->cornerRadius)), &nv)) {
            commitEdit(ed, n, [&](Node& node) {
                node.cornerRadius = std::max(0.0f, static_cast<float>(nv));
                node.rectangleCornerRadii.reset();
            });
        }
        uiText("Op%", fx + ui(120), y + ui(6), fontS(), kTextDim);
        if (intField(ed, 111, {fx + ui(152), y, ui(56), ui(26)},
                     static_cast<int>(std::lround(n->opacity * 100)), &nv)) {
            commitEdit(ed, n, [&](Node& node) {
                node.opacity = std::max(0, std::min(100, nv)) / 100.0f;
            });
        }
        y += ui(38);
    }

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
            int pct;
            if (intField(ed, rowId + 1, {fx + ui(110), y, ui(52), ui(24)},
                         static_cast<int>(std::lround(p.opacity * 100)), &pct)) {
                const float op = std::max(0, std::min(100, pct)) / 100.0f;
                commitEdit(ed, n, [&](Node& node) {
                    auto& list = sectionId == 0 ? node.fills : node.strokes;
                    if (i < list.size()) list[i].opacity = op;
                });
            }

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
        int nv;
        if (intField(ed, 300, {fx + ui(52), y, ui(52), ui(26)},
                     static_cast<int>(std::lround(n->strokeWeight)), &nv)) {
            commitEdit(ed, n,
                       [&](Node& node) { node.strokeWeight = std::max(0, nv) * 1.0f; });
        }
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
        int nv;
        if (intField(ed, 400, {fx + ui(36), y, ui(56), ui(26)},
                     static_cast<int>(std::lround(n->textStyle.fontSize)), &nv)) {
            commitEdit(ed, n, [&](Node& node) {
                node.textStyle.fontSize = std::max(1, nv) * 1.0f;
            });
        }
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

    // clamp scroll to content height
    const float contentBottom = y + ed.inspectorScroll;
    const float maxScroll = std::max(0.0f, contentBottom - static_cast<float>(sh) + ui(20));
    ed.inspectorScroll = std::min(ed.inspectorScroll, maxScroll);
}

}  // namespace figmaedit
