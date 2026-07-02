#include "figo/ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <unordered_map>

#include "figo/layout.h"
#include "figo/parser.h"

namespace figo {

struct FigmaUI::Impl {
    std::unique_ptr<Document> doc;
    Renderer renderer;
    Node* frame = nullptr;
    Node* hovered = nullptr;
    Node* pressed = nullptr;
    Node* focused = nullptr;  // editable TEXT node owning the caret
    // Bumped whenever nodes are destroyed/rebuilt (bindList, setVariant):
    // event dispatch walks raw Node* chains and must bail out once a handler
    // invalidated them.
    uint64_t structureRev = 0;
    ResizeMode resizeMode = ResizeMode::Scale;

    // Touch-style drag scrolling: candidate picked at pointerDown, panning
    // starts once the pointer travels past a small threshold; the release is
    // then swallowed instead of clicking.
    Node* dragScrollNode = nullptr;
    bool dragScrolling = false;
    float dragAccumX = 0, dragAccumY = 0;
    float lastPointerX = 0, lastPointerY = 0;
    static constexpr float kDragScrollThreshold = 6.0f;  // viewport px

    // ---- Gesture recognition (long press / swipe) ----
    // All positions are viewport px; pressElapsed accumulates RAW update(dt)
    // time (not the 1/30s-clamped animation clock) so held time is real time.
    static constexpr float kLongPressSec = 0.5f;
    static constexpr float kSwipeMinDx = 60.0f;  // viewport px
    float pressDownX = 0, pressDownY = 0;  // pointerDown position
    float pressElapsed = 0;                // seconds the press has been held
    bool pressMoved = false;      // travelled past kDragScrollThreshold
    bool longPressDone = false;   // long-press evaluated for this press
    bool suppressClick = false;   // a long-press handler fired -> eat the click
    float dragConsumedX = 0;      // frame-local X actually eaten by drag-scroll
    // Pressing inside the focused text starts caret placement / drag-select
    // (and takes the gesture away from drag scrolling).
    bool selectingText = false;

    // Smooth scrolling. Wheel deltas ease toward a target offset; releasing a
    // drag keeps its velocity as a decaying fling. Advanced by update(dt);
    // every step goes through Renderer::markScrollDirty (transform retarget,
    // no scene rebuild), which is what makes per-frame animation affordable.
    struct ScrollAnim {
        float targetX = 0, targetY = 0;  // wheel easing target (frame-local)
        float velX = 0, velY = 0;        // fling velocity (frame-local px/s)
        bool easing = false, fling = false;
    };
    std::unordered_map<Node*, ScrollAnim> scrollAnims;
    Node* dragConsumer = nullptr;        // node the current drag actually scrolls
    float dragMoveX = 0, dragMoveY = 0;  // frame-local travel since last update
    float dragVelX = 0, dragVelY = 0;    // lowpassed drag velocity (px/s)
    static constexpr float kEaseRate = 14.0f;    // wheel easing strength (1/s)
    static constexpr float kFlingFriction = 4.0f;  // fling decay (1/s)
    static constexpr float kFlingStart = 60.0f;  // min release speed (px/s)
    static constexpr float kFlingStop = 10.0f;   // fling ends below this
    std::unordered_map<std::string, std::vector<ClickHandler>> clickHandlers;
    std::unordered_map<std::string, std::vector<HoverHandler>> hoverHandlers;
    std::unordered_map<std::string, std::vector<LongPressHandler>> longPressHandlers;
    std::unordered_map<std::string, std::vector<SwipeHandler>> swipeHandlers;
    std::unordered_map<std::string, std::vector<ScrollHandler>> scrollHandlers;

    // Scrolling frames whose offset changed since the last dispatch; flushed
    // (coalesced, one call per node) at the end of update(dt). Cleared
    // whenever structureRev bumps — the pointers may be about to dangle.
    std::vector<Node*> pendingScroll;

    void noteScroll(Node* n) {
        if (scrollHandlers.empty()) return;  // fast path: feature unused
        if (std::find(pendingScroll.begin(), pendingScroll.end(), n) ==
            pendingScroll.end()) {
            pendingScroll.push_back(n);
        }
    }

    void dispatchScrollEvents() {
        if (pendingScroll.empty()) return;
        std::vector<Node*> batch;
        batch.swap(pendingScroll);  // handlers may queue new changes
        const uint64_t rev = structureRev;
        for (Node* n : batch) {
            const auto it = scrollHandlers.find(n->name);
            if (it == scrollHandlers.end()) continue;
            for (auto& h : it->second) {
                h(*n, n->scrollX, n->scrollY);
                // A handler rebuilt the tree (bindList/setVariant): every
                // remaining pointer in the batch may be freed memory.
                if (structureRev != rev) return;
            }
        }
    }

    // Layout changes shrink scroll ranges (a taller viewport can swallow the
    // whole range): pull every scrolling frame's offset back inside, or the
    // content stays shoved out of its clip box.
    void clampScrollOffsets() {
        if (!frame) return;
        frame->visit([this](Node& n) {
            if (n.scrolls()) {
                const float nx = std::clamp(n.scrollX, 0.0f, n.maxScrollX());
                const float ny = std::clamp(n.scrollY, 0.0f, n.maxScrollY());
                if (nx != n.scrollX || ny != n.scrollY) {
                    n.scrollX = nx;
                    n.scrollY = ny;
                    noteScroll(&n);
                }
            }
            return true;
        });
    }

    // In Reflow mode the current frame tracks the viewport size.
    void reflow() {
        if (resizeMode != ResizeMode::Reflow || !frame) return;
        const uint32_t w = renderer.width(), h = renderer.height();
        if (w == 0 || h == 0) return;
        layoutFrame(*frame, static_cast<float>(w), static_cast<float>(h));
        clampScrollOffsets();
        renderer.markDirty();
    }

    // Point in root-frame coordinates → deepest visible hit node.
    Node* hitTestFrame(Node& node, float fx, float fy) {
        if (!node.effectivelyVisible() || node.type == NodeType::Slice) return nullptr;

        const auto inv = node.absoluteTransform.inverted();
        if (!inv) return nullptr;
        float lx, ly;
        inv->apply(fx, fy, lx, ly);
        const bool inside =
            lx >= 0 && ly >= 0 && lx <= node.width && ly <= node.height;

        if ((node.clipsContent || node.scrolls()) && !inside) return nullptr;

        // Topmost child wins.
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            if (Node* hit = hitTestFrame(**it, fx, fy)) return hit;
        }

        if (!inside) return nullptr;
        const bool paintable = !node.fills.empty() || !node.strokes.empty() ||
                               !node.fillGeometry.empty() || !node.characters.empty();
        return paintable ? &node : nullptr;
    }

    Node* hitTestViewport(float x, float y) {
        if (!frame) return nullptr;
        const auto inv = renderer.contentTransform().inverted();
        if (!inv) return nullptr;
        float fx, fy;
        inv->apply(x, y, fx, fy);
        return hitTestFrame(*frame, fx, fy);
    }

    // Fires handlers registered for the node or any of its ancestors.
    // Returns true when at least one handler ran (gesture "consumed").
    template <typename Map, typename Fn>
    bool fireUp(Map& handlers, Node* node, Fn&& invoke) {
        // Capture the stop node up front: a handler may navigate and swap
        // `frame` mid-walk, and the loop must not run past the old frame.
        Node* stop = frame;
        const uint64_t rev = structureRev;
        bool fired = false;
        for (Node* n = node; n; n = n->parent) {
            if (auto it = handlers.find(n->name); it != handlers.end()) {
                for (auto& h : it->second) {
                    invoke(h, *n);
                    fired = true;
                    // Navigation consumes the event: once a handler switched
                    // frames, ancestors of the OLD page must not keep firing.
                    // (Nav items commonly share names with their destination
                    // frames — "Discover" the button bubbling up to
                    // "Discover" the frame would navigate right back.)
                    if (frame != stop) return fired;
                    // So does a structural mutation (bindList/setVariant):
                    // `n` and its parent chain may now be freed memory.
                    if (structureRev != rev) return fired;
                }
            }
            if (n == stop) break;
        }
        return fired;
    }

    // Name lookup for mutations: current frame first, then the whole document
    // (matches findNode in the script host and the script.h contract — apps
    // legitimately write to other pages, e.g. a cart badge while on Detail).
    Node* findMutable(const std::string& name) {
        if (frame) {
            if (Node* n = frame->findByName(name)) return n;
        }
        return doc && doc->root ? doc->root->findByName(name) : nullptr;
    }

    // ---- Scrolling ----

    // Frame-local pixels per viewport pixel (contentTransform is a uniform
    // scale-to-fit, so one axis suffices).
    float viewportToFrameScale() const {
        const float s = renderer.contentTransform().m00;
        return s > 0 ? 1.0f / s : 1.0f;
    }

    // Move a frame's content by (dx, dy) in frame-local pixels, clamped to
    // the scroll range. Returns true when the offset actually changed.
    bool applyScroll(Node& n, float dx, float dy) {
        const float nx = std::clamp(n.scrollX + dx, 0.0f, n.maxScrollX());
        const float ny = std::clamp(n.scrollY + dy, 0.0f, n.maxScrollY());
        if (nx == n.scrollX && ny == n.scrollY) return false;
        dragConsumedX += nx - n.scrollX;  // swipe vs. horizontal drag-scroll
        n.scrollX = nx;
        n.scrollY = ny;
        noteScroll(&n);
        return true;
    }

    // Deepest scrollable frame with a non-empty range containing the point
    // (frame coordinates). Unlike hitTestFrame this ignores paintability, so
    // empty padding inside a scroll area still scrolls.
    Node* scrollableUnder(Node& node, float fx, float fy) {
        if (!node.effectivelyVisible() || node.type == NodeType::Slice) return nullptr;
        const auto inv = node.absoluteTransform.inverted();
        if (!inv) return nullptr;
        float lx, ly;
        inv->apply(fx, fy, lx, ly);
        const bool inside =
            lx >= 0 && ly >= 0 && lx <= node.width && ly <= node.height;
        if ((node.clipsContent || node.scrolls()) && !inside) return nullptr;
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            if (Node* s = scrollableUnder(**it, fx, fy)) return s;
        }
        if (inside && node.scrolls() &&
            (node.maxScrollX() > 0 || node.maxScrollY() > 0)) {
            return &node;
        }
        return nullptr;
    }

    Node* scrollableAtViewport(float x, float y) {
        if (!frame) return nullptr;
        const auto inv = renderer.contentTransform().inverted();
        if (!inv) return nullptr;
        float fx, fy;
        inv->apply(x, y, fx, fy);
        return scrollableUnder(*frame, fx, fy);
    }

    // Apply a frame-local delta at `start`, bubbling to scrollable ancestors
    // when the inner frame is already at its limit. Returns the node that
    // consumed the delta (drag scrolling flings it on release).
    Node* scrollWithBubble(Node* start, float dx, float dy) {
        for (Node* n = start; n; n = n->parent) {
            if (n->scrolls() && applyScroll(*n, dx, dy)) {
                renderer.markScrollDirty();
                return n;
            }
            if (n == frame) break;
        }
        return nullptr;
    }

    // Wheel path: ease toward an accumulated target instead of jumping.
    // Bubbling compares against the pending target, so notches that already
    // saturated an inner frame flow to its scrollable ancestor.
    bool smoothScrollBy(Node* start, float dx, float dy) {
        for (Node* n = start; n; n = n->parent) {
            if (n->scrolls()) {
                const auto it = scrollAnims.find(n);
                const bool easing = it != scrollAnims.end() && it->second.easing;
                const float bx = easing ? it->second.targetX : n->scrollX;
                const float by = easing ? it->second.targetY : n->scrollY;
                const float nx = std::clamp(bx + dx, 0.0f, n->maxScrollX());
                const float ny = std::clamp(by + dy, 0.0f, n->maxScrollY());
                if (nx != bx || ny != by) {
                    ScrollAnim& a = scrollAnims[n];
                    a.targetX = nx;
                    a.targetY = ny;
                    a.easing = true;
                    a.fling = false;
                    return true;
                }
            }
            if (n == frame) break;
        }
        return false;
    }

    // Advance wheel easing and drag flings; also turns the raw drag travel of
    // this tick into a lowpassed release velocity.
    void stepScrollAnims(float dt) {
        if (dragScrolling) {
            const float blend = 1 - std::exp(-dt * 20.0f);
            dragVelX += (dragMoveX / dt - dragVelX) * blend;
            dragVelY += (dragMoveY / dt - dragVelY) * blend;
            dragMoveX = dragMoveY = 0;
        }
        bool changed = false;
        for (auto it = scrollAnims.begin(); it != scrollAnims.end();) {
            Node* n = it->first;
            ScrollAnim& a = it->second;
            float x = n->scrollX, y = n->scrollY;
            if (a.easing) {
                const float k = 1 - std::exp(-dt * kEaseRate);
                x += (a.targetX - x) * k;
                y += (a.targetY - y) * k;
                if (std::abs(a.targetX - x) < 0.25f && std::abs(a.targetY - y) < 0.25f) {
                    x = a.targetX;
                    y = a.targetY;
                    a.easing = false;
                }
                // Layout may have shrunk the range under a stale target.
                x = std::clamp(x, 0.0f, n->maxScrollX());
                y = std::clamp(y, 0.0f, n->maxScrollY());
            } else if (a.fling) {
                x = std::clamp(x + a.velX * dt, 0.0f, n->maxScrollX());
                y = std::clamp(y + a.velY * dt, 0.0f, n->maxScrollY());
                const float decay = std::exp(-dt * kFlingFriction);
                a.velX *= decay;
                a.velY *= decay;
                // Edges swallow the remaining momentum on their axis.
                if (x <= 0 || x >= n->maxScrollX()) a.velX = 0;
                if (y <= 0 || y >= n->maxScrollY()) a.velY = 0;
                if (std::hypot(a.velX, a.velY) < kFlingStop) a.fling = false;
            }
            if (x != n->scrollX || y != n->scrollY) {
                n->scrollX = x;
                n->scrollY = y;
                noteScroll(n);
                changed = true;
            }
            if (!a.easing && !a.fling) it = scrollAnims.erase(it);
            else ++it;
        }
        if (changed) renderer.markScrollDirty();
    }

    void stopScrollAnims() {
        scrollAnims.clear();
        dragVelX = dragVelY = 0;
        dragMoveX = dragMoveY = 0;
        dragConsumer = nullptr;
    }

    // ---- Long press / swipe ----

    // Advances the held press with RAW dt and fires long-press handlers once
    // the press has been still for kLongPressSec. If a handler actually ran,
    // the release stops counting as a click (suppressClick).
    void stepLongPress(float dt) {
        if (!pressed) return;
        pressElapsed += dt;  // also feeds the swipe duration check
        if (longPressDone || dragScrolling || selectingText || pressMoved) return;
        if (pressElapsed < kLongPressSec) return;
        longPressDone = true;  // evaluate at most once per press
        if (longPressHandlers.empty()) return;
        const float x = lastPointerX, y = lastPointerY;
        // fireUp guards navigation/structure changes; a handler that calls
        // bindList/setVariant also nulls `pressed` for us.
        suppressClick = fireUp(longPressHandlers, pressed,
                               [&](LongPressHandler& h, Node& n) { h(n, x, y); });
    }

    // Swipe check on release (viewport coordinates). Fires along the pressed
    // chain like click; returns true when a handler consumed the gesture.
    bool maybeFireSwipe(float x, float y) {
        if (swipeHandlers.empty() || !pressed) return false;
        if (pressElapsed >= kLongPressSec) return false;  // that's a hold
        const float dx = x - pressDownX, dy = y - pressDownY;
        if (std::abs(dx) < kSwipeMinDx || std::abs(dx) <= 2.0f * std::abs(dy)) {
            return false;
        }
        // Already consumed as HORIZONTAL scrolling? Then it was a pan, not a
        // swipe. (A vertical scroll chain clamps X, so a horizontal flick on
        // a list row still swipes — the swipe-to-delete case.)
        if (std::abs(dragConsumedX) > kDragScrollThreshold) return false;
        const char* dir = dx < 0 ? "left" : "right";
        return fireUp(swipeHandlers, pressed,
                      [&](SwipeHandler& h, Node& n) { h(n, dir); });
    }

    // ---- Text editing ----

    void setFocus(Node* n) {
        if (focused == n) return;
        if (focused) {
            focused->caretByte = -1;
            focused->selAnchorByte = -1;
        }
        focused = n;
        if (focused) {
            focused->caretByte = static_cast<int>(focused->characters.size());
            focused->selAnchorByte = -1;
        }
        renderer.markDirty();
    }

    // Viewport point → caret byte in the node (via the cached absolute
    // transform, so it stays valid under scrolling). -1 when unmappable.
    int caretByteAtViewport(Node& n, float x, float y) {
        const auto invC = renderer.contentTransform().inverted();
        const auto invA = n.absoluteTransform.inverted();
        if (!invC || !invA) return -1;
        float fx, fy, lx, ly;
        invC->apply(x, y, fx, fy);
        invA->apply(fx, fy, lx, ly);
        return renderer.textByteAtPoint(n, lx, ly);
    }

    // Active selection range of the focused node, or false when collapsed.
    bool selectionRange(size_t& lo, size_t& hi) const {
        if (!focused || focused->selAnchorByte < 0 || focused->caretByte < 0 ||
            focused->selAnchorByte == focused->caretByte) {
            return false;
        }
        const size_t len = focused->characters.size();
        lo = std::min(static_cast<size_t>(std::min(focused->selAnchorByte,
                                                   focused->caretByte)), len);
        hi = std::min(static_cast<size_t>(std::max(focused->selAnchorByte,
                                                   focused->caretByte)), len);
        return hi > lo;
    }

    // UTF-8 codepoint boundaries around the caret.
    static size_t prevCp(const std::string& s, size_t i) {
        if (i == 0) return 0;
        --i;
        while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
        return i;
    }
    static size_t nextCp(const std::string& s, size_t i) {
        if (i >= s.size()) return s.size();
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        return i;
    }

    void editChanged() {
        // Re-run the text through setNodeText so the runs match the new
        // string (and keep the per-token font fallback for CJK input).
        if (focused) setNodeText(*focused, focused->characters);
        reflow();  // auto-height boxes follow the new content
        renderer.markDirty();
    }

    // Pristine item templates detached by bindList, keyed by the list node.
    std::unordered_map<Node*, std::unique_ptr<Node>> listTemplates;

    // ---- Value binding (bindSlider) ----
    // Bindings are keyed by node NAME (like the handler maps): they survive
    // bindList/setVariant rebuilds; the track and its knob/fill children are
    // re-resolved by name on every touch, never held across frames.
    FigmaUI* self = nullptr;  // for Impl helpers that call public methods
    std::unordered_map<std::string, FigmaUI::SliderOptions> sliders;
    std::string sliderName;     // track armed by the current press ("" = none)
    bool sliderDecided = false; // axis decision made for this press
    bool sliderActive = false;  // ...and the slider won (drag drives the value)

    static float snapSliderValue(const FigmaUI::SliderOptions& o, float v) {
        if (o.step > 0 && o.max != o.min) {
            v = o.min + std::round((v - o.min) / o.step) * o.step;
        }
        return std::clamp(v, std::min(o.min, o.max), std::max(o.min, o.max));
    }

    // Nearest interactive slider track in the ancestor chain (readonly
    // progress bars never take the gesture).
    std::string sliderAtChain(Node* hit) {
        if (sliders.empty()) return {};
        for (Node* n = hit; n; n = n->parent) {
            const auto it = sliders.find(n->name);
            if (it != sliders.end() && !it->second.readonly) return n->name;
            if (n == frame) break;
        }
        return {};
    }

    // Engine-side visuals: knob translated / fill resized along the axis, in
    // frame-local space. baseTransform/baseWidth are updated in the authored
    // space too, so a Reflow relayout or a Scale-mode resetLayout reproduces
    // the same value position instead of wiping it.
    void applySliderVisuals(Node* track, const FigmaUI::SliderOptions& o) {
        const float range = o.max - o.min;
        const float t = range != 0 ? (o.value - o.min) / range : 0.0f;
        bool dirty = false;
        if (!o.knob.empty()) {
            if (Node* k = track->findByName(o.knob)) {
                if (o.axisY) {
                    k->relativeTransform.m12 =
                        t * std::max(0.0f, track->height - k->height);
                    k->baseTransform.m12 =
                        t * std::max(0.0f, track->baseHeight - k->baseHeight);
                } else {
                    k->relativeTransform.m02 =
                        t * std::max(0.0f, track->width - k->width);
                    k->baseTransform.m02 =
                        t * std::max(0.0f, track->baseWidth - k->baseWidth);
                }
                dirty = true;
            }
        }
        if (!o.fill.empty()) {
            if (Node* f = track->findByName(o.fill)) {
                if (o.axisY) {
                    f->height = t * track->height;
                    f->baseHeight = t * track->baseHeight;
                } else {
                    f->width = t * track->width;
                    f->baseWidth = t * track->baseWidth;
                }
                dirty = true;
            }
        }
        if (dirty) renderer.markDirty();
    }

    // Viewport point -> snapped slider value via the track's cached absolute
    // transform (valid under scrolling, like caret placement).
    bool sliderValueAtViewport(Node* track, const FigmaUI::SliderOptions& o,
                               float x, float y, float& out) {
        const auto invC = renderer.contentTransform().inverted();
        const auto invA = track->absoluteTransform.inverted();
        if (!invC || !invA) return false;
        float fx, fy, lx, ly;
        invC->apply(x, y, fx, fy);
        invA->apply(fx, fy, lx, ly);
        const float span = o.axisY ? track->height : track->width;
        if (span <= 0) return false;
        const float t = std::clamp((o.axisY ? ly : lx) / span, 0.0f, 1.0f);
        out = snapSliderValue(o, o.min + t * (o.max - o.min));
        return true;
    }

    // Advance the armed slider to the pointer position. Fires onChange on
    // value change (committed=false) and once on release (committed=true).
    // The track is re-found by name — onChange may rebuild the tree.
    void sliderDragUpdate(const std::string& name, float x, float y, bool committed) {
        const auto it = sliders.find(name);
        if (it == sliders.end()) return;
        Node* track = findMutable(name);
        if (!track) return;
        float v;
        if (!sliderValueAtViewport(track, it->second, x, y, v)) return;
        const bool changed = v != it->second.value;
        if (changed) {
            it->second.value = v;
            applySliderVisuals(track, it->second);
        }
        if ((changed || committed) && it->second.onChange) {
            it->second.onChange(v, committed);
        }
    }

    // ---- autoStates: hover/press -> automatic variant switching ----
    // Registered per instance NAME. The switches queue up during pointer
    // dispatch and flush AFTER it (end of the pointer function / update
    // tick): setVariant bumps structureRev, and a bump mid-dispatch would
    // consume the very click that caused it (G10 semantics).
    static constexpr float kAutoStateFadeSec = 0.12f;
    struct AutoStateSpec {
        std::string hover, pressed, base;  // "Prop=Value"
    };
    std::unordered_map<std::string, AutoStateSpec> autoStateSpecs;
    std::string autoHoverName;  // instance currently in hover state
    std::string autoPressName;  // instance currently in pressed state
    struct PendingVariant {
        std::string name, prop, value;
    };
    std::vector<PendingVariant> pendingAutoVariant;

    // Nearest registered instance name in the ancestor chain.
    std::string autoTargetOf(Node* n) {
        if (autoStateSpecs.empty()) return {};
        for (Node* p = n; p; p = p->parent) {
            if (autoStateSpecs.count(p->name)) return p->name;
            if (p == frame) break;
        }
        return {};
    }

    // state: 0 = base, 1 = hover, 2 = pressed.
    void queueAutoVariant(const std::string& name, int state) {
        const auto it = autoStateSpecs.find(name);
        if (it == autoStateSpecs.end()) return;
        const std::string& pv = state == 2   ? it->second.pressed
                                : state == 1 ? it->second.hover
                                             : it->second.base;
        const size_t eq = pv.find('=');
        if (eq == std::string::npos) return;
        pendingAutoVariant.push_back({name, pv.substr(0, eq), pv.substr(eq + 1)});
    }

    void flushAutoVariants();  // defined after setVariant (needs it)

    // Hover bookkeeping shared by pointerMove and the press release.
    void noteAutoHover(Node* hit) {
        const std::string name = autoTargetOf(hit);
        if (name == autoHoverName) return;
        if (!autoHoverName.empty() && autoHoverName != autoPressName) {
            queueAutoVariant(autoHoverName, 0);
        }
        autoHoverName = name;
        if (!name.empty()) {
            queueAutoVariant(name, name == autoPressName ? 2 : 1);
        }
    }

    // ---- setVariant dissolve (v1: fade-in of the new subtree) ----
    // Keyed by instance name (survives further rebuilds); stepped by
    // update(dt). runtimeOpacity animates 0 -> 1, then resets to authored.
    struct VariantFade {
        float elapsed = 0, duration = 0;
    };
    std::unordered_map<std::string, VariantFade> variantFades;

    void stepVariantFades(float dt) {
        if (variantFades.empty()) return;
        for (auto it = variantFades.begin(); it != variantFades.end();) {
            it->second.elapsed += dt;
            Node* n = findMutable(it->first);
            if (!n) {
                it = variantFades.erase(it);
                continue;
            }
            const float p = it->second.duration > 0
                                ? it->second.elapsed / it->second.duration
                                : 1.0f;
            n->runtimeOpacity = p >= 1.0f ? -1.0f : p;  // -1 = back to authored
            if (p >= 1.0f) it = variantFades.erase(it);
            else ++it;
        }
        renderer.markDirty();
    }

    void finishVariantFades() {
        for (auto& [name, fade] : variantFades) {
            if (Node* n = findMutable(name)) n->runtimeOpacity = -1.0f;
        }
        if (!variantFades.empty()) renderer.markDirty();
        variantFades.clear();
    }

    // ---- Navigation ----
    struct NavEntry {
        Node* frame;
        FigmaUI::Transition transition;  // used to leave it (reversed on back)
    };
    std::vector<NavEntry> navStack;
    Node* transFrom = nullptr;  // outgoing frame of the active transition
    FigmaUI::Transition transType = FigmaUI::Transition::None;
    float transElapsed = 0, transDuration = 0;
    float transProgress = 0;  // eased, consumed by the backend compositor
    uint32_t transId = 0;     // bumped per navigation → backend snapshots
    float transBandTop = -1;  // frame-local top of shared bottom chrome; <0 none

    // Shared-chrome overlay: the matched chrome subtrees of BOTH frames are
    // suppressed in the page rasters for the duration of the transition and
    // drawn by the backend as one static straight-alpha layer on top.
    std::vector<Node*> chromeNodes;      // suppressed (incoming + outgoing)
    std::vector<uint32_t> chromePixels;  // overlay raster (incoming chrome)
    uint32_t chromeW = 0, chromeH = 0;
    float chromeTopPx = 0;          // viewport-space top row of the overlay
    bool snapshotPending = false;   // chrome-free outgoing re-render owed

    void clearChromeOverlay() {
        for (Node* n : chromeNodes) n->renderSuppressed = false;
        if (!chromeNodes.empty()) renderer.markDirty();
        chromeNodes.clear();
        chromeW = chromeH = 0;
        snapshotPending = false;
    }

    Node* findFrame(const std::string& nameOrId) {
        for (Node* f : doc->topLevelFrames()) {
            if (f->name == nameOrId || f->id == nameOrId) return f;
        }
        return nullptr;
    }

    void switchToFrame(Node* f) {
        setFocus(nullptr);
        frame = f;
        renderer.setFrame(f);
        hovered = nullptr;
        pressed = nullptr;
        dragScrollNode = nullptr;
        dragScrolling = false;
        // Leaving a page mid-gesture: the old page's auto-state instances go
        // back to base (queued — we may be inside handler dispatch right now;
        // the flush happens at the end of the pointer event / update tick).
        if (!autoHoverName.empty()) queueAutoVariant(autoHoverName, 0);
        if (!autoPressName.empty() && autoPressName != autoHoverName) {
            queueAutoVariant(autoPressName, 0);
        }
        autoHoverName.clear();
        autoPressName.clear();
        sliderName.clear();
        sliderDecided = sliderActive = false;
        stopScrollAnims();
        reflow();
    }

    static FigmaUI::Transition reverseOf(FigmaUI::Transition t) {
        switch (t) {
        case FigmaUI::Transition::SlideLeft: return FigmaUI::Transition::SlideRight;
        case FigmaUI::Transition::SlideRight: return FigmaUI::Transition::SlideLeft;
        case FigmaUI::Transition::SlideUp: return FigmaUI::Transition::SlideDown;
        case FigmaUI::Transition::SlideDown: return FigmaUI::Transition::SlideUp;
        default: return t;
        }
    }

    void startTransition(Node* from, FigmaUI::Transition t, float duration) {
        ++transId;  // even a cut (no animation) invalidates a backend snapshot
        clearChromeOverlay();  // an interrupted transition keeps no suppression
        if (t == FigmaUI::Transition::None || duration <= 0 || !from || from == frame) {
            transFrom = nullptr;
            return;
        }
        transFrom = from;
        transType = t;
        transElapsed = 0;
        transDuration = duration;
        transProgress = 0;
        std::vector<Node*> incomingChrome;
        findStaticBottomChrome(from, incomingChrome);
        if (transBandTop < 0) return;
        // Rasterize the overlay BEFORE suppressing (the builder skips
        // suppressed nodes), then hide the chrome from both page rasters.
        float x = 0;
        chromeTopPx = 0;
        renderer.contentTransform().apply(0, transBandTop, x, chromeTopPx);
        // 2px of anti-aliasing headroom: a shape whose geometric top lands
        // exactly on the band line still feathers a row above it. The extra
        // overlay rows are transparent, so the sliding pages show through.
        chromeTopPx = std::clamp(std::floor(chromeTopPx) - 2.0f, 0.0f,
                                 static_cast<float>(renderer.height()));
        if (renderer.renderOverlay(incomingChrome, chromeTopPx, chromePixels,
                                   chromeW, chromeH)) {
            for (Node* n : chromeNodes) n->renderSuppressed = true;
            snapshotPending = true;
        } else {
            // Overlay raster failed → the backend falls back to the static
            // band (transitionStaticBottomY) over unmodified page textures.
            clearChromeOverlay();
        }
    }

    // How far a node's own style paints above its layout rect: outside and
    // center strokes, drop shadows and layer blurs all bleed upward.
    static float paintBleedTop(const Node& n) {
        float bleed = 0;
        for (const auto& s : n.strokes) {
            if (!s.visible) continue;
            if (n.strokeAlign == StrokeAlign::Outside) bleed = n.strokeWeight;
            if (n.strokeAlign == StrokeAlign::Center) bleed = n.strokeWeight * 0.5f;
            break;
        }
        for (const auto& e : n.effects) {
            if (!e.visible) continue;
            if (e.type == Effect::Type::DropShadow) {
                bleed = std::max(bleed, e.radius + e.spread - e.offsetY);
            } else if (e.type == Effect::Type::LayerBlur) {
                bleed = std::max(bleed, e.radius);
            }
        }
        return bleed;
    }

    // Visual top of `n` in frame space (`y` = n's own top): non-clipping
    // containers let children overflow upward (e.g. a round action button
    // poking above the tab bar), and the static band must cover the full
    // painted extent or the overhang gets sliced at the band line.
    static float visualTop(const Node& n, float y) {
        float top = y - paintBleedTop(n);
        if (n.clipsContent) return top;
        for (const auto& ch : n.children) {
            if (!ch->effectivelyVisible()) continue;
            top = std::min(top, visualTop(*ch, y + ch->relativeTransform.m12));
        }
        return top;
    }

    // Native tab-bar semantics: bottom-anchored scrollFixed top-level
    // elements present in both frames with the same name and geometry stay
    // put while the pages slide. Fills chromeNodes with the matched subtrees
    // of both frames (to suppress) and outIncoming with the current frame's
    // copies (the overlay raster). Full-frame fixed overlays (status bars
    // span from y=0) are excluded via the lower-half test.
    void findStaticBottomChrome(Node* from, std::vector<Node*>& outIncoming) {
        transBandTop = -1;
        if (!frame || !from) return;
        for (const auto& c : frame->children) {
            if (!c->scrollFixed || !c->effectivelyVisible()) continue;
            const float cy = c->relativeTransform.m12;
            if (cy < frame->height * 0.5f) continue;           // bottom chrome only
            if (cy + c->height < frame->height - 4) continue;  // anchored to the bottom
            Node* o = nullptr;
            for (const auto& fc : from->children) {
                if (fc->name == c->name) {
                    o = fc.get();
                    break;
                }
            }
            if (!o || !o->scrollFixed || !o->effectivelyVisible()) continue;
            if (std::abs(o->relativeTransform.m02 - c->relativeTransform.m02) > 1.5f ||
                std::abs(o->relativeTransform.m12 - cy) > 1.5f ||
                std::abs(o->width - c->width) > 1.5f ||
                std::abs(o->height - c->height) > 1.5f) {
                continue;
            }
            // Band at the painted top edge of both copies — the bar's frame
            // top would slice off anything overhanging it.
            const float top = std::min(visualTop(*c, cy),
                                       visualTop(*o, o->relativeTransform.m12));
            transBandTop = transBandTop < 0 ? top : std::min(transBandTop, top);
            chromeNodes.push_back(c.get());
            chromeNodes.push_back(o);
            outIncoming.push_back(c.get());
        }
    }

    // Maps an authored Figma transition type onto our animation set.
    static FigmaUI::Transition fromAuthored(const std::string& type) {
        if (type.find("DISSOLVE") != std::string::npos ||
            type.find("SMART") != std::string::npos) {
            return FigmaUI::Transition::Dissolve;
        }
        if (type.find("RIGHT") != std::string::npos) return FigmaUI::Transition::SlideRight;
        if (type.find("TOP") != std::string::npos || type.find("UP") != std::string::npos) {
            return FigmaUI::Transition::SlideUp;
        }
        if (type.find("BOTTOM") != std::string::npos ||
            type.find("DOWN") != std::string::npos) {
            return FigmaUI::Transition::SlideDown;
        }
        if (type.empty()) return FigmaUI::Transition::Dissolve;
        return FigmaUI::Transition::SlideLeft;
    }
};

FigmaUI::FigmaUI() : impl_(std::make_unique<Impl>()) { impl_->self = this; }
FigmaUI::~FigmaUI() = default;

std::unique_ptr<FigmaUI> FigmaUI::fromFile(const std::string& path) {
    auto ui = std::unique_ptr<FigmaUI>(new FigmaUI());
    LoadedFile loaded = loadFigmaFile(path);  // .fig / canvas.json / REST JSON
    ui->impl_->doc = std::move(loaded.document);
    if (!loaded.imageDirectory.empty()) {
        ui->impl_->renderer.setImageDirectory(loaded.imageDirectory);
    }
    // Conventions for design fonts: a "fonts" directory next to the input
    // file, plus the FIGO_FONTS_DIR environment variable.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path sibling = fs::path(path).parent_path() / "fonts";
        if (fs::is_directory(sibling, ec)) {
            ui->impl_->renderer.registerFontsFromDirectory(sibling.string());
        }
        if (const char* env = std::getenv("FIGO_FONTS_DIR"); env && *env) {
            if (fs::is_directory(env, ec)) {
                ui->impl_->renderer.registerFontsFromDirectory(env);
            }
        }
    }
    auto frames = ui->impl_->doc->topLevelFrames();
    if (!frames.empty()) {
        ui->impl_->frame = frames.front();
        ui->impl_->renderer.setFrame(frames.front());
    }
    return ui;
}

std::unique_ptr<FigmaUI> FigmaUI::fromJson(const std::string& jsonText) {
    auto ui = std::unique_ptr<FigmaUI>(new FigmaUI());
    ui->impl_->doc = parseDocument(jsonText);
    auto frames = ui->impl_->doc->topLevelFrames();
    if (!frames.empty()) {
        ui->impl_->frame = frames.front();
        ui->impl_->renderer.setFrame(frames.front());
    }
    return ui;
}

Document& FigmaUI::document() { return *impl_->doc; }
Renderer& FigmaUI::renderer() { return impl_->renderer; }

std::vector<std::string> FigmaUI::frameNames() const {
    std::vector<std::string> names;
    for (Node* f : impl_->doc->topLevelFrames()) names.push_back(f->name);
    return names;
}

bool FigmaUI::selectFrame(const std::string& name) {
    Node* f = impl_->findFrame(name);
    if (!f) return false;
    impl_->transFrom = nullptr;
    impl_->clearChromeOverlay();
    ++impl_->transId;  // hard switch: a backend snapshot of the old frame is stale
    impl_->navStack.clear();  // hard switch resets navigation history
    impl_->switchToFrame(f);
    return true;
}

bool FigmaUI::navigateTo(const std::string& frameName, Transition transition,
                         float durationSec) {
    Node* f = impl_->findFrame(frameName);
    if (!f || f == impl_->frame) return false;
    Node* from = impl_->frame;
    impl_->navStack.push_back({from, transition});
    impl_->switchToFrame(f);
    impl_->startTransition(from, transition, durationSec);
    impl_->renderer.markDirty();
    return true;
}

bool FigmaUI::navigateBack(float durationSec) {
    if (impl_->navStack.empty()) return false;
    const Impl::NavEntry entry = impl_->navStack.back();
    impl_->navStack.pop_back();
    Node* from = impl_->frame;
    impl_->switchToFrame(entry.frame);
    impl_->startTransition(from, Impl::reverseOf(entry.transition), durationSec);
    impl_->renderer.markDirty();
    return true;
}

bool FigmaUI::canGoBack() const { return !impl_->navStack.empty(); }

void FigmaUI::update(float dtSeconds) {
    if (dtSeconds <= 0) return;
    impl_->stepLongPress(dtSeconds);  // real held time, before the clamp
    // Animation clock, not wall clock: a hitch (e.g. the incoming frame's
    // first scene build can take hundreds of ms) consumes one slow tick of
    // the animation instead of swallowing the whole transition.
    dtSeconds = std::min(dtSeconds, 1.0f / 30.0f);
    impl_->stepScrollAnims(dtSeconds);
    if (impl_->transFrom) {
        impl_->transElapsed += dtSeconds;
        const float p =
            impl_->transDuration > 0 ? impl_->transElapsed / impl_->transDuration : 1.0f;
        if (p >= 1.0f) {
            impl_->transFrom = nullptr;
            impl_->clearChromeOverlay();  // restore the chrome into the page raster
        } else {
            // Ease in-out (cubic) for a Figma-like feel. No re-raster happens
            // here: the backend composites its cached textures at this
            // progress.
            impl_->transProgress =
                p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.0f) / 2;
        }
    }
    impl_->stepVariantFades(dtSeconds);  // setVariant dissolve fade-ins
    // All of this frame's scroll changes (wheel easing, fling, drag, script
    // writes) coalesce into one onScroll dispatch per node per frame.
    impl_->dispatchScrollEvents();
    // Auto-state switches queued during dispatch (e.g. a handler navigated)
    // apply now, after every handler ran.
    impl_->flushAutoVariants();
}

bool FigmaUI::animating() const { return impl_->transFrom != nullptr; }

uint32_t FigmaUI::transitionId() const { return impl_->transId; }

FigmaUI::Transition FigmaUI::transitionType() const {
    return impl_->transFrom ? impl_->transType : Transition::None;
}

float FigmaUI::transitionProgress() const {
    return impl_->transFrom ? impl_->transProgress : 1.0f;
}

bool FigmaUI::renderTransitionSnapshot() {
    auto& d = *impl_;
    if (!d.snapshotPending || !d.transFrom || !d.frame) return false;
    d.snapshotPending = false;
    // Re-rasterize the outgoing frame into the current target; its chrome is
    // already suppressed. The incoming frame re-rasters on the next render().
    Node* incoming = d.frame;
    d.renderer.setFrame(d.transFrom);
    d.renderer.markDirty();
    const bool ok = d.renderer.render();
    d.renderer.setFrame(incoming);
    d.renderer.markDirty();
    return ok;
}

const uint32_t* FigmaUI::transitionChromePixels(uint32_t& width, uint32_t& height,
                                                float& y) const {
    const auto& d = *impl_;
    if (!d.transFrom || d.chromeW == 0 || d.chromeH == 0) return nullptr;
    width = d.chromeW;
    height = d.chromeH;
    y = d.chromeTopPx;
    return d.chromePixels.data();
}

float FigmaUI::transitionStaticBottomY() const {
    const float full = static_cast<float>(impl_->renderer.height());
    if (!impl_->transFrom || !impl_->frame || impl_->transBandTop < 0) return full;
    float x, y;
    impl_->renderer.contentTransform().apply(0, impl_->transBandTop, x, y);
    return std::min(full, y);
}

Node* FigmaUI::currentFrame() const { return impl_->frame; }

void FigmaUI::setResizeMode(ResizeMode mode) {
    if (impl_->resizeMode == mode) return;
    impl_->resizeMode = mode;
    if (mode == ResizeMode::Scale && impl_->frame) {
        resetLayout(*impl_->frame);  // back to the authored geometry
        impl_->clampScrollOffsets();
        impl_->renderer.markDirty();
    } else {
        impl_->reflow();
    }
}

FigmaUI::ResizeMode FigmaUI::resizeMode() const { return impl_->resizeMode; }

void FigmaUI::setViewport(uint32_t width, uint32_t height) {
    const bool changed = width != impl_->renderer.width() || height != impl_->renderer.height();
    impl_->renderer.setTarget(width, height);
    if (changed) impl_->reflow();
}

bool FigmaUI::setViewportGL(int32_t fboId, uint32_t width, uint32_t height) {
    const bool changed = width != impl_->renderer.width() || height != impl_->renderer.height();
    if (!impl_->renderer.setTargetGL(fboId, width, height)) return false;
    if (changed) impl_->reflow();
    return true;
}

bool FigmaUI::render() { return impl_->renderer.render(); }
const uint32_t* FigmaUI::pixels() const { return impl_->renderer.pixels(); }
uint32_t FigmaUI::pixelWidth() const { return impl_->renderer.width(); }
uint32_t FigmaUI::pixelHeight() const { return impl_->renderer.height(); }
void FigmaUI::markDirty() { impl_->renderer.markDirty(); }

void FigmaUI::pointerMove(float x, float y) {
    if (impl_->selectingText && impl_->focused) {
        // Drag-select: the caret follows the pointer, the anchor stays put.
        const int b = impl_->caretByteAtViewport(*impl_->focused, x, y);
        if (b >= 0 && b != impl_->focused->caretByte) {
            impl_->focused->caretByte = b;
            impl_->renderer.markDirty();
        }
        impl_->lastPointerX = x;
        impl_->lastPointerY = y;
        return;  // no hover churn / scrolling while selecting
    }
    // Gesture bookkeeping: once the pointer strays past the drag-scroll
    // threshold this press can no longer become a long press.
    if (impl_->pressed && !impl_->pressMoved &&
        std::hypot(x - impl_->pressDownX, y - impl_->pressDownY) >
            Impl::kDragScrollThreshold) {
        impl_->pressMoved = true;
    }
    // Armed slider: decide the gesture's axis once it travels past the drag
    // threshold. Along the slider's axis the slider wins (over scrolling);
    // across it the press is handed back to normal drag scrolling.
    if (impl_->pressed && !impl_->sliderName.empty()) {
        if (!impl_->sliderDecided) {
            const float dx = x - impl_->pressDownX, dy = y - impl_->pressDownY;
            if (std::hypot(dx, dy) > Impl::kDragScrollThreshold) {
                impl_->sliderDecided = true;
                const auto it = impl_->sliders.find(impl_->sliderName);
                const bool axisY = it != impl_->sliders.end() && it->second.axisY;
                const float along = axisY ? dy : dx, cross = axisY ? dx : dy;
                if (std::abs(along) >= std::abs(cross)) {
                    impl_->sliderActive = true;
                    impl_->dragScrollNode = nullptr;  // same axis: slider wins
                    impl_->sliderDragUpdate(impl_->sliderName, x, y, false);
                } else {
                    impl_->sliderName.clear();  // cross axis: scrolling's turn
                }
            }
        } else if (impl_->sliderActive) {
            impl_->sliderDragUpdate(impl_->sliderName, x, y, false);
        }
        if (impl_->sliderActive) {  // no hover churn / scrolling while sliding
            impl_->lastPointerX = x;
            impl_->lastPointerY = y;
            return;
        }
    }
    if (impl_->pressed && impl_->dragScrollNode) {
        const float dx = x - impl_->lastPointerX, dy = y - impl_->lastPointerY;
        impl_->lastPointerX = x;
        impl_->lastPointerY = y;
        if (!impl_->dragScrolling) {
            impl_->dragAccumX += dx;
            impl_->dragAccumY += dy;
            if (std::hypot(impl_->dragAccumX, impl_->dragAccumY) >
                Impl::kDragScrollThreshold) {
                impl_->dragScrolling = true;
                // Replay the pre-threshold travel so the content doesn't jump.
                const float s = impl_->viewportToFrameScale();
                impl_->dragConsumer = impl_->scrollWithBubble(
                    impl_->dragScrollNode, -impl_->dragAccumX * s, -impl_->dragAccumY * s);
                impl_->dragMoveX = -impl_->dragAccumX * s;
                impl_->dragMoveY = -impl_->dragAccumY * s;
            }
        } else {
            // Content follows the finger: dragging up reveals what is below.
            const float s = impl_->viewportToFrameScale();
            if (Node* c = impl_->scrollWithBubble(impl_->dragScrollNode, -dx * s, -dy * s)) {
                impl_->dragConsumer = c;
            }
            impl_->dragMoveX += -dx * s;
            impl_->dragMoveY += -dy * s;
        }
        if (impl_->dragScrolling) return;  // no hover churn while panning
    }

    Node* hit = impl_->hitTestViewport(x, y);
    if (hit == impl_->hovered) return;
    const uint64_t rev = impl_->structureRev;
    if (impl_->hovered) {
        impl_->fireUp(impl_->hoverHandlers, impl_->hovered,
                      [&](HoverHandler& h, Node& n) { h(n, false, x, y); });
    }
    // A leave handler may have rebuilt the tree; `hit` would be dangling.
    if (impl_->structureRev != rev) hit = impl_->hitTestViewport(x, y);
    impl_->hovered = hit;
    if (hit) {
        const uint64_t revEnter = impl_->structureRev;
        impl_->fireUp(impl_->hoverHandlers, hit,
                      [&](HoverHandler& h, Node& n) { h(n, true, x, y); });
        // The enter handler may have rebuilt the tree too.
        if (impl_->structureRev != revEnter) {
            hit = impl_->hitTestViewport(x, y);
            impl_->hovered = hit;
        }
    }
    // autoStates: hover enter/leave switches variants — queued during
    // dispatch, applied after it (so the rebuild can't eat this event).
    impl_->noteAutoHover(hit);
    impl_->flushAutoVariants();
}

void FigmaUI::pointerDown(float x, float y) {
    impl_->pressed = impl_->hitTestViewport(x, y);

    // Pressing inside the already-focused text: place the caret there and arm
    // drag-selection; the gesture belongs to the text, not to scrolling.
    impl_->selectingText = false;
    if (impl_->focused) {
        for (Node* n = impl_->pressed; n; n = n->parent) {
            if (n == impl_->focused) {
                const int b = impl_->caretByteAtViewport(*impl_->focused, x, y);
                if (b >= 0) {
                    impl_->focused->caretByte = b;
                    impl_->focused->selAnchorByte = b;
                    impl_->selectingText = true;
                    impl_->renderer.markDirty();
                }
                break;
            }
            if (n == impl_->frame) break;
        }
    }

    impl_->dragScrollNode =
        impl_->selectingText ? nullptr : impl_->scrollableAtViewport(x, y);
    impl_->dragScrolling = false;
    impl_->dragAccumX = impl_->dragAccumY = 0;
    impl_->lastPointerX = x;
    impl_->lastPointerY = y;
    // Arm gesture recognition for this press.
    impl_->pressDownX = x;
    impl_->pressDownY = y;
    impl_->pressElapsed = 0;
    impl_->pressMoved = false;
    impl_->longPressDone = false;
    impl_->suppressClick = false;
    impl_->dragConsumedX = 0;
    impl_->stopScrollAnims();  // the finger catches any easing/fling in flight

    // Arm slider capture: a press on a bound track (or its subtree) may
    // become a value drag once the axis decision falls its way.
    impl_->sliderName =
        impl_->selectingText ? std::string() : impl_->sliderAtChain(impl_->pressed);
    impl_->sliderDecided = false;
    impl_->sliderActive = false;

    // autoStates: press state (wins over hover), applied after this event.
    impl_->autoPressName = impl_->autoTargetOf(impl_->pressed);
    if (!impl_->autoPressName.empty()) {
        impl_->queueAutoVariant(impl_->autoPressName, 2);
    }
    impl_->flushAutoVariants();
}

void FigmaUI::pointerUp(float x, float y) {
    pointerUpMain(x, y);
    // autoStates: settle the pressed instance — back to hover when released
    // over it, base otherwise. Done in this wrapper so every early-return
    // path of the main handler (drag release, swipe, text) is covered, and
    // the variant swap happens strictly AFTER click dispatch.
    auto& d = *impl_;
    if (!d.autoPressName.empty()) {  // a navigation already cleared + queued
        const std::string name = d.autoPressName;
        d.autoPressName.clear();
        Node* now = d.hitTestViewport(x, y);
        if (d.autoTargetOf(now) == name) {
            d.queueAutoVariant(name, 1);
            d.autoHoverName = name;
        } else {
            d.queueAutoVariant(name, 0);
            if (d.autoHoverName == name) d.autoHoverName.clear();
            d.noteAutoHover(now);
        }
    }
    d.flushAutoVariants();
}

void FigmaUI::pointerUpMain(float x, float y) {
    if (impl_->selectingText) {
        impl_->selectingText = false;
        Node* f = impl_->focused;
        if (f && f->selAnchorByte == f->caretByte) f->selAnchorByte = -1;  // plain click
        impl_->pressed = nullptr;  // editing gesture, not a click
        impl_->dragScrollNode = nullptr;
        return;
    }
    if (impl_->dragScrolling) {
        // The gesture was a pan, not a click. Fast release → fling.
        Node* n = impl_->dragConsumer ? impl_->dragConsumer : impl_->dragScrollNode;
        if (n && std::hypot(impl_->dragVelX, impl_->dragVelY) > Impl::kFlingStart) {
            Impl::ScrollAnim& a = impl_->scrollAnims[n];
            a.velX = impl_->dragVelX;
            a.velY = impl_->dragVelY;
            a.fling = true;
            a.easing = false;
        }
        impl_->dragVelX = impl_->dragVelY = 0;
        impl_->dragMoveX = impl_->dragMoveY = 0;
        impl_->dragConsumer = nullptr;
        // A horizontal flick whose X the scroll chain clamped away (vertical
        // list) is still a swipe — the swipe-to-delete gesture.
        impl_->maybeFireSwipe(x, y);
        impl_->pressed = nullptr;
        impl_->dragScrollNode = nullptr;
        impl_->dragScrolling = false;
        return;
    }
    impl_->dragScrollNode = nullptr;

    // Slider release: an axis-won drag commits its final value; a tap on the
    // track jumps to the tapped position and commits. Either way the release
    // is the slider's (no click, no focus change).
    if (!impl_->sliderName.empty()) {
        const std::string name = impl_->sliderName;
        const bool take = impl_->sliderActive || !impl_->sliderDecided;
        impl_->sliderName.clear();
        impl_->sliderDecided = impl_->sliderActive = false;
        if (take) {
            impl_->sliderDragUpdate(name, x, y, true);
            impl_->pressed = nullptr;
            return;
        }
    }

    // A swipe or a fired long press consumes the release: no focus change,
    // no click, no prototype navigation.
    if (impl_->maybeFireSwipe(x, y) || impl_->suppressClick) {
        impl_->pressed = nullptr;
        return;
    }

    Node* hit = impl_->hitTestViewport(x, y);
    Node* frameAtClick = impl_->frame;  // handlers may navigate mid-dispatch
    const uint64_t revAtClick = impl_->structureRev;  // ... or rebuild the tree

    // Focus follows the click: nearest editable text in the hit chain wins;
    // clicking anywhere else (or outside the frame) blurs.
    Node* toFocus = nullptr;
    for (Node* n = hit; n; n = n->parent) {
        if (n->editable && n->type == NodeType::Text) {
            toFocus = n;
            break;
        }
        if (n == impl_->frame) break;
    }
    impl_->setFocus(toFocus);
    if (toFocus) {  // caret lands where the click was, not at the end
        const int b = impl_->caretByteAtViewport(*toFocus, x, y);
        if (b >= 0) {
            toFocus->caretByte = b;
            toFocus->selAnchorByte = -1;
            impl_->renderer.markDirty();
        }
    }
    if (hit && impl_->pressed) {
        // Click counts if press and release share the hit node or an ancestor.
        Node* target = nullptr;
        for (Node* n = hit; n; n = n->parent) {
            if (n == impl_->pressed) {
                target = n;
                break;
            }
            if (n == impl_->frame) break;
        }
        if (!target) {
            for (Node* n = impl_->pressed; n; n = n->parent) {
                if (n == hit) {
                    target = n;
                    break;
                }
                if (n == impl_->frame) break;
            }
        }
        if (target) {
            impl_->fireUp(impl_->clickHandlers, target,
                          [&](ClickHandler& h, Node& n) { h(n, x, y); });
        }
        // Authored Figma prototype link anywhere in the chain — but only when
        // no click handler already navigated (navigation consumes the click)
        // and the hit chain is still alive (a handler may have rebuilt it).
        if (impl_->frame == frameAtClick && impl_->structureRev == revAtClick) {
            for (Node* n = hit; n; n = n->parent) {
                if (!n->transitionNodeId.empty()) {
                    navigateTo(n->transitionNodeId, Impl::fromAuthored(n->transitionType),
                               n->transitionDuration > 0 ? n->transitionDuration : 0.3f);
                    break;
                }
                if (n == frameAtClick) break;
            }
        }
    }
    impl_->pressed = nullptr;
}

Node* FigmaUI::hitTest(float x, float y) { return impl_->hitTestViewport(x, y); }
Node* FigmaUI::hoveredNode() const { return impl_->hovered; }
Node* FigmaUI::pressedNode() const { return impl_->pressed; }

bool FigmaUI::scrollBy(float x, float y, float dx, float dy) {
    Node* target = impl_->scrollableAtViewport(x, y);
    if (!target) return false;
    const float s = impl_->viewportToFrameScale();
    return impl_->smoothScrollBy(target, dx * s, dy * s);
}

bool FigmaUI::setScroll(const std::string& nodeName, float offsetX, float offsetY) {
    Node* n = impl_->findMutable(nodeName);
    return n && setScroll(*n, offsetX, offsetY);
}

bool FigmaUI::setScroll(Node& node, float offsetX, float offsetY) {
    if (!node.scrolls()) return false;
    impl_->scrollAnims.erase(&node);  // programmatic set overrides any animation
    const float nx = std::clamp(offsetX, 0.0f, node.maxScrollX());
    const float ny = std::clamp(offsetY, 0.0f, node.maxScrollY());
    if (nx != node.scrollX || ny != node.scrollY) {
        node.scrollX = nx;
        node.scrollY = ny;
        impl_->noteScroll(&node);
    }
    impl_->renderer.markScrollDirty();
    return true;
}

Node* FigmaUI::scrollableAt(float x, float y) {
    return impl_->scrollableAtViewport(x, y);
}

bool FigmaUI::setEditable(const std::string& nodeName, bool editable) {
    Node* n = impl_->findMutable(nodeName);
    if (!n || n->type != NodeType::Text) return false;
    n->editable = editable;
    if (!editable && impl_->focused == n) impl_->setFocus(nullptr);
    return true;
}

bool FigmaUI::focusText(const std::string& nodeName) {
    Node* n = impl_->findMutable(nodeName);
    if (!n || n->type != NodeType::Text) return false;
    impl_->setFocus(n);
    return true;
}

void FigmaUI::blur() { impl_->setFocus(nullptr); }

Node* FigmaUI::focusedNode() const { return impl_->focused; }

void FigmaUI::textInput(const std::string& utf8) {
    Node* n = impl_->focused;
    if (!n || utf8.empty()) return;
    // Keep printable text and newlines; hosts feed raw key events elsewhere.
    std::string clean;
    clean.reserve(utf8.size());
    for (char c : utf8) {
        if (c == '\r') continue;
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n') continue;
        clean.push_back(c);
    }
    if (clean.empty()) return;
    size_t at;
    size_t lo, hi;
    if (impl_->selectionRange(lo, hi)) {  // typing replaces the selection
        n->characters.erase(lo, hi - lo);
        at = lo;
    } else {
        const size_t len = n->characters.size();
        at = n->caretByte < 0 ? len : std::min(static_cast<size_t>(n->caretByte), len);
    }
    n->selAnchorByte = -1;
    n->characters.insert(at, clean);
    n->caretByte = static_cast<int>(at + clean.size());
    impl_->editChanged();
}

void FigmaUI::editKey(EditKey key) {
    Node* n = impl_->focused;
    if (!n) return;
    std::string& s = n->characters;
    size_t caret = n->caretByte < 0
                       ? s.size()
                       : std::min(static_cast<size_t>(n->caretByte), s.size());
    size_t lo, hi;
    const bool hasSel = impl_->selectionRange(lo, hi);
    switch (key) {
    case EditKey::Left: caret = hasSel ? lo : Impl::prevCp(s, caret); break;
    case EditKey::Right: caret = hasSel ? hi : Impl::nextCp(s, caret); break;
    case EditKey::Home: {
        const size_t nl = caret == 0 ? std::string::npos : s.rfind('\n', caret - 1);
        caret = nl == std::string::npos ? 0 : nl + 1;
        break;
    }
    case EditKey::End: {
        const size_t nl = s.find('\n', caret);
        caret = nl == std::string::npos ? s.size() : nl;
        break;
    }
    case EditKey::Backspace:
        if (hasSel) {
            s.erase(lo, hi - lo);
            caret = lo;
            n->selAnchorByte = -1;
            impl_->editChanged();
        } else if (caret > 0) {
            const size_t b = Impl::prevCp(s, caret);
            s.erase(b, caret - b);
            caret = b;
            impl_->editChanged();
        }
        break;
    case EditKey::Delete:
        if (hasSel) {
            s.erase(lo, hi - lo);
            caret = lo;
            n->selAnchorByte = -1;
            impl_->editChanged();
        } else if (caret < s.size()) {
            s.erase(caret, Impl::nextCp(s, caret) - caret);
            impl_->editChanged();
        }
        break;
    }
    n->selAnchorByte = -1;  // any edit key collapses the selection
    n->caretByte = static_cast<int>(caret);
    impl_->renderer.markDirty();
}

std::string FigmaUI::editCopy() {
    Node* n = impl_->focused;
    size_t lo, hi;
    // passwordMask: the selection is bullets on screen but plaintext in the
    // node — never hand it out.
    if (!n || n->passwordMask || !impl_->selectionRange(lo, hi)) return {};
    return n->characters.substr(lo, hi - lo);
}

std::string FigmaUI::editCut() {
    std::string copied = editCopy();
    if (copied.empty()) return copied;  // includes the passwordMask case
    Node* n = impl_->focused;
    size_t lo, hi;
    if (impl_->selectionRange(lo, hi)) {
        n->characters.erase(lo, hi - lo);
        n->caretByte = static_cast<int>(lo);
        n->selAnchorByte = -1;
        impl_->editChanged();
    }
    return copied;
}

void FigmaUI::editPaste(const std::string& utf8) {
    textInput(utf8);  // replaces the selection, filters \r/control, keeps \n
}

void FigmaUI::editSelectAll() {
    Node* n = impl_->focused;
    if (!n) return;
    n->selAnchorByte = 0;
    n->caretByte = static_cast<int>(n->characters.size());
    impl_->renderer.markDirty();
}

void FigmaUI::onClick(const std::string& nodeName, ClickHandler fn) {
    impl_->clickHandlers[nodeName].push_back(std::move(fn));
}

void FigmaUI::onHover(const std::string& nodeName, HoverHandler fn) {
    impl_->hoverHandlers[nodeName].push_back(std::move(fn));
}

void FigmaUI::onLongPress(const std::string& nodeName, LongPressHandler fn) {
    impl_->longPressHandlers[nodeName].push_back(std::move(fn));
}

void FigmaUI::onSwipe(const std::string& nodeName, SwipeHandler fn) {
    impl_->swipeHandlers[nodeName].push_back(std::move(fn));
}

void FigmaUI::onScroll(const std::string& nodeName, ScrollHandler fn) {
    impl_->scrollHandlers[nodeName].push_back(std::move(fn));
}

void FigmaUI::clearHandlers() {
    impl_->clickHandlers.clear();
    impl_->hoverHandlers.clear();
    impl_->longPressHandlers.clear();
    impl_->swipeHandlers.clear();
    impl_->scrollHandlers.clear();
    impl_->pendingScroll.clear();
    // Value bindings and auto-states are script-registered like handlers:
    // hot reload re-registers them from scratch.
    impl_->sliders.clear();
    impl_->sliderName.clear();
    impl_->sliderDecided = impl_->sliderActive = false;
    impl_->autoStateSpecs.clear();
    impl_->pendingAutoVariant.clear();
    impl_->autoHoverName.clear();
    impl_->autoPressName.clear();
    impl_->finishVariantFades();  // don't leave a half-faded subtree behind
}

bool FigmaUI::bindList(const std::string& listName, size_t count,
                       const ListBinder& bind) {
    Node* list = impl_->findMutable(listName);
    if (!list) return false;
    impl_->stopScrollAnims();  // items are about to be destroyed/recreated

    // First bind detaches the template; later binds reuse the stored one.
    auto it = impl_->listTemplates.find(list);
    if (it == impl_->listTemplates.end()) {
        std::unique_ptr<Node> tmpl;
        for (auto& c : list->children) {
            if (c->type != NodeType::Slice) {
                tmpl = std::move(c);
                break;
            }
        }
        if (!tmpl) return false;
        it = impl_->listTemplates.emplace(list, std::move(tmpl)).first;
    }

    impl_->setFocus(nullptr);  // the focused node may be a list item
    impl_->hovered = nullptr;
    impl_->pressed = nullptr;
    impl_->structureRev++;  // list items are about to be freed
    impl_->pendingScroll.clear();  // queued Node*s may point into them
    // A data-driven list grows with its data: hug the main axis and pack from
    // the start (a fixed CENTER stack would overlap its surroundings).
    // Exception: the design says the list scrolls along its main axis — then
    // the authored size IS the viewport and overflow feeds the scroll range
    // instead (hugging would grow the box and pin maxScroll at 0).
    if (list->autoLayout.enabled()) {
        const bool vertical = list->autoLayout.mode == AutoLayout::Mode::Vertical;
        const bool mainAxisScrolls =
            list->scrollDirection == ScrollDirection::Both ||
            list->scrollDirection == (vertical ? ScrollDirection::Vertical
                                               : ScrollDirection::Horizontal);
        if (!mainAxisScrolls) list->autoLayout.primarySizing = AutoLayout::Sizing::Hug;
        list->autoLayout.primaryAlign = AutoLayout::Align::Min;
    }
    list->children.clear();
    for (size_t i = 0; i < count; ++i) {
        list->children.push_back(cloneNode(*it->second, list));
        if (bind) bind(*list->children.back(), i);
    }

    relayoutNode(*list);  // re-stack the clones, grow hug axes
    impl_->reflow();      // Reflow mode: ancestors (incl. hug chains) follow
    impl_->clampScrollOffsets();  // fewer items can shrink the scroll range
    impl_->renderer.markDirty();
    return true;
}

bool FigmaUI::setVisible(const std::string& nodeName, bool visible) {
    Node* n = impl_->findMutable(nodeName);
    if (!n) return false;
    n->runtimeVisible = visible ? 1 : 0;
    impl_->renderer.markDirty();
    return true;
}

bool FigmaUI::setOpacity(const std::string& nodeName, float opacity) {
    Node* n = impl_->findMutable(nodeName);
    if (!n) return false;
    n->runtimeOpacity = opacity;
    impl_->renderer.markDirty();
    return true;
}

bool FigmaUI::setText(const std::string& nodeName, const std::string& text) {
    Node* n = impl_->findMutable(nodeName);
    if (!n || n->type != NodeType::Text) return false;
    setNodeText(*n, text);
    impl_->reflow();  // auto-height text can change the layout around it
    impl_->renderer.markDirty();
    return true;
}

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string px(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

}  // namespace

std::vector<Diagnostic> FigmaUI::diagnostics() const {
    std::vector<Diagnostic> out;
    Node* frame = impl_->frame;
    if (!frame) return out;
    Renderer& renderer = impl_->renderer;
    constexpr float kTol = 1.0f;  // px slack for AA / rounding noise

    auto add = [&](const char* kind, const Node& n, std::string message) {
        out.push_back({kind, n.name, n.id, std::move(message)});
    };

    std::function<void(Node&)> walk = [&](Node& n) {
        if (!n.effectivelyVisible()) return;

        if (n.type == NodeType::Text && !n.characters.empty()) {
            // font-fallback: resolve every requested (family, weight, italic)
            // the way rendering does and compare the family it landed on.
            std::vector<std::string> seen;
            auto checkFont = [&](const TextStyle& ts) {
                const std::string want = lowerAscii(ts.fontFamily);
                const std::string sig =
                    want + "|" + std::to_string(ts.fontWeight) + (ts.italic ? "|i" : "");
                if (std::find(seen.begin(), seen.end(), sig) != seen.end()) return;
                seen.push_back(sig);
                std::string got;
                if (!renderer.resolveFontFamily(ts.fontFamily, ts.fontWeight, ts.italic,
                                                got)) {
                    add("font-fallback", n,
                        "font \"" + ts.fontFamily + "\" not found and no substitute "
                        "could be loaded -- text may not render");
                } else if (got != want) {
                    add("font-fallback", n,
                        "font \"" + ts.fontFamily + "\" not found; rendered with \"" +
                            got + "\"");
                }
            };
            checkFont(n.textStyle);
            for (const auto& run : n.textRuns) checkFont(run.style);

            // text-overflow: re-measure with the exact wrap pass rendering
            // uses and compare against the box. Authored truncation
            // (TRUNCATE / textTruncation: ENDING) clips by design — skip it.
            const TextStyle& ts = n.textStyle;
            if (ts.autoResize != "TRUNCATE" && !ts.truncateEnding) {
                const bool noWrap = ts.autoResize == "WIDTH_AND_HEIGHT" || n.width <= 0;
                float tw = 0, th = 0;
                if (renderer.measureText(n, noWrap ? 0.0f : n.width, tw, th)) {
                    // A few px of line-box overhang is routine (actual font
                    // metrics vs the authored box) and TEXT nodes don't
                    // self-clip; only flag when a meaningful slice of a line
                    // falls outside the box.
                    const float lineH =
                        ts.lineHeightPx > 0 ? ts.lineHeightPx : ts.fontSize * 1.2f;
                    const float slack = std::max(kTol, 0.3f * lineH);
                    if (th > n.height + slack) {
                        add("text-overflow", n,
                            "text needs " + px(th) + "px of height but the box is " +
                                px(n.height) + "px -- bottom line(s) are cut off");
                    } else if (tw > n.width + std::max(kTol, 0.3f * ts.fontSize)) {
                        add("text-overflow", n,
                            "text line is " + px(tw) + "px wide but the box is " +
                                px(n.width) + "px -- text runs past the box");
                    }
                }
            }
        }

        // node-overflow: visible children sticking out of a clipsContent
        // parent get their pixels cut. A scrolling frame overflows its scroll
        // axes by design, so those axes are exempt.
        if (n.clipsContent && n.width > 0 && n.height > 0) {
            const bool skipX = n.scrollDirection == ScrollDirection::Horizontal ||
                               n.scrollDirection == ScrollDirection::Both;
            const bool skipY = n.scrollDirection == ScrollDirection::Vertical ||
                               n.scrollDirection == ScrollDirection::Both;
            for (const auto& childPtr : n.children) {
                Node& c = *childPtr;
                if (!c.effectivelyVisible() || (c.width <= 0 && c.height <= 0)) continue;
                // Child bounding box in parent-local coordinates.
                float minX = 0, minY = 0, maxX = 0, maxY = 0;
                const float corners[4][2] = {
                    {0, 0}, {c.width, 0}, {0, c.height}, {c.width, c.height}};
                for (int i = 0; i < 4; ++i) {
                    float x, y;
                    c.relativeTransform.apply(corners[i][0], corners[i][1], x, y);
                    minX = i == 0 ? x : std::min(minX, x);
                    maxX = i == 0 ? x : std::max(maxX, x);
                    minY = i == 0 ? y : std::min(minY, y);
                    maxY = i == 0 ? y : std::max(maxY, y);
                }
                std::string parts;
                auto over = [&](float amount, const char* edge) {
                    if (amount <= kTol) return;
                    if (!parts.empty()) parts += ", ";
                    parts += px(amount) + "px past the " + edge + " edge";
                };
                if (!skipX) {
                    over(-minX, "left");
                    over(maxX - n.width, "right");
                }
                if (!skipY) {
                    over(-minY, "top");
                    over(maxY - n.height, "bottom");
                }
                if (!parts.empty()) {
                    add("node-overflow", c,
                        "extends " + parts + " of clipping parent \"" + n.name +
                            "\" -- the overhang is clipped away");
                }
            }
        }

        for (const auto& child : n.children) walk(*child);
    };
    walk(*frame);
    return out;
}

namespace {

// "State=Hover, Size=Large" → {("state","hover"), ("size","large")}.
// Keys and values compare case-insensitively, whitespace-trimmed.
std::map<std::string, std::string> parseVariantName(const std::string& name) {
    std::map<std::string, std::string> props;
    auto normalize = [](std::string s) {
        const auto b = s.find_first_not_of(" \t");
        const auto e = s.find_last_not_of(" \t");
        s = b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    size_t pos = 0;
    while (pos <= name.size()) {
        const size_t comma = std::min(name.find(',', pos), name.size());
        const std::string part = name.substr(pos, comma - pos);
        if (const size_t eq = part.find('='); eq != std::string::npos) {
            props[normalize(part.substr(0, eq))] = normalize(part.substr(eq + 1));
        }
        pos = comma + 1;
    }
    return props;
}

}  // namespace

bool FigmaUI::setVariant(const std::string& instanceName, const std::string& property,
                         const std::string& value, float durationSec) {
    Node* inst = impl_->findMutable(instanceName);
    if (!inst || inst->componentId.empty()) return false;
    impl_->stopScrollAnims();  // the instance subtree is about to be replaced
    Document& doc = *impl_->doc;

    Node* current = doc.findById(inst->componentId);
    if (!current || !current->parent ||
        current->parent->type != NodeType::ComponentSet) {
        return false;
    }
    Node* set = current->parent;

    // Desired property map: the current variant with one property replaced.
    auto want = parseVariantName(current->name);
    auto target_props = parseVariantName(property + "=" + value);
    for (auto& kv : target_props) want[kv.first] = kv.second;

    Node* target = nullptr;
    for (auto& cand : set->children) {
        if (cand->type != NodeType::Component) continue;
        if (parseVariantName(cand->name) == want) {
            target = cand.get();
            break;
        }
    }
    if (!target) return false;
    if (target == current) return true;  // already in that state

    // Swap in clones of the target variant's children, then reflow them from
    // the component's authored size to this instance's authored size so the
    // new subtree behaves exactly like a parse-time instance.
    impl_->setFocus(nullptr);  // the focused node may live in the old subtree
    impl_->structureRev++;     // the old variant subtree is about to be freed
    impl_->pendingScroll.clear();  // queued Node*s may point into it
    inst->children.clear();
    for (const auto& c : target->children) inst->children.push_back(cloneNode(*c, inst));
    inst->componentId = target->id;

    const float instBaseW = inst->baseWidth, instBaseH = inst->baseHeight;
    inst->baseWidth = target->baseWidth;
    inst->baseHeight = target->baseHeight;
    layoutFrame(*inst, instBaseW > 0 ? instBaseW : target->baseWidth,
                instBaseH > 0 ? instBaseH : target->baseHeight);
    inst->baseWidth = instBaseW;
    inst->baseHeight = instBaseH;
    for (auto& c : inst->children) {
        c->visit([](Node& n) {
            n.baseTransform = n.relativeTransform;
            n.baseWidth = n.width;
            n.baseHeight = n.height;
            return true;
        });
    }

    impl_->reflow();  // re-run viewport reflow when in Reflow mode
    impl_->hovered = nullptr;
    impl_->pressed = nullptr;
    impl_->renderer.markDirty();

    // The clones carry the MASTER's cached absolute transforms (canvas
    // space); hit-testing must work before the next render re-caches them
    // (autoStates re-resolves the in-flight press right after this swap).
    {
        std::function<void(Node&)> recache = [&](Node& n) {
            for (auto& c : n.children) {
                c->absoluteTransform = n.absoluteTransform * c->relativeTransform;
                recache(*c);
            }
        };
        recache(*inst);
    }

    // Dissolve (v1): the new subtree fades in from transparent over the
    // duration; update(dt) steps it and restores the authored opacity.
    // (The full-frame transition snapshot channel is per-navigation and
    // backend-composited — no per-node reuse — so no old-subtree snapshot.)
    if (durationSec > 0) {
        inst->runtimeOpacity = 0.0f;
        impl_->variantFades[instanceName] = {0.0f, durationSec};
    } else {
        impl_->variantFades.erase(instanceName);  // instant switch cancels a fade
        inst->runtimeOpacity = -1.0f;
    }

    // Sliders bound inside the swapped subtree: re-place knob/fill so the
    // fresh clone shows the current value, not the authored one.
    for (auto& [name, opts] : impl_->sliders) {
        if (Node* track = inst->findByName(name)) {
            impl_->applySliderVisuals(track, opts);
        }
    }
    return true;
}

// Deferred autoStates switches: applied strictly outside handler dispatch.
// setVariant nulls hovered/pressed (their subtree was freed), which would
// break the in-flight gesture — so both are re-resolved at the last pointer
// position afterwards and the press keeps clicking on release.
void FigmaUI::Impl::flushAutoVariants() {
    if (pendingAutoVariant.empty()) return;
    std::vector<PendingVariant> batch;
    batch.swap(pendingAutoVariant);  // applying may queue again (never loops:
                                     // queueAutoVariant only runs in pointer code)
    const bool hadPressed = pressed != nullptr;
    const bool hadHovered = hovered != nullptr;
    const uint64_t rev = structureRev;
    for (const PendingVariant& p : batch) {
        // Fails soft when the set lacks the variant (e.g. no Pressed state).
        self->setVariant(p.name, p.prop, p.value, kAutoStateFadeSec);
    }
    if (structureRev == rev) return;  // nothing actually swapped
    if (hadPressed) pressed = hitTestViewport(lastPointerX, lastPointerY);
    if (hadHovered) hovered = hitTestViewport(lastPointerX, lastPointerY);
}

bool FigmaUI::bindSlider(const std::string& trackName, SliderOptions opts) {
    opts.value = Impl::snapSliderValue(opts, opts.value);
    auto& slot = impl_->sliders[trackName];
    slot = std::move(opts);
    Node* track = impl_->findMutable(trackName);
    if (track) impl_->applySliderVisuals(track, slot);
    return track != nullptr;
}

bool FigmaUI::setValue(const std::string& trackName, float value) {
    const auto it = impl_->sliders.find(trackName);
    if (it == impl_->sliders.end()) return false;
    it->second.value = Impl::snapSliderValue(it->second, value);
    if (Node* track = impl_->findMutable(trackName)) {
        impl_->applySliderVisuals(track, it->second);
    }
    return true;
}

bool FigmaUI::autoStates(const std::string& instanceName, const AutoStateMap& states) {
    Impl::AutoStateSpec spec;
    spec.hover = states.hover.empty() ? "State=Hover" : states.hover;
    spec.pressed = states.pressed.empty() ? "State=Pressed" : states.pressed;
    spec.base = states.base.empty() ? "State=Default" : states.base;
    impl_->autoStateSpecs[instanceName] = std::move(spec);
    Node* n = impl_->findMutable(instanceName);
    return n && !n->componentId.empty();
}

}  // namespace figo
