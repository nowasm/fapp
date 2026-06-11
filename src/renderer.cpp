#include "figmalib/renderer.h"

#include <algorithm>
#include <atomic>
#include <vector>

#include <thorvg.h>

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

}  // namespace

struct Renderer::Impl {
    std::unique_ptr<tvg::SwCanvas> canvas;
    std::vector<uint32_t> buffer;
    uint32_t width = 0, height = 0;
    Node* frame = nullptr;
    bool dirty = true;       // document/scene changed → rebuild the tvg scene
    bool viewDirty = true;   // only the view transform changed → re-raster only
    bool targetValid = false;
    FontProvider fonts;
    std::string imageDir;
    Mat23 viewTransform;
    bool hasViewTransform = false;
    // Persistent scene: rebuilding thousands of tvg objects (path parsing,
    // text layout) dwarfs rasterization cost, so the scene survives view
    // changes and is only rebuilt when the document changes.
    tvg::Scene* rootScene = nullptr;  // owned by the canvas once added

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
}

Renderer::~Renderer() {
    impl_->canvas.reset();
    engineUnref();
}

bool Renderer::setTarget(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || !impl_->canvas) return false;
    if (width == impl_->width && height == impl_->height && impl_->targetValid) return true;

    impl_->buffer.assign(static_cast<size_t>(width) * height, 0);
    impl_->width = width;
    impl_->height = height;
    // RGBA8888 straight alpha — matches common engine texture formats.
    const auto res = impl_->canvas->target(impl_->buffer.data(), width, width, height,
                                           tvg::ColorSpace::ABGR8888S);
    impl_->targetValid = res == tvg::Result::Success;
    impl_->viewDirty = true;  // scene survives; only the raster target changed
    return impl_->targetValid;
}

void Renderer::setFrame(Node* frame) {
    if (impl_->frame == frame) return;
    impl_->frame = frame;
    impl_->dirty = true;
}

void Renderer::markDirty() { impl_->dirty = true; }

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
    if ((!d.dirty && !d.viewDirty) || !d.frame || !d.targetValid || !d.canvas) return false;

    if (!d.rootScene) {
        d.rootScene = tvg::Scene::gen();
        d.canvas->add(d.rootScene);
    }

    if (d.dirty) {  // document changed → rebuild the scene graph
        d.rootScene->remove();
        BuildContext ctx;
        ctx.fonts = &d.fonts;
        ctx.imageDir = d.imageDir;
        // In view-transform (editor) mode the frame keeps its canvas position
        // so sibling frames of a page stay laid out; in fit mode it is
        // re-origined.
        const bool reorigin = !d.hasViewTransform;
        if (auto* frameScene = buildNodeScene(*d.frame, Mat23::identity(), ctx, reorigin)) {
            d.rootScene->add(frameScene);
        }
    }

    const Mat23 content = d.contentTransform();
    d.rootScene->transform({content.m00, content.m01, content.m02, content.m10, content.m11,
                            content.m12, 0, 0, 1});
    d.canvas->update();
    d.canvas->draw(true /*clear*/);
    d.canvas->sync();
    d.dirty = false;
    d.viewDirty = false;
    return true;
}

const uint32_t* Renderer::pixels() const { return impl_->buffer.data(); }
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
