#pragma once
// figmaedit — a Figma-style editor on top of figmalib.
// Interaction semantics intentionally mirror Figma: scope-based selection
// (click = top-level under cursor, double-click drills in, Ctrl+click deep
// selects, Esc pops out), space/middle-drag pan, Ctrl+wheel or two-finger
// trackpad pinch (macOS) zoom at cursor.

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <figmalib/figmalib.h>

#include <raylib.h>

namespace figmaedit {

using figmalib::Document;
using figmalib::Mat23;
using figmalib::Node;

// ---- layout (scaled once at startup for HiDPI) -------------------------------
extern float gUiScale;
extern int kToolbarH;    // 44 * scale
extern int kLayersW;     // 260 * scale
extern int kInspectorW;  // 280 * scale
void initUiScale();      // call after InitWindow

inline int fontS() { return static_cast<int>(13 * gUiScale); }
inline int fontM() { return static_cast<int>(15 * gUiScale); }
inline float ui(float v) { return v * gUiScale; }

// ---- theme (designed in Figma: figmaedit UI Design) -------------------------
inline constexpr ::Color kThemeCanvas{30, 30, 30, 255};     // #1E1E1E
inline constexpr ::Color kThemePanel{44, 44, 44, 255};      // #2C2C2C
inline constexpr ::Color kThemeField{36, 36, 36, 255};      // #242424 inputs
inline constexpr ::Color kThemeEdge{17, 17, 17, 255};       // panel borders
inline constexpr ::Color kThemeLine{64, 64, 64, 255};       // separators
inline constexpr ::Color kThemeText{217, 217, 217, 255};    // primary text
inline constexpr ::Color kThemeDim{128, 128, 128, 255};     // secondary text
inline constexpr ::Color kThemeAccent{13, 153, 255, 255};   // #0D99FF
inline constexpr ::Color kThemeRowSel{13, 153, 255, 45};    // selected row wash
inline constexpr ::Color kThemeRowHover{255, 255, 255, 12};

// Applies the dark theme to all raygui controls. Call once after InitWindow.
void applyEditorTheme();

// ---- UI font (Segoe UI; switches to Microsoft YaHei when the document
// contains CJK names). Codepoints are collected per document. ----
extern Font gUiFont;
void initUiFont();                                  // ASCII, at startup
void rebuildUiFontFor(const figmalib::Document& doc);
void uiText(const char* text, float x, float y, int size, ::Color color);
float uiMeasure(const char* text, int size);

constexpr float kZoomMin = 0.02f;
constexpr float kZoomMax = 64.0f;

// ---- camera ------------------------------------------------------------------
struct Camera2D2 {
    float zoom = 1.0f;
    float panX = 0.0f, panY = 0.0f;  // world origin in canvas-viewport pixels

    Mat23 view() const {
        Mat23 m;
        m.m00 = m.m11 = zoom;
        m.m02 = panX;
        m.m12 = panY;
        return m;
    }
    void worldToScreen(float wx, float wy, float& sx, float& sy) const {
        sx = wx * zoom + panX;
        sy = wy * zoom + panY;
    }
    void screenToWorld(float sx, float sy, float& wx, float& wy) const {
        wx = (sx - panX) / zoom;
        wy = (sy - panY) / zoom;
    }
    // Zoom keeping the given viewport point fixed (Figma Ctrl+wheel).
    void zoomAt(float sx, float sy, float factor) {
        float wx, wy;
        screenToWorld(sx, sy, wx, wy);
        zoom = std::max(kZoomMin, std::min(kZoomMax, zoom * factor));
        panX = sx - wx * zoom;
        panY = sy - wy * zoom;
    }
};

// ---- undo --------------------------------------------------------------------
// Two kinds of edits: property mutations (captured as before/after snapshots
// of the touched nodes) and tree structure changes (remove/insert one node).
struct NodeProps {
    Node* node = nullptr;
    Mat23 transform;
    float width = 0, height = 0;
    float opacity = 1;
    float cornerRadius = 0;
    bool visible = true;
    std::vector<figmalib::Paint> fills;
    std::vector<figmalib::Paint> strokes;
    float strokeWeight = 1;
    figmalib::StrokeAlign strokeAlign = figmalib::StrokeAlign::Inside;
    std::vector<figmalib::Effect> effects;
    figmalib::TextStyle textStyle;
    std::string characters;
    // Fields below are only edited via MCP, but capturing them here keeps
    // every AI edit undoable with the same machinery.
    std::string name;
    bool clipsContent = false;
    std::optional<std::array<float, 4>> rectangleCornerRadii;
    std::vector<float> strokeDashes;
    std::vector<figmalib::TextRun> textRuns;
    std::vector<figmalib::PathGeometry> fillGeometry;
    figmalib::Constraint constraintH = figmalib::Constraint::Min;
    figmalib::Constraint constraintV = figmalib::Constraint::Min;
    figmalib::AutoLayout autoLayout;
    float layoutGrow = 0;
    bool layoutAlignStretch = false;
    bool layoutAbsolute = false;

    static NodeProps capture(Node* n);
    void apply() const;
};

struct TreeChange {
    bool isInsert = false;            // false = node was removed
    Node* parent = nullptr;
    size_t index = 0;
    Node* node = nullptr;             // identity (owned below when detached)
    std::unique_ptr<Node> detached;   // holds the node while out of the tree
};

struct UndoEntry {
    std::vector<NodeProps> before, after;
    std::vector<TreeChange> tree;
};

// ---- editor state ------------------------------------------------------------
enum class Tool { Move, Hand };

enum class DragMode { None, Pan, Marquee, MoveNodes, Resize };

struct EditorState {
    figmalib::LoadedFile file;
    std::string filePath;     // original input
    std::string savePath;     // where Ctrl+S writes
    figmalib::Renderer renderer;

    Node* page = nullptr;     // current CANVAS
    int pageIndex = 0;
    Node* scope = nullptr;    // drill-in container (page by default)
    std::vector<Node*> selection;
    Node* hovered = nullptr;

    Camera2D2 cam;
    Tool tool = Tool::Move;
    bool spaceHeld = false;

    // gesture
    DragMode drag = DragMode::None;
    Vector2 dragStartScreen{};
    float dragStartWX = 0, dragStartWY = 0;
    int resizeHandle = -1;            // 0 TL, 1 TR, 2 BR, 3 BL, 4 T, 5 R, 6 B, 7 L
    std::vector<NodeProps> gestureBefore;
    bool gestureChanged = false;

    // undo
    std::vector<UndoEntry> undoStack;
    std::vector<UndoEntry> redoStack;

    // ---- per-frame render cache ----
    // Each top-level object of the page rasterizes into its own texture
    // (with padding for shadows). Pan/zoom only composites textures on the
    // GPU; a frame re-rasterizes when its content version changes or the
    // zoom leaves the band it was rendered at.
    struct FrameCacheEntry {
        std::unique_ptr<figmalib::Renderer> renderer;  // persistent tvg scene
        Texture2D tex{};
        bool texValid = false;
        float zoom = -1;        // raster zoom of the texture
        uint64_t version = ~0ull;
        double lastUsed = 0;
    };
    std::unordered_map<Node*, FrameCacheEntry> frameCache;
    std::unordered_map<Node*, uint64_t> frameVersion;
    std::string imageDir;                 // renderer config for cache entries
    std::vector<std::string> fontDirs;

    double lastViewChange = 0;
    bool viewSettled = true;
    bool docDirty = false;            // cache validity must be re-checked
    bool unsaved = false;
    float lastRenderMs = 0;           // measured raster cost (debug)

    uint64_t versionOf(Node* topChild);
    void bumpNode(Node* n);           // invalidate the top-level frame containing n
    void bumpAllFrames();
    void invalidateCache();           // unload all textures (file/page switch)
    void updateAbsoluteTransforms();  // page-space transforms for hit testing

    // ui
    std::unordered_set<Node*> expanded;  // layers tree
    float layersScroll = 0;
    float inspectorScroll = 0;
    std::string status;
    double statusUntil = 0;
    bool textEditActive = false;      // an inspector textbox owns the keyboard
    bool fileMenuOpen = false;        // File dropdown visible (blocks canvas clicks)

    int viewportX() const { return kLayersW; }
    int viewportY() const { return kToolbarH; }
    int viewportW() const { return GetScreenWidth() - kLayersW - kInspectorW; }
    int viewportH() const { return GetScreenHeight() - kToolbarH; }

    void setStatus(const std::string& s, double seconds = 2.5);
    void selectPage(int index);
    void zoomToFit();

    // selection helpers
    bool isSelected(Node* n) const;
    void setSelection(std::vector<Node*> sel);

    // edits
    void beginGesture();                     // capture NodeProps of selection
    void commitGesture();                    // push undo entry if changed
    void pushPropsUndo(std::vector<NodeProps> before);  // for one-shot edits
    void deleteSelection();
    void duplicateSelection();
    void undo();
    void redo();
    void markDocChanged();

    // Figma-style alignment of the selection: single node aligns within its
    // parent, multi-selection aligns within the selection bounds.
    enum class Align { Left, HCenter, Right, Top, VCenter, Bottom };
    void alignSelection(Align op);
};

// world-space axis-aligned bounds of a node (using absoluteTransform)
struct WorldRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
    bool contains(float x, float y) const {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
    bool intersects(const WorldRect& o) const {
        return x0 <= o.x1 && x1 >= o.x0 && y0 <= o.y1 && y1 >= o.y0;
    }
};
WorldRect worldBounds(const Node& n);

// interaction + canvas (editor_canvas.cpp)
void updateCanvas(EditorState& ed);
void drawCanvas(EditorState& ed);

// panels (editor_panels.cpp)
void drawToolbar(EditorState& ed);
void drawLayersPanel(EditorState& ed);
void drawInspector(EditorState& ed);

// native file dialogs (editor_dialogs.cpp); empty string = cancelled
std::string showOpenFileDialog();
std::string showSaveFileDialog(const std::string& suggested);

// file operations (main.cpp)
bool openFile(EditorState& ed, const std::string& path);
void saveFile(EditorState& ed);
void saveFileAs(EditorState& ed);

}  // namespace figmaedit
