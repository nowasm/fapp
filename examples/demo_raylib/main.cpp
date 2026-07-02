// figo demo: a Figma file rendered as the UI of a raylib "game".
//
// The animated starfield is the game scene; everything else on screen comes
// from examples/assets/sample_ui.json (Figma REST API format).

#include <cmath>
#include <string>

#include <raylib.h>

#include <figo/figo.h>
#include <figo_raylib.h>

#ifndef ASSETS_DIR
#define ASSETS_DIR "."
#endif

int main(int argc, char** argv) {
    // Flags: --gpu (ThorVG GL engine, zero-copy), --screenshot <file> (render
    // ~30 frames, save a screenshot, exit — used for automated verification).
    // First non-flag argument: .fig file, fig2json canvas.json, or REST JSON.
    std::string input = ASSETS_DIR "/sample_ui.json";
    bool wantGpu = false;
    const char* shotPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--gpu") wantGpu = true;
        else if (arg == "--screenshot" && i + 1 < argc) shotPath = argv[++i];
        else input = arg;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(900, 640, "figo - Figma UI in raylib");

    auto ui = figo::FigmaUI::fromFile(input);

    // Hover feedback: dim any button the pointer is over.
    for (const char* name : {"btn-start", "btn-options", "btn-quit", "btn-back"}) {
        ui->onHover(name, [&ui](figo::Node& n, bool entered, float, float) {
            ui->setOpacity(n.name, entered ? 0.8f : -1.0f);  // <0 restores authored
        });
    }

    bool quit = false;
    int runs = 0;
    ui->onClick("btn-start", [&](figo::Node&, float, float) {
        ++runs;
        ui->setText("subtitle", "Game started! (run #" + std::to_string(runs) + ")");
    });
    ui->onClick("btn-options", [&](figo::Node&, float, float) { ui->selectFrame("Settings"); });
    ui->onClick("btn-back", [&](figo::Node&, float, float) { ui->selectFrame("MainMenu"); });
    ui->onClick("btn-quit", [&](figo::Node&, float, float) { quit = true; });

    figo::RaylibFigmaView view(*ui);
    if (wantGpu) {
        view.resize(GetScreenWidth(), GetScreenHeight());  // create the GL context targets
        const bool ok = view.setGpu(true);
        SetWindowTitle(ok ? "figo - Figma UI in raylib [GPU]"
                          : "figo - Figma UI in raylib (GL engine unavailable)");
    }

    // Left/right arrows page through frames (useful for multi-screen .fig files).
    const auto frames = ui->frameNames();
    int frameIdx = 0;
    int frameCount = 0;

    while (!WindowShouldClose() && !quit) {
        if (!frames.empty() && (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_LEFT))) {
            const int dir = IsKeyPressed(KEY_RIGHT) ? 1 : -1;
            frameIdx = (frameIdx + dir + static_cast<int>(frames.size())) %
                       static_cast<int>(frames.size());
            ui->selectFrame(frames[frameIdx]);
        }
        // G toggles GPU (ThorVG GL engine) vs CPU rasterization.
        if (IsKeyPressed(KEY_G)) {
            const bool on = view.setGpu(!view.gpu());
            SetWindowTitle(on ? "figo - Figma UI in raylib [GPU]"
                              : "figo - Figma UI in raylib");
        }
        // R toggles responsive reflow (constraints/auto-layout) vs scale-to-fit.
        if (IsKeyPressed(KEY_R)) {
            const bool reflow = ui->resizeMode() == figo::FigmaUI::ResizeMode::Reflow;
            ui->setResizeMode(reflow ? figo::FigmaUI::ResizeMode::Scale
                                     : figo::FigmaUI::ResizeMode::Reflow);
            SetWindowTitle(reflow ? "figo - Figma UI in raylib"
                                  : "figo - Figma UI in raylib [reflow]");
        }
        view.resize(GetScreenWidth(), GetScreenHeight());
        view.update();

        BeginDrawing();
        ClearBackground(Color{10, 8, 20, 255});

        // "Game" layer: drifting stars behind the (semi-transparent) Figma UI.
        const float t = static_cast<float>(GetTime());
        const int w = GetScreenWidth(), h = GetScreenHeight();
        for (int i = 0; i < 64; ++i) {
            const float speed = 12.0f + (i % 7) * 9.0f;
            const float x = std::fmod(i * 137.0f + t * speed, static_cast<float>(w + 40)) - 20;
            const float y = std::fmod(i * 251.0f, static_cast<float>(h));
            const float r = 1.0f + (i % 3);
            const unsigned char a = static_cast<unsigned char>(90 + (i * 53) % 120);
            DrawCircleV({x, y}, r, Color{200, 210, 255, a});
        }

        view.draw();
        DrawFPS(8, 8);
        EndDrawing();

        if (shotPath && ++frameCount >= 30) {
            TakeScreenshot(shotPath);
            quit = true;
        }
    }

    CloseWindow();
    return 0;
}
