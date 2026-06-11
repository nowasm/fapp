#pragma once
// figmalib — high-level UI runtime.
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

namespace figmalib {

class FigmaUI {
public:
    using ClickHandler = std::function<void(Node&)>;
    using HoverHandler = std::function<void(Node&, bool entered)>;

    static std::unique_ptr<FigmaUI> fromFile(const std::string& path);
    static std::unique_ptr<FigmaUI> fromJson(const std::string& jsonText);
    ~FigmaUI();

    Document& document();
    Renderer& renderer();

    // ---- Frames ----
    std::vector<std::string> frameNames() const;
    bool selectFrame(const std::string& name);  // also accepts a node id
    Node* currentFrame() const;

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

    // ---- Runtime mutation (marks dirty, returns false if node not found) ----
    bool setVisible(const std::string& nodeName, bool visible);
    bool setOpacity(const std::string& nodeName, float opacity);  // <0 resets to authored
    bool setText(const std::string& nodeName, const std::string& text);

    // Switch an INSTANCE to a sibling variant of its component set, e.g.
    // setVariant("btn-start", "State", "Hover") finds the variant whose name
    // matches the instance's current properties with State replaced. The
    // component set must be present in the document. Fails (false) when the
    // instance, its set, or the target variant can't be resolved.
    bool setVariant(const std::string& instanceName, const std::string& property,
                    const std::string& value);

private:
    FigmaUI();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace figmalib
