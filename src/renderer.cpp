#include "figmalib/renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

#include <thorvg.h>

#include "figmalib/layout.h"
#include "font_provider.h"
#include "scene_builder.h"

namespace figmalib {

namespace {

// ThorVG engine init is global; refcount across Renderer instances.
std::atomic<int> g_engineRefs{0};

void engineRef() {
    if (g_engineRefs.fetch_add(1) == 0) tvg::Initializer::init(0 /*all threads*/);
}

void engineUnref() {
    if (g_engineRefs.fetch_sub(1) == 1) tvg::Initializer::term();
}

// Owner of the layout engine's global text measurer (last renderer wins).
Renderer* g_measurerOwner = nullptr;

}  // namespace

struct Renderer::Impl {
    std::unique_ptr<tvg::Canvas> canvas;  // SwCanvas (CPU) or GlCanvas (GPU)
    bool gpu = false;
    int32_t fboId = 0;
    std::vector<uint32_t> buffer;
    uint32_t width = 0, height = 0;
    Node* frame = nullptr;
    bool dirty = true;        // document/scene changed → rebuild the tvg scene
    bool viewDirty = true;    // only the view transform changed → re-raster only
    bool scrollDirty = false; // only scroll offsets changed → retarget + re-raster
    bool targetValid = false;
    FontProvider fonts;
    std::string imageDir;
    Mat23 viewTransform;
    bool hasViewTransform = false;
    // Persistent scene: rebuilding thousands of tvg objects (path parsing,
    // text layout) dwarfs rasterization cost, so the scene survives view
    // changes and is only rebuilt when the document changes.
    tvg::Scene* rootScene = nullptr;  // owned by the canvas once added
    // Scrolled sub-scenes registered during the last build, plus the scroll
    // offsets they were built with (to delta-update hit-test transforms).
    std::vector<ScrollBinding> scrollBindings;
    std::unordered_map<Node*, std::pair<float, float>> appliedScroll;

    // Retarget every scrolled sub-scene at the current offsets and shift the
    // cached absolute (hit-test) transforms by the world-space delta. Pure
    // transform updates — no tvg object is created or destroyed.
    void syncScroll() {
        for (auto& b : scrollBindings) {
            Mat23 shift;
            shift.m02 = -b.scroller->scrollX;
            shift.m12 = -b.scroller->scrollY;
            const Mat23 local = shift * b.baseLocal;
            const tvg::Matrix m{local.m00, local.m01, local.m02,
                                local.m10, local.m11, local.m12, 0, 0, 1};
            b.scene->transform(m);
            if (b.clip) b.clip->transform(m);
        }
        for (auto& [scroller, applied] : appliedScroll) {
            const float dx = applied.first - scroller->scrollX;
            const float dy = applied.second - scroller->scrollY;
            if (dx == 0 && dy == 0) continue;
            // Translation in scroller-local space → world space via the
            // linear part of its absolute transform (scale/rotation only).
            const Mat23& a = scroller->absoluteTransform;
            const float wx = a.m00 * dx + a.m01 * dy;
            const float wy = a.m10 * dx + a.m11 * dy;
            for (auto& b : scrollBindings) {
                if (b.scroller != scroller) continue;
                b.child->visit([&](Node& n) {
                    n.absoluteTransform.m02 += wx;
                    n.absoluteTransform.m12 += wy;
                    return true;
                });
            }
            applied = {scroller->scrollX, scroller->scrollY};
        }
    }

    Mat23 contentTransform() const {
        if (hasViewTransform) return viewTransform;
        Mat23 m;
        if (!frame || frame->width <= 0 || frame->height <= 0 || width == 0 || height == 0) {
            return m;
        }
        const float s = std::min(static_cast<float>(width) / frame->width,
                                 static_cast<float>(height) / frame->height);
        m.m00 = m.m11 = s;
        m.m02 = (static_cast<float>(width) - frame->width * s) * 0.5f;
        m.m12 = (static_cast<float>(height) - frame->height * s) * 0.5f;
        return m;
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {
    engineRef();
    impl_->canvas.reset(tvg::SwCanvas::gen());
    g_measurerOwner = this;
    setTextMeasurer([this](const Node& n, float maxW, float& w, float& h) {
        return measureText(n, maxW, w, h);
    });
}

Renderer::~Renderer() {
    if (g_measurerOwner == this) {
        g_measurerOwner = nullptr;
        setTextMeasurer(nullptr);
    }
    impl_->canvas.reset();
    engineUnref();
}

bool Renderer::measureText(const Node& node, float maxWidth, float& outWidth,
                           float& outHeight) {
    BuildContext ctx;
    ctx.fonts = &impl_->fonts;
    ctx.imageDir = impl_->imageDir;
    return measureTextNode(node, maxWidth, ctx, outWidth, outHeight);
}

int Renderer::textByteAtPoint(const Node& node, float x, float y) {
    BuildContext ctx;
    ctx.fonts = &impl_->fonts;
    ctx.imageDir = impl_->imageDir;
    return textByteFromPoint(node, ctx, x, y);
}

bool Renderer::setTarget(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (impl_->gpu) {  // switch back to CPU: the scene dies with the canvas
        impl_->canvas.reset(tvg::SwCanvas::gen());
        impl_->rootScene = nullptr;
        impl_->gpu = false;
        impl_->targetValid = false;
        impl_->dirty = true;
    }
    if (!impl_->canvas) return false;
    if (width == impl_->width && height == impl_->height && impl_->targetValid) return true;

    impl_->buffer.assign(static_cast<size_t>(width) * height, 0);
    impl_->width = width;
    impl_->height = height;
    // RGBA8888 straight alpha — matches common engine texture formats.
    const auto res = static_cast<tvg::SwCanvas*>(impl_->canvas.get())
                         ->target(impl_->buffer.data(), width, width, height,
                                  tvg::ColorSpace::ABGR8888S);
    impl_->targetValid = res == tvg::Result::Success;
    impl_->viewDirty = true;  // scene survives; only the raster target changed
    return impl_->targetValid;
}

bool Renderer::setTargetGL(int32_t fboId, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (!impl_->gpu) {
        auto* gl = tvg::GlCanvas::gen();
        if (!gl) return false;  // ThorVG built without the GL engine
        impl_->canvas.reset(gl);
        impl_->rootScene = nullptr;
        impl_->gpu = true;
        impl_->targetValid = false;
        impl_->dirty = true;
    } else if (width == impl_->width && height == impl_->height &&
               fboId == impl_->fboId && impl_->targetValid) {
        return true;
    }
    impl_->buffer.clear();
    impl_->width = width;
    impl_->height = height;
    impl_->fboId = fboId;
    // No display/surface/context: the caller's GL context is already current.
    const auto res = static_cast<tvg::GlCanvas*>(impl_->canvas.get())
                         ->target(nullptr, nullptr, nullptr, fboId, width, height,
                                  tvg::ColorSpace::ABGR8888S);
    impl_->targetValid = res == tvg::Result::Success;
    impl_->viewDirty = true;
    return impl_->targetValid;
}

bool Renderer::isGpu() const { return impl_->gpu; }

void Renderer::setFrame(Node* frame) {
    if (impl_->frame == frame) return;
    impl_->frame = frame;
    impl_->dirty = true;
}

void Renderer::markDirty() { impl_->dirty = true; }

void Renderer::markScrollDirty() { impl_->scrollDirty = true; }

void Renderer::setViewTransform(const Mat23& view) {
    const Mat23& cur = impl_->viewTransform;
    if (impl_->hasViewTransform && cur.m00 == view.m00 && cur.m01 == view.m01 &&
        cur.m02 == view.m02 && cur.m10 == view.m10 && cur.m11 == view.m11 &&
        cur.m12 == view.m12) {
        return;  // unchanged
    }
    // Re-origin behavior differs between modes, so the scene must be rebuilt
    // when first entering view mode; afterwards only the transform updates.
    if (!impl_->hasViewTransform) impl_->dirty = true;
    impl_->viewTransform = view;
    impl_->hasViewTransform = true;
    impl_->viewDirty = true;
}

void Renderer::clearViewTransform() {
    if (!impl_->hasViewTransform) return;
    impl_->hasViewTransform = false;
    impl_->dirty = true;
}

bool Renderer::render() {
    auto& d = *impl_;
    if ((!d.dirty && !d.viewDirty && !d.scrollDirty) || !d.frame || !d.targetValid ||
        !d.canvas) {
        return false;
    }

    if (!d.rootScene) {
        d.rootScene = tvg::Scene::gen();
        d.canvas->add(d.rootScene);
    }

    if (d.dirty) {  // document changed → rebuild the scene graph
        d.rootScene->remove();
        d.scrollBindings.clear();
        d.appliedScroll.clear();
        BuildContext ctx;
        ctx.fonts = &d.fonts;
        ctx.imageDir = d.imageDir;
        ctx.scrollBindings = &d.scrollBindings;
        // In view-transform (editor) mode the frame keeps its canvas position
        // so sibling frames of a page stay laid out; in fit mode it is
        // re-origined.
        const bool reorigin = !d.hasViewTransform;
        if (auto* frameScene = buildNodeScene(*d.frame, Mat23::identity(), ctx, reorigin)) {
            d.rootScene->add(frameScene);
        }
        for (const auto& b : d.scrollBindings) {
            d.appliedScroll[b.scroller] = {b.scroller->scrollX, b.scroller->scrollY};
        }
    } else if (d.scrollDirty) {  // offsets only → retarget the scrolled scenes
        d.syncScroll();
    }

    const Mat23 content = d.contentTransform();
    d.rootScene->transform({content.m00, content.m01, content.m02, content.m10, content.m11,
                            content.m12, 0, 0, 1});
    d.canvas->update();
    d.canvas->draw(true /*clear*/);
    d.canvas->sync();
    d.dirty = false;
    d.viewDirty = false;
    d.scrollDirty = false;
    return true;
}

bool Renderer::renderOverlay(const std::vector<Node*>& nodes, float yTopPx,
                             std::vector<uint32_t>& out, uint32_t& outWidth,
                             uint32_t& outHeight) {
    auto& d = *impl_;
    if (nodes.empty() || d.width == 0 || d.height == 0) return false;
    const auto top = static_cast<uint32_t>(
        std::clamp(std::floor(yTopPx), 0.0f, static_cast<float>(d.height)));
    const uint32_t h = d.height - top;
    if (h == 0) return false;

    std::unique_ptr<tvg::Canvas> canvas(tvg::SwCanvas::gen());
    if (!canvas) return false;
    out.assign(static_cast<size_t>(d.width) * h, 0);
    if (static_cast<tvg::SwCanvas*>(canvas.get())
            ->target(out.data(), d.width, d.width, h, tvg::ColorSpace::ABGR8888S) !=
        tvg::Result::Success) {
        return false;
    }

    auto* root = tvg::Scene::gen();
    canvas->add(root);
    BuildContext ctx;
    ctx.fonts = &d.fonts;
    ctx.imageDir = d.imageDir;
    std::vector<ScrollBinding> bindings;  // throwaway: the overlay never scrolls
    ctx.scrollBindings = &bindings;
    for (Node* n : nodes) {
        // Not isRoot: keep each node's frame position. The absoluteTransform
        // side effect matches the main scene (the frame renders at origin).
        if (auto* s = buildNodeScene(*n, Mat23::identity(), ctx)) root->add(s);
    }
    const Mat23 m = d.contentTransform();
    root->transform({m.m00, m.m01, m.m02, m.m10, m.m11,
                     m.m12 - static_cast<float>(top), 0, 0, 1});
    canvas->update();
    if (canvas->draw(true) != tvg::Result::Success) return false;
    canvas->sync();
    outWidth = d.width;
    outHeight = h;
    return true;
}

const uint32_t* Renderer::pixels() const {
    return impl_->buffer.empty() ? nullptr : impl_->buffer.data();
}
uint32_t Renderer::width() const { return impl_->width; }
uint32_t Renderer::height() const { return impl_->height; }
Mat23 Renderer::contentTransform() const { return impl_->contentTransform(); }

void Renderer::registerFont(const std::string& family, const std::string& ttfPath,
                            int weight, bool italic) {
    impl_->fonts.registerFont(family, ttfPath, weight, italic);
}

int Renderer::registerFontsFromDirectory(const std::string& dir) {
    const int n = impl_->fonts.registerFontsFromDirectory(dir);
    if (n > 0) impl_->dirty = true;
    return n;
}

void Renderer::setDefaultFontFamily(const std::string& family) {
    impl_->fonts.setDefaultFamily(family);
}

void Renderer::setImageDirectory(const std::string& dir) {
    impl_->imageDir = dir;
    impl_->dirty = true;
}

}  // namespace figmalib
