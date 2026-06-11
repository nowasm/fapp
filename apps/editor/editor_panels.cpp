// Toolbar, layers tree and inspector. raygui for widgets, hand-rolled rows
// for the layers tree (raygui has no tree view).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "editor.h"

#define RAYGUI_IMPLEMENTATION
#include <raygui.h>

namespace figmaedit {

namespace {

constexpr ::Color kPanelBg{43, 43, 43, 255};
constexpr ::Color kPanelEdge{30, 30, 30, 255};
constexpr ::Color kTextCol{220, 220, 220, 255};
constexpr ::Color kTextDim{140, 140, 140, 255};
constexpr ::Color kRowSel{13, 153, 255, 70};
constexpr ::Color kRowHover{255, 255, 255, 14};

// One undoable property edit on a single node.
template <typename Fn>
void commitEdit(EditorState& ed, Node* n, Fn&& fn) {
    std::vector<NodeProps> before{NodeProps::capture(n)};
    fn(*n);
    ed.pushPropsUndo(std::move(before));
    ed.markDocChanged();
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

const char* nodeIcon(const Node& n) {
    switch (n.type) {
    case figmalib::NodeType::Frame:
    case figmalib::NodeType::Component:
    case figmalib::NodeType::Instance: return "#";
    case figmalib::NodeType::Group: return "[]";
    case figmalib::NodeType::Text: return "T";
    case figmalib::NodeType::Ellipse: return "O";
    case figmalib::NodeType::Vector:
    case figmalib::NodeType::BooleanOperation:
    case figmalib::NodeType::Star:
    case figmalib::NodeType::RegularPolygon: return "~";
    default: return "::";
    }
}

}  // namespace

void drawToolbar(EditorState& ed) {
    const float w = static_cast<float>(GetScreenWidth());
    DrawRectangle(0, 0, static_cast<int>(w), kToolbarH, kPanelBg);
    DrawLine(0, kToolbarH, static_cast<int>(w), kToolbarH, kPanelEdge);

    float x = ui(8);
    const float bh = ui(28), by = (kToolbarH - bh) / 2;

    // ---- File menu ----
    const Rectangle fileBtn{x, by, ui(52), bh};
    if (GuiButton(fileBtn, "File")) ed.fileMenuOpen = !ed.fileMenuOpen;
    x += ui(64);
    if (ed.fileMenuOpen) {
        struct Item {
            const char* label;
            int action;  // 1 open, 2 save, 3 save as
        };
        const Item items[] = {
            {"Open...        Ctrl+O", 1},
            {"Save           Ctrl+S", 2},
            {"Save As...     Ctrl+Shift+S", 3},
        };
        const float mw = ui(230), ih = ui(30);
        const Rectangle menuRect{fileBtn.x, static_cast<float>(kToolbarH),
                                 mw, ih * static_cast<float>(std::size(items)) + ui(8)};
        DrawRectangleRec(menuRect, kPanelBg);
        DrawRectangleLinesEx(menuRect, 1, kPanelEdge);
        float iy = menuRect.y + ui(4);
        int action = 0;
        for (const Item& item : items) {
            if (GuiLabelButton({menuRect.x + ui(8), iy, mw - ui(16), ih}, item.label)) {
                action = item.action;
            }
            iy += ih;
        }
        if (action != 0) {
            ed.fileMenuOpen = false;
            switch (action) {
            case 1: {
                const std::string path = showOpenFileDialog();
                if (!path.empty()) openFile(ed, path);
                break;
            }
            case 2: saveFile(ed); break;
            case 3: saveFileAs(ed); break;
            }
        } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                   !CheckCollisionPointRec(GetMousePosition(), menuRect) &&
                   !CheckCollisionPointRec(GetMousePosition(), fileBtn)) {
            ed.fileMenuOpen = false;  // click-away closes
        }
    }

    const ::Color activeTint{13, 153, 255, 255};
    auto toolButton = [&](const char* label, Tool t) {
        const bool active = ed.tool == t;
        if (active)
            DrawRectangleRounded({x - 2, by - 2, ui(60), bh + 4}, 0.3f, 4, activeTint);
        if (GuiButton({x, by, ui(56), bh}, label)) ed.tool = t;
        x += ui(64);
    };
    toolButton("Move V", Tool::Move);
    toolButton("Hand H", Tool::Hand);
    x += ui(12);

    if (GuiButton({x, by, ui(52), bh}, "Save")) saveFile(ed);
    x += ui(60);
    if (GuiButton({x, by, ui(52), bh}, "Undo")) ed.undo();
    x += ui(60);
    if (GuiButton({x, by, ui(52), bh}, "Redo")) ed.redo();
    x += ui(72);

    // page switcher
    if (GuiButton({x, by, ui(28), bh}, "<")) ed.selectPage(ed.pageIndex - 1);
    x += ui(32);
    const std::string pageLabel =
        (ed.page ? ed.page->name : "-") + (ed.unsaved ? " *" : "");
    DrawText(pageLabel.c_str(), static_cast<int>(x), static_cast<int>(by + ui(7)), fontM(),
             kTextCol);
    x += static_cast<float>(MeasureText(pageLabel.c_str(), fontM())) + ui(10);
    if (GuiButton({x, by, ui(28), bh}, ">")) ed.selectPage(ed.pageIndex + 1);
    x += ui(44);

    char zoom[32];
    std::snprintf(zoom, sizeof(zoom), "%.0f%%", ed.cam.zoom * 100);
    DrawText(zoom, static_cast<int>(x), static_cast<int>(by + ui(7)), fontM(), kTextDim);

    // status (right-aligned)
    if (GetTime() < ed.statusUntil && !ed.status.empty()) {
        const int tw = MeasureText(ed.status.c_str(), fontS());
        DrawText(ed.status.c_str(), static_cast<int>(w) - tw - kInspectorW - ui(16),
                 static_cast<int>(by + ui(8)), fontS(), kTextDim);
    }
}

// ---- layers tree ------------------------------------------------------------

namespace {

struct LayerRowCtx {
    EditorState* ed;
    float y;            // current row y (already scrolled)
    float panelTop, panelBottom;
    bool mouseInPanel;
    float mx, my;
    bool clicked;
};

void drawLayerRows(LayerRowCtx& c, Node& n, int depth) {
    const float rowH = ui(22);
    EditorState& ed = *c.ed;

    if (c.y + rowH >= c.panelTop && c.y <= c.panelBottom) {
        const Rectangle row{0, c.y, static_cast<float>(kLayersW), rowH};
        const bool hover = c.mouseInPanel && c.my >= row.y && c.my < row.y + rowH;
        if (ed.isSelected(&n)) DrawRectangleRec(row, kRowSel);
        else if (hover) DrawRectangleRec(row, kRowHover);

        const float indent = ui(10.0f + depth * 14.0f);
        const bool container = !n.children.empty();
        const int ty = static_cast<int>(c.y + ui(5));

        // expand arrow
        if (container) {
            const bool open = ed.expanded.count(&n) > 0;
            DrawText(open ? "v" : ">", static_cast<int>(indent), ty, fontS(), kTextDim);
            if (hover && c.clicked && c.mx >= indent - ui(4) && c.mx <= indent + ui(12)) {
                if (open) ed.expanded.erase(&n);
                else ed.expanded.insert(&n);
                c.clicked = false;
            }
        }
        // icon + name
        DrawText(nodeIcon(n), static_cast<int>(indent + ui(16)), ty, fontS(), kTextDim);
        const ::Color nameCol = n.visible ? kTextCol : kTextDim;
        DrawText(n.name.c_str(), static_cast<int>(indent + ui(38)), ty, fontS(), nameCol);

        // visibility eye
        const float eyeX = kLayersW - ui(26.0f);
        DrawText(n.visible ? "o" : "-", static_cast<int>(eyeX), ty, fontS(), kTextDim);
        if (hover && c.clicked && c.mx >= eyeX - ui(6) && c.mx <= eyeX + ui(14)) {
            commitEdit(ed, &n, [](Node& node) { node.visible = !node.visible; });
            c.clicked = false;
        }

        // select
        if (hover && c.clicked) {
            ed.selection = {&n};
            ed.scope = n.parent ? n.parent : ed.page;
            c.clicked = false;
        }
    }
    c.y += rowH;

    if (!n.children.empty() && ed.expanded.count(&n) > 0) {
        // Figma lists layers top-of-stack first.
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
            drawLayerRows(c, **it, depth + 1);
        }
    }
}

float treeHeight(EditorState& ed, Node& n) {
    float h = ui(22);
    if (!n.children.empty() && ed.expanded.count(&n) > 0) {
        for (auto& ch : n.children) h += treeHeight(ed, *ch);
    }
    return h;
}

}  // namespace

void drawLayersPanel(EditorState& ed) {
    const int sh = GetScreenHeight();
    DrawRectangle(0, kToolbarH, kLayersW, sh - kToolbarH, kPanelBg);
    DrawLine(kLayersW, kToolbarH, kLayersW, sh, kPanelEdge);
    if (!ed.page) return;

    const float mx = static_cast<float>(GetMouseX());
    const float my = static_cast<float>(GetMouseY());
    const bool mouseInPanel = mx < kLayersW && my > kToolbarH;

    if (mouseInPanel) {
        ed.layersScroll -= GetMouseWheelMove() * ui(44);
    }

    BeginScissorMode(0, kToolbarH, kLayersW, sh - kToolbarH);
    LayerRowCtx c{};
    c.ed = &ed;
    c.y = kToolbarH + ui(6) - ed.layersScroll;
    c.panelTop = kToolbarH;
    c.panelBottom = static_cast<float>(sh);
    c.mouseInPanel = mouseInPanel;
    c.mx = mx;
    c.my = my;
    c.clicked = mouseInPanel && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

    float total = 0;
    for (auto it = ed.page->children.rbegin(); it != ed.page->children.rend(); ++it) {
        drawLayerRows(c, **it, 0);
    }
    for (auto& ch : ed.page->children) total += treeHeight(ed, *ch);
    EndScissorMode();

    const float maxScroll = std::max(0.0f, total - (sh - kToolbarH - 12));
    ed.layersScroll = std::max(0.0f, std::min(ed.layersScroll, maxScroll));
}

// ---- inspector ----------------------------------------------------------------

void drawInspector(EditorState& ed) {
    const int sw = GetScreenWidth(), sh = GetScreenHeight();
    const int px = sw - kInspectorW;
    DrawRectangle(px, kToolbarH, kInspectorW, sh - kToolbarH, kPanelBg);
    DrawLine(px, kToolbarH, px, sh, kPanelEdge);

    ed.textEditActive = false;
    if (ed.selection.size() != 1) {
        const char* msg = ed.selection.empty() ? "No selection" : "Multiple selection";
        DrawText(msg, px + static_cast<int>(ui(16)), kToolbarH + static_cast<int>(ui(18)),
                 fontS(), kTextDim);
        return;
    }
    Node* n = ed.selection[0];
    float y = kToolbarH + ui(12.0f);
    const float fx = px + ui(14.0f);

    DrawText(n->name.c_str(), static_cast<int>(fx), static_cast<int>(y), fontM(), kTextCol);
    y += ui(26);

    // ---- X / Y / W / H value boxes ----
    static bool editing[4] = {};
    static int values[4] = {};
    static Node* lastNode = nullptr;
    const WorldRect wr = worldBounds(*n);
    const int current[4] = {
        static_cast<int>(std::lround(n->relativeTransform.m02)),
        static_cast<int>(std::lround(n->relativeTransform.m12)),
        static_cast<int>(std::lround(wr.w())),
        static_cast<int>(std::lround(wr.h())),
    };
    if (lastNode != n) {
        lastNode = n;
        for (int i = 0; i < 4; ++i) {
            values[i] = current[i];
            editing[i] = false;
        }
    }
    const char* labels[4] = {"X", "Y", "W", "H"};
    for (int i = 0; i < 4; ++i) {
        const float bx = fx + (i % 2) * ui(124.0f);
        const float byy = y + (i / 2) * ui(34.0f);
        if (!editing[i]) values[i] = current[i];
        DrawText(labels[i], static_cast<int>(bx), static_cast<int>(byy + ui(7)), fontS(),
                 kTextDim);
        if (GuiValueBox({bx + ui(16), byy, ui(92), ui(26)}, nullptr, &values[i], -1000000,
                        1000000, editing[i])) {
            if (editing[i] && values[i] != current[i]) {
                const int v = values[i];
                commitEdit(ed, n, [&](Node& node) {
                    switch (i) {
                    case 0: node.relativeTransform.m02 = static_cast<float>(v); break;
                    case 1: node.relativeTransform.m12 = static_cast<float>(v); break;
                    case 2: setNodeSize(node, static_cast<float>(v), worldBounds(node).h());
                        break;
                    case 3: setNodeSize(node, worldBounds(node).w(), static_cast<float>(v));
                        break;
                    }
                });
            }
            editing[i] = !editing[i];
        }
        if (editing[i]) ed.textEditActive = true;
    }
    y += ui(76);

    // ---- opacity ----
    static bool opacityDragging = false;
    static std::vector<NodeProps> opacityBefore;
    float opacityPct = n->opacity * 100.0f;
    DrawText("Opacity", static_cast<int>(fx), static_cast<int>(y + ui(6)), fontS(),
             kTextDim);
    const Rectangle opRect{fx + ui(64), y, ui(150), ui(22)};
    GuiSlider(opRect, nullptr, TextFormat("%.0f%%", opacityPct), &opacityPct, 0, 100);
    const bool overOp = CheckCollisionPointRec(GetMousePosition(), opRect);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && overOp) {
        opacityDragging = true;
        opacityBefore = {NodeProps::capture(n)};
    }
    if (opacityDragging) {
        if (std::fabs(opacityPct / 100.0f - n->opacity) > 0.001f) {
            n->opacity = opacityPct / 100.0f;
            ed.markDocChanged();
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            opacityDragging = false;
            if (!opacityBefore.empty() &&
                std::fabs(opacityBefore[0].opacity - n->opacity) > 0.001f) {
                ed.pushPropsUndo(std::move(opacityBefore));
            }
            opacityBefore.clear();
        }
    }
    y += ui(34);

    // ---- corner radius ----
    if (n->type != figmalib::NodeType::Text) {
        static bool radiusDragging = false;
        static std::vector<NodeProps> radiusBefore;
        float radius = n->cornerRadius;
        const float maxR = std::max(1.0f, std::min(n->width, n->height) * 0.5f);
        DrawText("Radius", static_cast<int>(fx), static_cast<int>(y + ui(6)), fontS(),
                 kTextDim);
        const Rectangle rRect{fx + ui(64), y, ui(150), ui(22)};
        GuiSlider(rRect, nullptr, TextFormat("%.0f", radius), &radius, 0, maxR);
        const bool overR = CheckCollisionPointRec(GetMousePosition(), rRect);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && overR) {
            radiusDragging = true;
            radiusBefore = {NodeProps::capture(n)};
        }
        if (radiusDragging) {
            if (std::fabs(radius - n->cornerRadius) > 0.01f) {
                n->cornerRadius = radius;
                n->rectangleCornerRadii.reset();
                ed.markDocChanged();
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                radiusDragging = false;
                if (!radiusBefore.empty() &&
                    std::fabs(radiusBefore[0].cornerRadius - n->cornerRadius) > 0.01f) {
                    ed.pushPropsUndo(std::move(radiusBefore));
                }
                radiusBefore.clear();
            }
        }
        y += ui(34);
    }

    // ---- fill color (first solid paint) ----
    figmalib::Paint* solid = nullptr;
    for (auto& p : n->fills) {
        if (p.type == figmalib::PaintType::Solid) {
            solid = &p;
            break;
        }
    }
    if (solid) {
        DrawText("Fill", static_cast<int>(fx), static_cast<int>(y), fontS(), kTextDim);
        y += ui(18);
        static bool pickerDragging = false;
        static std::vector<NodeProps> pickerBefore;
        ::Color c{static_cast<unsigned char>(solid->color.r * 255),
                  static_cast<unsigned char>(solid->color.g * 255),
                  static_cast<unsigned char>(solid->color.b * 255),
                  static_cast<unsigned char>(solid->color.a * 255)};
        const Rectangle pickRect{fx, y, ui(180), ui(160)};
        GuiColorPicker(pickRect, nullptr, &c);
        const Rectangle pickAll{fx, y, ui(220), ui(160)};
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(), pickAll)) {
            pickerDragging = true;
            pickerBefore = {NodeProps::capture(n)};
        }
        const figmalib::Color nc{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
                                 solid->color.a};
        if (pickerDragging &&
            (nc.r != solid->color.r || nc.g != solid->color.g || nc.b != solid->color.b)) {
            solid->color = nc;
            ed.markDocChanged();
        }
        if (pickerDragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            pickerDragging = false;
            if (!pickerBefore.empty()) ed.pushPropsUndo(std::move(pickerBefore));
            pickerBefore.clear();
        }
        y += ui(172);
    }

    // ---- text content ----
    if (n->type == figmalib::NodeType::Text) {
        DrawText("Text", static_cast<int>(fx), static_cast<int>(y), fontS(), kTextDim);
        y += ui(18);
        static char buf[512] = {};
        static bool editText = false;
        static Node* lastTextNode = nullptr;
        if (lastTextNode != n) {
            lastTextNode = n;
            std::snprintf(buf, sizeof(buf), "%s", n->characters.c_str());
            editText = false;
        }
        if (!editText) std::snprintf(buf, sizeof(buf), "%s", n->characters.c_str());
        if (GuiTextBox({fx, y, ui(220), ui(28)}, buf, sizeof(buf), editText)) {
            if (editText && n->characters != buf) {
                const std::string text = buf;
                commitEdit(ed, n, [&](Node& node) {
                    node.characters = text;
                    node.textRuns.clear();  // uniform style after manual edit
                });
            }
            editText = !editText;
        }
        if (editText) ed.textEditActive = true;
        y += 40;
    }
}

}  // namespace figmaedit
