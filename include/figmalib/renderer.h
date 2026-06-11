#pragma once
// figmalib — engine-agnostic renderer.
//
// Rasterizes a Figma frame into an RGBA8888 (straight-alpha) pixel buffer via
// ThorVG's software canvas. Game-engine backends upload this buffer as a
// texture; nothing here depends on any particular engine.

#include <cstdint>
#include <memory>
#include <string>

#include "document.h"

namespace figmalib {

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // (Re)allocate the output buffer. Safe to call on window resize.
    bool setTarget(uint32_t width, uint32_t height);

    // GPU mode: rasterize with ThorVG's GL engine straight into the given
    // framebuffer object (0 = main surface) — zero CPU pixel copy. The GL
    // context must be current on the calling thread for target/render calls.
    // Returns false when ThorVG was built without the GL engine; the caller
    // should fall back to setTarget(). pixels() is null in GPU mode.
    bool setTargetGL(int32_t fboId, uint32_t width, uint32_t height);
    bool isGpu() const;

    // Frame to render. The frame is drawn at origin (its canvas position is
    // ignored) and uniformly scaled to fit the target, centered (letterboxed).
    void setFrame(Node* frame);

    // Editor mode: render with an explicit world→pixel view transform instead
    // of the letterbox fit. The frame keeps its own canvas position, so a
    // whole CANVAS page (with frames at their canvas coordinates) can be
    // rendered and panned/zoomed freely. Call clearViewTransform() to return
    // to fit mode.
    void setViewTransform(const Mat23& view);
    void clearViewTransform();

    // Re-rasterize if dirty. Returns true if the pixel buffer changed.
    bool render();
    void markDirty();

    // Pixel access — RGBA8888, straight (non-premultiplied) alpha, row-major,
    // stride == width. Valid after the first successful render().
    const uint32_t* pixels() const;
    uint32_t width() const;
    uint32_t height() const;

    // Maps frame-local coordinates to target pixels (scale-to-fit transform).
    Mat23 contentTransform() const;

    // Measures a TEXT node with the renderer's fonts and the exact wrap pass
    // used for drawing: wrapped at maxWidth (<= 0 → no wrapping). Outputs the
    // widest line and total line-box height; false when fonts are missing.
    // Each constructed Renderer also installs itself as the layout engine's
    // text measurer (see layout.h setTextMeasurer).
    bool measureText(const Node& node, float maxWidth, float& outWidth,
                     float& outHeight);

    // Fonts: explicit registration wins over system lookup (Windows registry).
    void registerFont(const std::string& family, const std::string& ttfPath,
                      int weight = 400, bool italic = false);
    // Registers every .ttf/.otf in a directory under the family/weight/italic
    // read from the font file itself. Returns the number registered.
    int registerFontsFromDirectory(const std::string& dir);
    void setDefaultFontFamily(const std::string& family);

    // Directory containing images referenced by IMAGE fills (<imageRef>.png/.jpg).
    void setImageDirectory(const std::string& dir);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace figmalib
