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

void applyEditorTheme() {
    auto pack = [](::Color c) { return ColorToInt(c); };
    // Base look of every control (designed in Figma: dark fields, subtle
    // borders, accent on press).
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, pack(kThemePanel));
    GuiSetStyle(DEFAULT, LINE_COLOR, pack(kThemeLine));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, pack(kThemeField));
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, pack(::Color{58, 58, 58, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, pack(kThemeText));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, pack(::Color{51, 51, 51, 255}));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, pack(::Color{90, 160, 230, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, pack(::Color{255, 255, 255, 255}));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, pack(kThemeAccent));
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, pack(kThemeAccent));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, pack(::Color{255, 255, 255, 255}));
    GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);
    GuiSetStyle(BUTTON, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
    GuiSetStyle(VALUEBOX, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    GuiSetStyle(TEXTBOX, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
}

namespace {

constexpr ::Color kPanelBg = kThemePanel;
constexpr ::Color kPanelEdge = kThemeEdge;
constexpr ::Color kTextCol = kThemeText;
constexpr ::Color kTextDim = kThemeDim;
constexpr ::Color kRowSel = kThemeRowSel;
constexpr ::Color kRowHover = kThemeRowHover;

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

    auto toolButton = [&](const char* label, Tool t) {
        const bool active = ed.tool == t;
        if (active) {  // Figma-style: the active tool is a solid accent button
            GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt(kThemeAccent));
            GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt(kThemeAccent));
            GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(::Color{255, 255, 255, 255}));
        }
        if (GuiButton({x, by, ui(56), bh}, label)) ed.tool = t;
        if (active) {
            GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt(kThemeField));
            GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt(::Color{58, 58, 58, 255}));
            GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(kThemeText));
        }
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
    uiText(pageLabel.c_str(), static_cast<int>(x), static_cast<int>(by + ui(7)), fontM(),
             kTextCol);
    x += static_cast<float>(uiMeasure(pageLabel.c_str(), fontM())) + ui(10);
    if (GuiButton({x, by, ui(28), bh}, ">")) ed.selectPage(ed.pageIndex + 1);
    x += ui(44);

    char zoom[32];
    std::snprintf(zoom, sizeof(zoom), "%.0f%%", ed.cam.zoom * 100);
    uiText(zoom, static_cast<int>(x), static_cast<int>(by + ui(7)), fontM(), kTextDim);

    // status (right-aligned)
    if (GetTime() < ed.statusUntil && !ed.status.empty()) {
        const int tw = uiMeasure(ed.status.c_str(), fontS());
        uiText(ed.status.c_str(), static_cast<int>(w) - tw - kInspectorW - ui(16),
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
            uiText(open ? "v" : ">", static_cast<int>(indent), ty, fontS(), kTextDim);
            if (hover && c.clicked && c.mx >= indent - ui(4) && c.mx <= indent + ui(12)) {
                if (open) ed.expanded.erase(&n);
                else ed.expanded.insert(&n);
                c.clicked = false;
            }
        }
        // icon + name
        uiText(nodeIcon(n), static_cast<int>(indent + ui(16)), ty, fontS(), kTextDim);
        const ::Color nameCol = n.visible ? kTextCol : kTextDim;
        uiText(n.name.c_str(), static_cast<int>(indent + ui(38)), ty, fontS(), nameCol);

        // visibility eye
        const float eyeX = kLayersW - ui(26.0f);
        uiText(n.visible ? "o" : "-", static_cast<int>(eyeX), ty, fontS(), kTextDim);
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


}  // namespace figmaedit
