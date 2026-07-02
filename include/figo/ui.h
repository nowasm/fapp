#pragma once
// figo — high-level UI runtime.
//
// Owns a parsed Figma document plus a renderer and adds frame selection,
// hit-testing, pointer input and per-node callbacks. This is the API game
// code talks to; engine backends only shuttle pixels and input events.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "document.h"
#include "renderer.h"

namespace figo {

// One machine-readable warning from FigmaUI::diagnostics(). kind is one of:
//   "font-fallback" — a TEXT node's requested font family could not be
//                     resolved and rendering silently substituted another
//                     family (or nothing loaded at all)
//   "text-overflow" — a TEXT node's laid-out content exceeds its box (lines
//                     are cut off / a non-wrapping line runs past the width)
//   "node-overflow" — a visible child sticks out of a clipsContent parent,
//                     so part of it is clipped away
struct Diagnostic {
    std::string kind;
    std::string nodeName;
    std::string nodeId;
    std::string message;  // human-readable detail (requested vs actual, px amounts)
};

class FigmaUI {
public:
    // Event coordinates are viewport/pixel-space (the same space the pointer
    // methods take).
    using ClickHandler = std::function<void(Node&, float x, float y)>;
    using HoverHandler = std::function<void(Node&, bool entered, float x, float y)>;
    using LongPressHandler = std::function<void(Node&, float x, float y)>;
    // direction is "left" or "right" (v1 is horizontal-only).
    using SwipeHandler = std::function<void(Node&, const char* direction)>;
    // x/y are the node's current scroll offset in frame-local px.
    using ScrollHandler = std::function<void(Node&, float scrollX, float scrollY)>;
    // snapIndex = the child the container is snapped to (snapToChildren
    // containers), -1 otherwise. See onScrollEnd.
    using ScrollEndHandler =
        std::function<void(Node&, float scrollX, float scrollY, int snapIndex)>;

    static std::unique_ptr<FigmaUI> fromFile(const std::string& path);
    static std::unique_ptr<FigmaUI> fromJson(const std::string& jsonText);
    ~FigmaUI();

    Document& document();
    Renderer& renderer();

    // ---- Frames ----
    std::vector<std::string> frameNames() const;
    bool selectFrame(const std::string& name);  // also accepts a node id
    Node* currentFrame() const;

    // ---- Navigation (frame stack with transitions) ----
    // navigateTo pushes the current frame onto the back stack and switches to
    // the target (name or id) with an animated transition; navigateBack pops
    // with the reverse animation. Drive animations by calling update(dt) once
    // per host frame. Nodes with an authored Figma prototype link
    // (transitionNodeID) navigate automatically when clicked.
    enum class Transition { None, Dissolve, SlideLeft, SlideRight, SlideUp, SlideDown };
    bool navigateTo(const std::string& frameName,
                    Transition transition = Transition::SlideLeft,
                    float durationSec = 0.3f);
    bool navigateBack(float durationSec = 0.3f);
    bool canGoBack() const;
    void update(float dtSeconds);  // advances transitions; cheap when idle
    bool animating() const;

    // ---- Transition compositing (backend contract) ----
    // Transitions do not re-rasterize the vector scene per tick. Instead the
    // backend keeps the previous frame's texture as a snapshot and composites
    // it with the current frame's texture on the GPU:
    //   - transitionId() increments every time a navigation starts. When the
    //     backend sees a new id, its current texture still shows the outgoing
    //     frame — snapshot it into a "previous" texture before rendering.
    //   - While animating(), draw the snapshot and the current texture
    //     offset (slides) or alpha-faded (dissolve, incoming on top) by
    //     transitionProgress(); the incoming frame itself rasterizes once.
    // See backends/raylib for the reference implementation.
    uint32_t transitionId() const;
    Transition transitionType() const;
    float transitionProgress() const;  // eased, [0,1)

    // Persistent bottom chrome (native tab-bar semantics): bottom-anchored
    // scrollFixed top-level elements that exist in BOTH frames with identical
    // name and geometry stay put while the pages slide. During such a
    // transition the chrome is excluded from both page rasters and handed to
    // the backend as a separate straight-alpha image to draw statically on
    // top — so anything overhanging the bar (round center buttons) and the
    // page pixels visible around it composite correctly:
    //   - On a new transitionId, call renderTransitionSnapshot() BEFORE
    //     snapshotting the current texture: it re-rasterizes the OUTGOING
    //     frame without the chrome into the current target (false → no
    //     shared chrome, snapshot as-is).
    //   - transitionChromePixels() is the overlay image (width × height,
    //     RGBA8888 straight alpha) to draw at viewport row `y` on top of the
    //     sliding pages; the pointer stays valid for the whole transition.
    //     nullptr → composite without an overlay.
    bool renderTransitionSnapshot();
    const uint32_t* transitionChromePixels(uint32_t& width, uint32_t& height,
                                           float& y) const;

    // Viewport-space Y of the top edge of the shared bottom chrome (band
    // fallback / app queries). Returns the full height when idle or when the
    // frames share no such chrome (e.g. navigating into a detail page — then
    // the whole page slides, which is the native behavior too).
    float transitionStaticBottomY() const;

    // ---- Render ----
    // How the frame follows the viewport size:
    //   Scale  — uniform scale-to-fit, letterboxed (default; authored layout)
    //   Reflow — responsive: the frame resizes to the viewport and children
    //            follow their Figma constraints / auto-layout
    enum class ResizeMode { Scale, Reflow };
    void setResizeMode(ResizeMode mode);
    ResizeMode resizeMode() const;

    void setViewport(uint32_t width, uint32_t height);
    // GPU variant: render into a GL framebuffer object via ThorVG's GL
    // engine (zero CPU copy). Requires a current GL context; returns false
    // when the GL engine is unavailable (caller falls back to setViewport).
    bool setViewportGL(int32_t fboId, uint32_t width, uint32_t height);
    bool render();  // true → pixel buffer changed, re-upload texture
    const uint32_t* pixels() const;
    uint32_t pixelWidth() const;
    uint32_t pixelHeight() const;
    void markDirty();

    // ---- Input (viewport/pixel coordinates) ----
    void pointerMove(float x, float y);
    void pointerDown(float x, float y);
    void pointerUp(float x, float y);

    // Wheel/trackpad scroll at a viewport position: moves the content of the
    // innermost scrollable frame under the point (positive dy scrolls the
    // content up, i.e. reveals what is below). When that frame is already at
    // its limit the delta bubbles to the next scrollable ancestor. Returns
    // false when nothing scrollable consumed the delta.
    //
    // Touch-style drag scrolling needs no extra calls: pointerDown +
    // pointerMove over a scrollable frame pans it once the drag exceeds a
    // small threshold, and the release is not reported as a click.
    bool scrollBy(float x, float y, float dx, float dy);

    // Set a scrolling frame's offset directly (clamped to the content range).
    bool setScroll(const std::string& nodeName, float offsetX, float offsetY);
    bool setScroll(Node& node, float offsetX, float offsetY);

    // ---- Scroll snapping (wheel-picker foundation) ----
    // node.snapToChildren (a runtime Node flag, settable from JS) makes a
    // scrolling frame settle on child boundaries whenever a scroll comes to a
    // natural rest: a fling that decays below its stop speed eases onto the
    // nearest child, a drag released too slow to fling does the same, and a
    // wheel notch quantizes its easing target to a child boundary (one notch
    // advances at least one item). A "child boundary" is the scroll offset
    // that aligns child i's main-axis start with the first child's start
    // (auto-layout padding/itemSpacing fall out of the child positions),
    // clamped into the scroll range.

    // Ease the named frame's scroll so child `index` (clamped) sits at the
    // first child's start. durationSec < 0 → the stock easing feel, 0 →
    // instant set. Works on any scrolling frame, snapToChildren or not.
    bool snapTo(const std::string& nodeName, int index, float durationSec = -1.0f);
    // Child the container is currently nearest/snapped to; -1 unless the
    // node has snapToChildren set (and at least one child).
    int snapIndex(const Node& node) const;

    // Innermost scrollable frame under the point with a non-empty scroll
    // range, or nullptr.
    Node* scrollableAt(float x, float y);

    // Deepest visible node under the point, or nullptr.
    Node* hitTest(float x, float y);
    Node* hoveredNode() const;
    Node* pressedNode() const;

    // Callbacks fire for the named node or any ancestor of the hit node with
    // that name (so a click on a button's label still triggers the button).
    void onClick(const std::string& nodeName, ClickHandler fn);
    void onHover(const std::string& nodeName, HoverHandler fn);
    // Long press: the pointer stays down >= 0.5s without travelling past the
    // drag-scroll threshold. Fires at most once per press (from update(dt));
    // a press that fired a long-press handler does not also click on release.
    void onLongPress(const std::string& nodeName, LongPressHandler fn);
    // Horizontal swipe, evaluated on pointerUp: total travel |dx| >= 60
    // viewport px, |dx| > 2|dy|, press shorter than 0.5s. Bubbles up from the
    // pressed node like click. A gesture the drag-scroller already consumed
    // as HORIZONTAL scrolling is not reported again; a horizontal flick over
    // a vertical scroll chain still swipes (list-row swipe-to-delete). A
    // fired swipe consumes the release (no click).
    void onSwipe(const std::string& nodeName, SwipeHandler fn);
    // Fires when the named scrolling frame's offset actually changed this
    // frame (wheel, drag, fling/easing, setScroll). Multiple changes within
    // one update(dt) coalesce into one call at the end of update; an ongoing
    // fling therefore fires once per frame. No ancestor bubbling: the handler
    // name must match the scrolled node.
    void onScroll(const std::string& nodeName, ScrollHandler fn);
    // Fires once when the named scrolling frame comes fully to rest: easing,
    // fling and the drag gesture are all over and the offset stopped moving.
    // A wheel-notch train or a fling therefore yields exactly one call at the
    // final position; an instant set (setScroll / scrollX|Y writes) counts as
    // an end of its own and fires at the end of that update(dt). snapIndex is
    // the child the container settled on for snapToChildren containers, -1
    // otherwise. No ancestor bubbling (like onScroll).
    void onScrollEnd(const std::string& nodeName, ScrollEndHandler fn);
    // Drop every registered event handler. Script hot reload uses this
    // before re-running a script that re-registers its handlers.
    void clearHandlers();

    // ---- Text editing ----
    // A TEXT node marked editable receives focus when clicked (caret at the
    // end); clicking anywhere else blurs. The focused node draws a caret and
    // consumes textInput/editKey. IME note: committed composition strings
    // arrive through the host's normal character events — feed them to
    // textInput like any typed character.
    enum class EditKey { Left, Right, Backspace, Delete, Home, End };

    bool setEditable(const std::string& nodeName, bool editable = true);
    bool focusText(const std::string& nodeName);  // programmatic focus
    void blur();
    Node* focusedNode() const;
    void textInput(const std::string& utf8);  // insert at the caret
    void editKey(EditKey key);
    // ---- Clipboard editing ----
    // The core never touches the platform clipboard; a backend glues these
    // to it (raylib: Ctrl+C/X/V/A, plus Cmd on macOS). On a passwordMask
    // node editCopy/editCut return "" (and cut deletes nothing) so passwords
    // never leave the node.
    std::string editCopy();  // selection text of the focused node, "" if none
    std::string editCut();   // editCopy() + delete the selection
    // Replace the selection / insert at the caret. Same filtering as
    // textInput: \r and control chars drop, \n survives (multi-line paste).
    void editPaste(const std::string& utf8);
    void editSelectAll();    // anchor at 0, caret at end (Ctrl+A)

    // ---- Value binding (G3): slider / progress semantics on designed nodes ----
    // The engine owns the "gesture -> value" mapping; the LOOK stays entirely
    // in the design (track/knob/fill are ordinary designed nodes).
    struct SliderOptions {
        float min = 0, max = 1;
        float step = 0;      // snap increment; 0 = continuous
        float value = 0;     // initial value (clamped/snapped)
        std::string knob;    // child name of the track: translated along the axis
        std::string fill;    // child name of the track: main-axis size follows value
        bool axisY = false;  // false = horizontal ("x", default); true = vertical
        // readonly = progress bar: gestures pass through untouched; the value
        // only moves via setValue().
        bool readonly = false;
        // Fires while dragging (committed=false, deduped per value change) and
        // once on release (committed=true, final value). Never fired by setValue.
        std::function<void(float value, bool committed)> onChange;
    };
    // Registers slider semantics on the named track node (registration is
    // by NAME, like onClick — it survives bindList/setVariant rebuilds; knob
    // and fill are re-resolved by name inside the track on every update).
    // A pointer press on the track (or any descendant) arms the slider; once
    // the drag direction is known, a drag along the slider's axis is consumed
    // by the slider (it wins over scrolling on that axis), a cross-axis drag
    // is handed to normal drag scrolling. A tap jumps to the tapped position
    // (committed immediately). The engine moves the knob / resizes the fill in
    // frame-local space (both relativeTransform AND baseTransform, so Scale
    // and Reflow resize modes agree). Value mapping is linear along +axis
    // (left/top = min). Returns false when no node with that name exists yet
    // (the binding is still registered).
    bool bindSlider(const std::string& trackName, SliderOptions opts);
    // Program-driven value (progress bars, initial sync): clamps + snaps,
    // places knob/fill, does NOT fire onChange. False when the name has no
    // slider binding.
    bool setValue(const std::string& trackName, float value);

    // ---- Automatic interaction states (G3) ----
    // Values are "Prop=Value" variant fragments; empty fields fall back to
    // the State=Hover / State=Pressed / State=Default convention.
    struct AutoStateMap {
        std::string hover, pressed, base;
    };
    // Registers automatic variant switching for the named INSTANCE (by name,
    // like onClick): the engine calls setVariant on hover enter/leave and
    // press/release (pressed wins over hover), each with the default 0.12s
    // dissolve. The switches are deferred to the end of the pointer event /
    // update tick, AFTER handler dispatch — so the structural rebuild they
    // cause never eats the bubbling of the very click that triggered them.
    // A variant the set doesn't contain is skipped (setVariant fails soft).
    // Returns whether an instance with that name currently exists.
    bool autoStates(const std::string& instanceName, const AutoStateMap& states = {});

    // ---- Data-driven lists ----
    // Treats the named node's first child as the item template and fills the
    // list with `count` clones of it; `bind` then stamps each clone with its
    // data (find parts via item.findByName, write text with setNodeText).
    // The pristine template is kept internally, so bindList can be called
    // again whenever the data changes. The list node should be an
    // auto-layout stack; inside a scrolling frame this yields a data-driven
    // scrollable list.
    using ListBinder = std::function<void(Node& item, size_t index)>;
    bool bindList(const std::string& listName, size_t count, const ListBinder& bind);

    // ---- Runtime mutation (marks dirty, returns false if node not found) ----
    bool setVisible(const std::string& nodeName, bool visible);
    bool setOpacity(const std::string& nodeName, float opacity);  // <0 resets to authored
    bool setText(const std::string& nodeName, const std::string& text);

    // ---- Diagnostics ----
    // Walks the current frame's visible subtree and reports render problems a
    // screenshot doesn't explain: silent font fallback, truncated/overflowing
    // text, and content clipped away by a clipsContent parent. Computed on
    // demand (no persistent state); empty = the frame renders clean. Scroll
    // containers legitimately overflow along their scroll axes — those axes
    // are not reported. Scripts read this via ui.diagnostics(); figoplay
    // --shot writes it next to the screenshot as <shot>.diagnostics.json.
    std::vector<Diagnostic> diagnostics() const;

    // Switch an INSTANCE to a sibling variant of its component set, e.g.
    // setVariant("btn-start", "State", "Hover") finds the variant whose name
    // matches the instance's current properties with State replaced. The
    // component set must be present in the document. Fails (false) when the
    // instance, its set, or the target variant can't be resolved.
    // durationSec > 0 adds a dissolve-style transition. v1 implementation:
    // the freshly swapped-in subtree fades in via runtimeOpacity 0 -> authored
    // over the duration (the old subtree is not snapshotted — the full-frame
    // transition compositing channel is per-navigation, not per-node).
    // autoStates uses 0.12s by default.
    bool setVariant(const std::string& instanceName, const std::string& property,
                    const std::string& value, float durationSec = 0.0f);

private:
    FigmaUI();
    void pointerUpMain(float x, float y);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace figo
