#include "figo_raylib.h"

#include <utility>

#include <rlgl.h>

namespace figo {

RaylibFigmaView::~RaylibFigmaView() {
    if (texture_.id != 0) UnloadTexture(texture_);
    if (rt_.id != 0) UnloadRenderTexture(rt_);
    dropPrev();
}

void RaylibFigmaView::dropPrev() {
    if (prevTexture_.id != 0) UnloadTexture(prevTexture_);
    prevTexture_ = {};
    if (rtPrev_.id != 0) UnloadRenderTexture(rtPrev_);
    rtPrev_ = {};
    prevValid_ = false;
    if (chromeTex_.id != 0) UnloadTexture(chromeTex_);
    chromeTex_ = {};
}

bool RaylibFigmaView::setGpu(bool enabled) {
    if (enabled == gpuWanted_) return gpuActive_;
    gpuWanted_ = enabled;
    // Drop both paths' resources; the next resize() rebuilds the active one.
    if (texture_.id != 0) {
        UnloadTexture(texture_);
        texture_ = {};
        textureValid_ = false;
    }
    if (rt_.id != 0) {
        UnloadRenderTexture(rt_);
        rt_ = {};
    }
    dropPrev();
    gpuActive_ = false;
    resize(GetScreenWidth(), GetScreenHeight());
    return gpuActive_;
}

void RaylibFigmaView::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    const auto w = static_cast<uint32_t>(width);
    const auto h = static_cast<uint32_t>(height);

    if (gpuWanted_) {
        if (gpuActive_ && rt_.id != 0 && w == ui_.pixelWidth() && h == ui_.pixelHeight()) return;
        if (rt_.id != 0) UnloadRenderTexture(rt_);
        dropPrev();  // snapshot no longer matches the target size
        rt_ = LoadRenderTexture(width, height);
        gpuActive_ = rt_.id != 0 &&
                     ui_.setViewportGL(static_cast<int32_t>(rt_.id), w, h);
        if (gpuActive_) return;
        // GL engine unavailable → CPU fallback below.
        if (rt_.id != 0) {
            UnloadRenderTexture(rt_);
            rt_ = {};
        }
        gpuWanted_ = false;
    }

    if (w == ui_.pixelWidth() && h == ui_.pixelHeight() && textureValid_) return;
    ui_.setViewport(w, h);
    if (texture_.id != 0) {
        UnloadTexture(texture_);
        texture_ = {};
        textureValid_ = false;
    }
    dropPrev();  // snapshot no longer matches the target size
}

void RaylibFigmaView::update() {
    ui_.update(GetFrameTime());  // frame transitions / animations
    const Vector2 mouse = GetMousePosition();
    ui_.pointerMove(mouse.x, mouse.y);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui_.pointerDown(mouse.x, mouse.y);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) ui_.pointerUp(mouse.x, mouse.y);
    const Vector2 wheel = GetMouseWheelMoveV();
    if (wheel.x != 0 || wheel.y != 0) {
        // One wheel notch ≈ 48 viewport px; wheel-up reveals content above.
        ui_.scrollBy(mouse.x, mouse.y, -wheel.x * 48.0f, -wheel.y * 48.0f);
    }

    // Text editing: while a node has focus, the keyboard belongs to it.
    // GetCharPressed also delivers committed IME strings (WM_CHAR), so CJK
    // input works without extra plumbing.
    if (ui_.focusedNode()) {
        using EK = FigmaUI::EditKey;
        // Clipboard chords glue the core's edit ops to the OS clipboard —
        // the core itself never touches it. Ctrl on Windows/Linux, plus Cmd
        // on macOS. AltGr = Ctrl+Alt on Windows layouts, so a chord only
        // counts without Alt (AltGr characters must still type below).
        const bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
        bool chord = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#if defined(__APPLE__)
        chord = chord || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#endif
        chord = chord && !alt;
        if (chord) {
            if (IsKeyPressed(KEY_A)) ui_.editSelectAll();
            if (IsKeyPressed(KEY_C)) {
                const std::string s = ui_.editCopy();  // "" on passwordMask
                if (!s.empty()) SetClipboardText(s.c_str());
            }
            if (IsKeyPressed(KEY_X)) {
                const std::string s = ui_.editCut();
                if (!s.empty()) SetClipboardText(s.c_str());
            }
            if (IsKeyPressed(KEY_V)) {
                const char* t = GetClipboardText();
                if (t && *t) ui_.editPaste(t);
            }
        }
        int cp;
        while ((cp = GetCharPressed()) != 0) {
            // Some hosts deliver Ctrl+letter as a control char (^C = 0x03);
            // textInput filters those, but drain the queue and never feed
            // chord keystrokes into the text.
            if (chord) continue;
            char buf[5] = {};
            if (cp < 0x80) {
                buf[0] = static_cast<char>(cp);
            } else if (cp < 0x800) {
                buf[0] = static_cast<char>(0xC0 | (cp >> 6));
                buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                buf[0] = static_cast<char>(0xE0 | (cp >> 12));
                buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                buf[0] = static_cast<char>(0xF0 | (cp >> 18));
                buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
            }
            ui_.textInput(buf);
        }
        auto rep = [](int k) { return IsKeyPressed(k) || IsKeyPressedRepeat(k); };
        if (rep(KEY_BACKSPACE)) ui_.editKey(EK::Backspace);
        if (rep(KEY_DELETE)) ui_.editKey(EK::Delete);
        if (rep(KEY_LEFT)) ui_.editKey(EK::Left);
        if (rep(KEY_RIGHT)) ui_.editKey(EK::Right);
        if (rep(KEY_HOME)) ui_.editKey(EK::Home);
        if (rep(KEY_END)) ui_.editKey(EK::End);
        if (rep(KEY_ENTER)) ui_.textInput("\n");
        if (IsKeyPressed(KEY_ESCAPE)) ui_.blur();
    }

    // A navigation just started: the live texture still shows the outgoing
    // frame, so swap it into the snapshot slot. The incoming frame then
    // rasterizes ONCE into a fresh target and draw() composites the two
    // textures per tick — no per-tick vector re-raster. With shared bottom
    // chrome the outgoing frame first re-rasterizes WITHOUT the chrome (it is
    // drawn as a separate static overlay, see chromeTex_).
    if (ui_.transitionId() != seenTransId_) {
        seenTransId_ = ui_.transitionId();
        if (chromeTex_.id != 0) {
            UnloadTexture(chromeTex_);
            chromeTex_ = {};
        }
        if (!ui_.animating()) {
            prevValid_ = false;  // hard cut — never composite a stale snapshot
        } else if (gpuActive_) {
            rlDrawRenderBatchActive();      // ThorVG GL renders the snapshot
            ui_.renderTransitionSnapshot();  // outgoing, chrome-free, into rt_
            std::swap(rt_, rtPrev_);
            prevValid_ = rtPrev_.id != 0;
            if (rt_.id == 0) {
                rt_ = LoadRenderTexture(static_cast<int>(ui_.pixelWidth()),
                                        static_cast<int>(ui_.pixelHeight()));
            }
            gpuActive_ = rt_.id != 0 &&
                         ui_.setViewportGL(static_cast<int32_t>(rt_.id),
                                           ui_.pixelWidth(), ui_.pixelHeight());
        } else {
            if (ui_.renderTransitionSnapshot() && textureValid_) {
                UpdateTexture(texture_, ui_.pixels());  // chrome-free outgoing
            }
            std::swap(texture_, prevTexture_);
            std::swap(textureValid_, prevValid_);
        }
        if (ui_.animating()) {
            uint32_t cw = 0, ch = 0;
            float cy = 0;
            if (const uint32_t* px = ui_.transitionChromePixels(cw, ch, cy)) {
                Image img{};
                img.data = const_cast<uint32_t*>(px);
                img.width = static_cast<int>(cw);
                img.height = static_cast<int>(ch);
                img.mipmaps = 1;
                img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                chromeTex_ = LoadTextureFromImage(img);
                chromeY_ = cy;
            }
        }
    }

    if (gpuActive_) {
        // ThorVG drives GL directly: flush raylib's pending batch first, then
        // restore the state rlgl assumes (it caches and won't re-apply).
        rlDrawRenderBatchActive();
        ui_.render();
        rlDisableFramebuffer();
        rlViewport(0, 0, GetRenderWidth(), GetRenderHeight());
        rlSetBlendMode(RL_BLEND_ADDITIVE);  // force a transition so ALPHA re-applies
        rlSetBlendMode(RL_BLEND_ALPHA);
        rlEnableColorBlend();
        rlDisableDepthTest();
        rlDisableScissorTest();
        return;
    }

    if (!ui_.render() && textureValid_) return;
    if (ui_.pixelWidth() == 0 || ui_.pixelHeight() == 0 || !ui_.pixels()) return;

    if (!textureValid_) {
        Image image{};
        image.data = const_cast<uint32_t*>(ui_.pixels());
        image.width = static_cast<int>(ui_.pixelWidth());
        image.height = static_cast<int>(ui_.pixelHeight());
        image.mipmaps = 1;
        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        texture_ = LoadTextureFromImage(image);  // copies the data to the GPU
        textureValid_ = texture_.id != 0;
    } else {
        UpdateTexture(texture_, ui_.pixels());
    }
}

void RaylibFigmaView::draw(int x, int y, ::Color tint) const {
    const bool curOk = gpuActive_ ? rt_.id != 0 : textureValid_;
    if (!curOk) return;
    const Texture2D& cur = gpuActive_ ? rt_.texture : texture_;

    // Source rect for the image-space band [top, top+bh). FBO color
    // attachments are bottom-up in GL: select the mirrored row range and draw
    // it flipped (negative height, the usual raylib RenderTexture convention).
    auto srcBand = [this](const Texture2D& t, float top, float bh) {
        return gpuActive_ ? Rectangle{0, static_cast<float>(t.height) - top - bh,
                                      static_cast<float>(t.width), -bh}
                          : Rectangle{0, top, static_cast<float>(t.width), bh};
    };
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    const float h = static_cast<float>(cur.height);

    if (!ui_.animating() || !prevValid_) {
        DrawTextureRec(cur, srcBand(cur, 0, h), {fx, fy}, tint);
        // No snapshot to composite, but the page raster is chrome-free while
        // a shared-chrome transition runs — keep the bar visible.
        if (ui_.animating() && chromeTex_.id != 0) {
            DrawTextureV(chromeTex_, {fx, fy + chromeY_}, tint);
        }
        return;
    }

    // Frame transition: composite the cached outgoing snapshot (behind) with
    // the incoming texture (on top) at the eased progress, all on the GPU.
    const Texture2D& prev = gpuActive_ ? rtPrev_.texture : prevTexture_;
    const float p = ui_.transitionProgress();
    const float w = static_cast<float>(cur.width);
    float inX = 0, inY = 0, outX = 0, outY = 0;
    ::Color inTint = tint;
    switch (ui_.transitionType()) {
    case FigmaUI::Transition::SlideLeft: inX = (1 - p) * w; outX = -p * w; break;
    case FigmaUI::Transition::SlideRight: inX = -(1 - p) * w; outX = p * w; break;
    case FigmaUI::Transition::SlideUp: inY = (1 - p) * h; outY = -p * h; break;
    case FigmaUI::Transition::SlideDown: inY = -(1 - p) * h; outY = p * h; break;
    default:  // Dissolve: incoming fades in over the outgoing frame
        inTint.a = static_cast<unsigned char>(tint.a * p);
        break;
    }

    // Shared bottom chrome (tab bar in both frames) stays put while the
    // pages slide. The page textures were rasterized WITHOUT the chrome, so
    // they slide full-height and the chrome overlay (straight alpha, drawn
    // from the incoming frame — its tab highlight switches immediately,
    // native behavior) composites statically on top. Anything overhanging
    // the bar and the moving page pixels around it stay correct.
    if (chromeTex_.id != 0) {
        DrawTextureRec(prev, srcBand(prev, 0, h), {fx + outX, fy + outY}, tint);
        DrawTextureRec(cur, srcBand(cur, 0, h), {fx + inX, fy + inY}, inTint);
        DrawTextureV(chromeTex_, {fx, fy + chromeY_}, tint);
        return;
    }

    // Fallback (overlay raster unavailable): pages slide cropped above the
    // band, the band renders statically from the incoming texture.
    const float bandY = std::min(ui_.transitionStaticBottomY(), h);
    DrawTextureRec(prev, srcBand(prev, 0, bandY), {fx + outX, fy + outY}, tint);
    DrawTextureRec(cur, srcBand(cur, 0, bandY), {fx + inX, fy + inY}, inTint);
    if (bandY < h - 0.5f) {
        DrawTextureRec(cur, srcBand(cur, bandY, h - bandY), {fx, fy + bandY}, tint);
    }
}

}  // namespace figo
