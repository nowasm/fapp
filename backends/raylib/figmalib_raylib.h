#pragma once
// figmalib — raylib backend.
//
// Bridges a FigmaUI to raylib: uploads the rendered RGBA buffer into a
// Texture2D and forwards mouse input. This file is the template for writing
// backends for other engines — the whole contract is "give me pixels, take
// my input events".

#include <raylib.h>

#include <figmalib/ui.h>

namespace figmalib {

class RaylibFigmaView {
public:
    // The view does not own the FigmaUI; keep it alive while the view exists.
    explicit RaylibFigmaView(FigmaUI& ui) : ui_(ui) {}
    ~RaylibFigmaView();

    RaylibFigmaView(const RaylibFigmaView&) = delete;
    RaylibFigmaView& operator=(const RaylibFigmaView&) = delete;

    // GPU mode: ThorVG's GL engine renders straight into a raylib
    // RenderTexture (zero CPU pixel copy). Falls back to CPU rasterization
    // automatically when the GL engine is unavailable. Returns the mode
    // actually active.
    bool setGpu(bool enabled);
    bool gpu() const { return gpuActive_; }

    // Match the UI raster size to the given size (e.g. GetScreenWidth/Height).
    void resize(int width, int height);

    // Call once per frame before drawing: feeds mouse input, re-renders if
    // dirty, and re-uploads the texture when pixels changed.
    void update();

    // Draw the UI texture. Call between BeginDrawing/EndDrawing. During a
    // frame transition this composites the cached outgoing-frame texture with
    // the incoming one on the GPU (the vector scene is not re-rasterized).
    // (::Color is raylib's — figmalib has its own Color type in this namespace.)
    void draw(int x = 0, int y = 0, ::Color tint = {255, 255, 255, 255}) const;

    const Texture2D& texture() const { return gpuActive_ ? rt_.texture : texture_; }

private:
    void dropPrev();  // release the transition snapshot (both paths)

    FigmaUI& ui_;
    Texture2D texture_{};
    bool textureValid_ = false;
    bool gpuWanted_ = false;
    bool gpuActive_ = false;
    RenderTexture2D rt_{};
    // Outgoing-frame snapshot for transition compositing; filled by swapping
    // with the live texture when a navigation starts (see update()).
    Texture2D prevTexture_{};
    RenderTexture2D rtPrev_{};
    bool prevValid_ = false;
    uint32_t seenTransId_ = 0;
};

}  // namespace figmalib
