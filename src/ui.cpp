#include "figmalib/ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <unordered_map>

#include "figmalib/layout.h"
#include "figmalib/parser.h"

namespace figmalib {

struct FigmaUI::Impl {
    std::unique_ptr<Document> doc;
    Renderer renderer;
    Node* frame = nullptr;
    Node* hovered = nullptr;
    Node* pressed = nullptr;
    Node* focused = nullptr;  // editable TEXT node owning the caret
    ResizeMode resizeMode = ResizeMode::Scale;

    // Touch-style drag scrolling: candidate picked at pointerDown, panning
    // starts once the pointer travels past a small threshold; the release is
    // then swallowed instead of clicking.
    Node* dragScrollNode = nullptr;
    bool dragScrolling = false;
    float dragAccumX = 0, dragAccumY = 0;
    float lastPointerX = 0, lastPointerY = 0;
    static constexpr float kDragScrollThreshold = 6.0f;  // viewport px
    std::unordered_map<std::string, std::vector<ClickHandler>> clickHandlers;
    std::unordered_map<std::string, std::vector<HoverHandler>> hoverHandlers;

    // In Reflow mode the current frame tracks the viewport size.
    void reflow() {
        if (resizeMode != ResizeMode::Reflow || !frame) return;
        const uint32_t w = renderer.width(), h = renderer.height();
        if (w == 0 || h == 0) return;
        layoutFrame(*frame, static_cast<float>(w), static_cast<float>(h));
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
    template <typename Map, typename Fn>
    void fireUp(Map& handlers, Node* node, Fn&& invoke) {
        for (Node* n = node; n; n = n->parent) {
            if (auto it = handlers.find(n->name); it != handlers.end()) {
                for (auto& h : it->second) invoke(h, *n);
            }
            if (n == frame) break;
        }
    }

    Node* findMutable(const std::string& name) {
        Node* base = frame ? frame : (doc ? doc->root.get() : nullptr);
        return base ? base->findByName(name) : nullptr;
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
    static bool applyScroll(Node& n, float dx, float dy) {
        const float nx = std::clamp(n.scrollX + dx, 0.0f, n.maxScrollX());
        const float ny = std::clamp(n.scrollY + dy, 0.0f, n.maxScrollY());
        if (nx == n.scrollX && ny == n.scrollY) return false;
        n.scrollX = nx;
        n.scrollY = ny;
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
    // when the inner frame is already at its limit.
    bool scrollWithBubble(Node* start, float dx, float dy) {
        for (Node* n = start; n; n = n->parent) {
            if (n->scrolls() && applyScroll(*n, dx, dy)) {
                renderer.markDirty();
                return true;
            }
            if (n == frame) break;
        }
        return false;
    }

    // ---- Text editing ----

    void setFocus(Node* n) {
        if (focused == n) return;
        if (focused) focused->caretByte = -1;
        focused = n;
        if (focused) {
            focused->caretByte = static_cast<int>(focused->characters.size());
        }
        renderer.markDirty();
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
        if (focused) focused->textRuns.clear();  // runs index the old string
        reflow();  // auto-height boxes follow the new content
        renderer.markDirty();
    }
};

FigmaUI::FigmaUI() : impl_(std::make_unique<Impl>()) {}
FigmaUI::~FigmaUI() = default;

std::unique_ptr<FigmaUI> FigmaUI::fromFile(const std::string& path) {
    auto ui = std::unique_ptr<FigmaUI>(new FigmaUI());
    LoadedFile loaded = loadFigmaFile(path);  // .fig / canvas.json / REST JSON
    ui->impl_->doc = std::move(loaded.document);
    if (!loaded.imageDirectory.empty()) {
        ui->impl_->renderer.setImageDirectory(loaded.imageDirectory);
    }
    // Conventions for design fonts: a "fonts" directory next to the input
    // file, plus the FIGMALIB_FONTS_DIR environment variable.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path sibling = fs::path(path).parent_path() / "fonts";
        if (fs::is_directory(sibling, ec)) {
            ui->impl_->renderer.registerFontsFromDirectory(sibling.string());
        }
        if (const char* env = std::getenv("FIGMALIB_FONTS_DIR"); env && *env) {
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
    for (Node* f : impl_->doc->topLevelFrames()) {
        if (f->name == name || f->id == name) {
            impl_->setFocus(nullptr);
            impl_->frame = f;
            impl_->renderer.setFrame(f);
            impl_->hovered = nullptr;
            impl_->pressed = nullptr;
            impl_->reflow();
            return true;
        }
    }
    return false;
}

Node* FigmaUI::currentFrame() const { return impl_->frame; }

void FigmaUI::setResizeMode(ResizeMode mode) {
    if (impl_->resizeMode == mode) return;
    impl_->resizeMode = mode;
    if (mode == ResizeMode::Scale && impl_->frame) {
        resetLayout(*impl_->frame);  // back to the authored geometry
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
                impl_->scrollWithBubble(impl_->dragScrollNode,
                                        -impl_->dragAccumX * s, -impl_->dragAccumY * s);
            }
        } else {
            // Content follows the finger: dragging up reveals what is below.
            const float s = impl_->viewportToFrameScale();
            impl_->scrollWithBubble(impl_->dragScrollNode, -dx * s, -dy * s);
        }
        if (impl_->dragScrolling) return;  // no hover churn while panning
    }

    Node* hit = impl_->hitTestViewport(x, y);
    if (hit == impl_->hovered) return;
    if (impl_->hovered) {
        impl_->fireUp(impl_->hoverHandlers, impl_->hovered,
                      [](HoverHandler& h, Node& n) { h(n, false); });
    }
    impl_->hovered = hit;
    if (hit) {
        impl_->fireUp(impl_->hoverHandlers, hit,
                      [](HoverHandler& h, Node& n) { h(n, true); });
    }
}

void FigmaUI::pointerDown(float x, float y) {
    impl_->pressed = impl_->hitTestViewport(x, y);
    impl_->dragScrollNode = impl_->scrollableAtViewport(x, y);
    impl_->dragScrolling = false;
    impl_->dragAccumX = impl_->dragAccumY = 0;
    impl_->lastPointerX = x;
    impl_->lastPointerY = y;
}

void FigmaUI::pointerUp(float x, float y) {
    if (impl_->dragScrolling) {
        // The gesture was a pan, not a click.
        impl_->pressed = nullptr;
        impl_->dragScrollNode = nullptr;
        impl_->dragScrolling = false;
        return;
    }
    impl_->dragScrollNode = nullptr;

    Node* hit = impl_->hitTestViewport(x, y);

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
                          [](ClickHandler& h, Node& n) { h(n); });
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
    return impl_->scrollWithBubble(target, dx * s, dy * s);
}

bool FigmaUI::setScroll(const std::string& nodeName, float offsetX, float offsetY) {
    Node* n = impl_->findMutable(nodeName);
    if (!n || !n->scrolls()) return false;
    n->scrollX = std::clamp(offsetX, 0.0f, n->maxScrollX());
    n->scrollY = std::clamp(offsetY, 0.0f, n->maxScrollY());
    impl_->renderer.markDirty();
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
    const size_t len = n->characters.size();
    const size_t at = n->caretByte < 0
                          ? len
                          : std::min(static_cast<size_t>(n->caretByte), len);
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
    switch (key) {
    case EditKey::Left: caret = Impl::prevCp(s, caret); break;
    case EditKey::Right: caret = Impl::nextCp(s, caret); break;
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
        if (caret > 0) {
            const size_t b = Impl::prevCp(s, caret);
            s.erase(b, caret - b);
            caret = b;
            impl_->editChanged();
        }
        break;
    case EditKey::Delete:
        if (caret < s.size()) {
            s.erase(caret, Impl::nextCp(s, caret) - caret);
            impl_->editChanged();
        }
        break;
    }
    n->caretByte = static_cast<int>(caret);
    impl_->renderer.markDirty();
}

void FigmaUI::onClick(const std::string& nodeName, ClickHandler fn) {
    impl_->clickHandlers[nodeName].push_back(std::move(fn));
}

void FigmaUI::onHover(const std::string& nodeName, HoverHandler fn) {
    impl_->hoverHandlers[nodeName].push_back(std::move(fn));
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
    n->characters = text;
    // Rich-text runs index the old string; rendering falls back to the base
    // style, which is what a runtime-driven label wants.
    n->textRuns.clear();
    impl_->reflow();  // auto-height text can change the layout around it
    impl_->renderer.markDirty();
    return true;
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
                         const std::string& value) {
    Node* inst = impl_->findMutable(instanceName);
    if (!inst || inst->componentId.empty()) return false;
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
    return true;
}

}  // namespace figmalib
