#pragma once
// figo — Figma document model
// In-memory representation of a Figma file (REST API JSON, geometry=paths).

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace figo {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};

// 2x3 affine transform, row-major:
//   x' = m00*x + m01*y + m02
//   y' = m10*x + m11*y + m12
struct Mat23 {
    float m00 = 1, m01 = 0, m02 = 0;
    float m10 = 0, m11 = 1, m12 = 0;

    static Mat23 identity() { return {}; }

    Mat23 operator*(const Mat23& o) const {
        Mat23 r;
        r.m00 = m00 * o.m00 + m01 * o.m10;
        r.m01 = m00 * o.m01 + m01 * o.m11;
        r.m02 = m00 * o.m02 + m01 * o.m12 + m02;
        r.m10 = m10 * o.m00 + m11 * o.m10;
        r.m11 = m10 * o.m01 + m11 * o.m11;
        r.m12 = m10 * o.m02 + m11 * o.m12 + m12;
        return r;
    }

    void apply(float x, float y, float& ox, float& oy) const {
        ox = m00 * x + m01 * y + m02;
        oy = m10 * x + m11 * y + m12;
    }

    std::optional<Mat23> inverted() const {
        float det = m00 * m11 - m01 * m10;
        if (det == 0.0f) return std::nullopt;
        float inv = 1.0f / det;
        Mat23 r;
        r.m00 = m11 * inv;
        r.m01 = -m01 * inv;
        r.m10 = -m10 * inv;
        r.m11 = m00 * inv;
        r.m02 = -(r.m00 * m02 + r.m01 * m12);
        r.m12 = -(r.m10 * m02 + r.m11 * m12);
        return r;
    }
};

enum class PaintType {
    Solid,
    GradientLinear,
    GradientRadial,
    GradientAngular,  // rendered as radial fallback
    GradientDiamond,  // rendered as radial fallback
    Image,
};

struct GradientStop {
    float position = 0;
    Color color;
};

struct Paint {
    PaintType type = PaintType::Solid;
    bool visible = true;
    float opacity = 1.0f;
    Color color;  // Solid
    std::vector<GradientStop> stops;
    // Gradient handles, normalized to the node's bounding box (0..1).
    // [0] = start/center, [1] = end/edge, [2] = width handle.
    std::array<float, 2> handle0{0, 0};
    std::array<float, 2> handle1{1, 1};
    std::array<float, 2> handle2{0, 1};
    std::string imageRef;          // Image
    std::string imageScaleMode;    // FILL | FIT | TILE | STRETCH
};

enum class NodeType {
    Document,
    Canvas,
    Frame,
    Group,
    Section,
    Rectangle,
    Ellipse,
    Line,
    Vector,
    BooleanOperation,
    Star,
    RegularPolygon,
    Text,
    Component,
    ComponentSet,
    Instance,
    Slice,
    Unknown,
};

enum class StrokeAlign { Inside, Outside, Center };

struct Effect {
    enum class Type { DropShadow, InnerShadow, LayerBlur, BackgroundBlur };
    Type type = Type::DropShadow;
    bool visible = true;
    Color color{0, 0, 0, 0.25f};
    float offsetX = 0, offsetY = 0;
    float radius = 0;  // blur radius
    float spread = 0;
};

struct TextStyle {
    std::string fontFamily = "Inter";
    std::string fontPostScriptName;
    float fontSize = 14;
    int fontWeight = 400;
    bool italic = false;
    float lineHeightPx = 0;    // 0 → font default
    float letterSpacing = 0;
    enum class AlignH { Left, Center, Right, Justified } alignH = AlignH::Left;
    enum class AlignV { Top, Center, Bottom } alignV = AlignV::Top;
    // Figma textAutoResize: NONE | HEIGHT | WIDTH_AND_HEIGHT | TRUNCATE.
    // WIDTH_AND_HEIGHT boxes hug the text, so wrapping must be disabled (a
    // substituted font could otherwise wrap and break the layout).
    std::string autoResize = "NONE";
    bool truncateEnding = false;  // textTruncation == "ENDING" → ellipsis
    int maxLines = 0;             // 0 = unlimited
};

// A single path from fillGeometry/strokeGeometry: SVG path data + winding rule.
struct PathGeometry {
    std::string path;
    bool evenOdd = false;
};

// How a node follows its parent frame's edges when the frame resizes.
// Min/Max = pin to left/top resp. right/bottom edge; Stretch = pin both edges;
// Scale = position and size scale proportionally.
enum class Constraint { Min, Center, Max, Stretch, Scale };

// Which axes a frame's content can scroll along. .fig: scrollDirection;
// REST: overflowDirection (HORIZONTAL/VERTICAL/HORIZONTAL_AND_VERTICAL_SCROLLING).
enum class ScrollDirection { None, Horizontal, Vertical, Both };

// Auto-layout ("stack") container properties. REST: layoutMode/padding*/
// itemSpacing/...; canvas.json (kiwi): stackMode/stackSpacing/stack*Padding/...
struct AutoLayout {
    enum class Mode { None, Horizontal, Vertical };
    enum class Sizing { Fixed, Hug };  // Hug = size to content (REST "AUTO")
    enum class Align { Min, Center, Max, SpaceBetween, Baseline };

    Mode mode = Mode::None;
    Sizing primarySizing = Sizing::Fixed;
    Sizing counterSizing = Sizing::Fixed;
    Align primaryAlign = Align::Min;    // children packing along the main axis
    Align counterAlign = Align::Min;    // children alignment across the axis
    float paddingLeft = 0, paddingRight = 0, paddingTop = 0, paddingBottom = 0;
    float itemSpacing = 0;
    float counterSpacing = 0;  // gap between wrapped rows/columns
    bool wrap = false;

    bool enabled() const { return mode != Mode::None; }
};

// Rich-text segment: [start, end) byte range of Node::characters with its own
// style. Only produced for ASCII single-style-per-character documents; the
// renderer falls back to the node's base style when it cannot honor a run.
struct TextRun {
    int start = 0, end = 0;
    TextStyle style;
    std::optional<Color> color;  // overrides the node's fill for this run
};

// One CSS @keyframes stop, reduced to the properties we can replay in an engine
// AnimationPlayer: normalized time t∈[0,1] plus optional opacity / 2D scale /
// 2D position delta (rest-relative px, for translate/slash-sweep animations).
struct AnimKey {
    float t = 0;
    bool hasOpacity = false;
    float opacity = 1.0f;
    bool hasScale = false;
    float sx = 1.0f, sy = 1.0f;
    bool hasPos = false;
    float dx = 0.0f, dy = 0.0f;
};

// A node's CSS animation, captured by web2canvas (animation-* + the resolved
// @keyframes). Minimal subset: opacity + transform:scale, looping/finite, with
// a transform-origin pivot. Consumed by figo2godot to emit an AnimationPlayer.
struct NodeAnim {
    float dur = 0;          // seconds (animation-duration)
    float delay = 0;        // seconds (animation-delay)
    int iter = 1;           // animation-iteration-count; 0 = infinite
    float pivotX = 0.5f;    // transform-origin as a fraction of the box
    float pivotY = 0.5f;
    std::string ease;       // animation-timing-function (first value)
    std::vector<AnimKey> keys;
};

// All value state of a node, split out so nodes can be cloned with a plain
// member-wise copy (see cloneNode) — new fields added here are picked up by
// clone/serialize automatically; only parent/children need special handling.
struct NodeData {
    std::string id;
    std::string name;
    NodeType type = NodeType::Unknown;
    bool visible = true;
    float opacity = 1.0f;

    // Transform relative to parent; node-local coordinates have origin at the
    // node's top-left corner.
    Mat23 relativeTransform;
    float width = 0, height = 0;
    bool clipsContent = false;

    std::vector<Paint> fills;
    std::vector<Paint> strokes;
    float strokeWeight = 1.0f;
    StrokeAlign strokeAlign = StrokeAlign::Inside;
    std::vector<float> strokeDashes;
    std::string strokeCap;   // NONE | ROUND | SQUARE
    std::string strokeJoin;  // MITER | BEVEL | ROUND

    float cornerRadius = 0;
    std::optional<std::array<float, 4>> rectangleCornerRadii;  // TL, TR, BR, BL

    std::vector<PathGeometry> fillGeometry;
    std::vector<PathGeometry> strokeGeometry;

    std::vector<Effect> effects;

    // Text
    std::string characters;
    TextStyle textStyle;
    std::vector<TextRun> textRuns;  // empty → uniform style

    // Instances: id of the COMPONENT this instance was created from.
    std::string componentId;

    // web2canvas: the SOURCE component type that rendered this node (from the
    // React fiber), set only on a component's root element. figo2godot groups
    // every instance of a type into one prefab. Empty for plain DOM elements.
    std::string compType;

    // Prototype navigation, when authored in Figma: clicking this node goes
    // to the target frame. FigmaUI wires these automatically.
    std::string transitionNodeId;  // destination frame id ("" = none)
    std::string transitionType;    // MOVE_IN/PUSH/SLIDE_IN/DISSOLVE/...
    float transitionDuration = 0;  // seconds

    // ---- Layout (responsive reflow) ----
    Constraint constraintH = Constraint::Min;
    Constraint constraintV = Constraint::Min;
    float minWidth = 0, maxWidth = 0;    // 0 = unconstrained
    float minHeight = 0, maxHeight = 0;
    AutoLayout autoLayout;            // container: auto-layout stack settings
    float layoutGrow = 0;             // child: flex-grow along parent's main axis
    bool layoutAlignStretch = false;  // child: stretch across parent's counter axis
    bool layoutAbsolute = false;      // child: excluded from auto-layout flow

    // Authored geometry, captured once after parsing. The layout engine
    // restores from this snapshot before every reflow so repeated viewport
    // resizes don't accumulate error.
    Mat23 baseTransform;
    float baseWidth = 0, baseHeight = 0;

    // ---- Scrolling ----
    ScrollDirection scrollDirection = ScrollDirection::None;
    // Child of a scrolling frame that stays put while siblings scroll
    // (.fig/REST scrollBehavior FIXED_WHEN_CHILD_OF_SCROLLING_FRAME / FIXED).
    bool scrollFixed = false;
    // Runtime-only switch (no design counterpart): scrolling that comes to a
    // natural rest settles onto the nearest child boundary along the main
    // scroll axis (wheel-picker semantics). See FigmaUI::snapTo/snapIndex.
    bool snapToChildren = false;

    // ---- Runtime state (set by the renderer / UI layer) ----
    Mat23 absoluteTransform;        // computed during scene build
    float runtimeOpacity = -1.0f;   // override; <0 → use authored opacity
    int runtimeVisible = -1;        // -1 inherit, 0 hidden, 1 visible
    // Excluded from the page raster only (transition chrome is drawn as a
    // separate static overlay); hit-testing still sees the node.
    bool renderSuppressed = false;
    float scrollX = 0, scrollY = 0;  // content offset of a scrolling frame, ≥0
    // Text editing (FigmaUI::setEditable / focus): a TEXT node the user can
    // type into. caretByte ≥ 0 → node has focus; the renderer draws the caret.
    bool editable = false;
    // Password field: the renderer displays every code point as U+2022 "•"
    // and all caret/selection/click-positioning metrics use that masked
    // display text, while `characters` stays the real plaintext (node.text
    // reads/writes plaintext). FigmaUI::editCopy/editCut return "" on masked
    // nodes so passwords never reach a clipboard. Copied by cloneNode.
    bool passwordMask = false;
    int caretByte = -1;  // UTF-8 byte offset into `characters`
    // Selection anchor (set on press, caret follows the drag); together with
    // caretByte it spans the highlighted range. -1 → no selection.
    int selAnchorByte = -1;

    // CSS animation (web2canvas only); absent for static nodes. The renderer
    // ignores it — only figo2godot replays it as a Godot AnimationPlayer.
    std::optional<NodeAnim> anim;
};

struct Node : NodeData {
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    bool effectivelyVisible() const {
        return runtimeVisible < 0 ? visible : runtimeVisible != 0;
    }
    float effectiveOpacity() const {
        return runtimeOpacity < 0 ? opacity : runtimeOpacity;
    }
    bool scrolls() const { return scrollDirection != ScrollDirection::None; }

    // Extent of the scrollable content: bounding box (from the node's origin)
    // of all visible non-fixed children in node-local coordinates. The scroll
    // range is contentExtent − frame size, clamped to ≥0 per axis.
    void contentExtent(float& w, float& h) const;
    float maxScrollX() const;
    float maxScrollY() const;

    Node* findById(const std::string& nodeId);
    Node* findByName(const std::string& nodeName);
    void visit(const std::function<bool(Node&)>& fn);  // fn returns false to stop descent
};

// Deep copy of a subtree; the copy's parent is set to `parent` (may be null).
std::unique_ptr<Node> cloneNode(const Node& src, Node* parent);

// Set a text node's characters at runtime. Clears stale rich-text runs (they
// index the old string); rendering falls back to the node's base style.
void setNodeText(Node& n, const std::string& text);

struct Document {
    std::string name;
    std::unique_ptr<Node> root;  // DOCUMENT node; children are CANVAS pages

    Node* findById(const std::string& id) { return root ? root->findById(id) : nullptr; }
    Node* findByName(const std::string& n) { return root ? root->findByName(n) : nullptr; }

    // Top-level frames of every page, in document order.
    std::vector<Node*> topLevelFrames() const;

    // Snapshot the current geometry of every node as the layout baseline
    // (base{Transform,Width,Height}). The parsers call this once after
    // loading; call it again after edits that should become the new baseline.
    void captureBaseLayout();
};

}  // namespace figo
