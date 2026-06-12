// figmaplay: the generic script player — an app is <design.fig> + <logic.js>.
//
//   figmaplay [design.fig] [logic.js] [--selfdrive prefix]
//
// All behavior lives in the script (see figmalib/script.h for the JS API);
// this host only loads the two files and runs the frame loop. With no
// arguments it plays the wallet demo (examples/demo_script/wallet.js).
// --selfdrive defines globalThis.SELFDRIVE for the script (which drives its
// own tour, e.g. tapping into a coin) and saves <prefix>_home/nav.png.

#include <cstdio>
#include <string>

#include <raylib.h>

#include <figmalib/figmalib.h>
#include <figmalib/script.h>
#include <figmalib_raylib.h>

int main(int argc, char** argv) {
    std::string design, script;
    const char* drivePrefix = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--selfdrive" && i + 1 < argc) drivePrefix = argv[++i];
        else if (arg.size() > 3 && arg.compare(arg.size() - 3, 3, ".js") == 0) script = arg;
        else design = arg;
    }
    if (design.empty()) {
        for (const char* cand : {"wallet.fig", "../wallet.fig",
                                 "D:/work_open/fig2psd/test/figma/wallet.fig"}) {
            if (FILE* f = fopen(cand, "rb")) {
                fclose(f);
                design = cand;
                break;
            }
        }
    }
    if (script.empty()) script = std::string(EXAMPLES_DIR) + "/demo_script/wallet.js";
    if (design.empty()) {
        std::printf("usage: figmaplay [design.fig] [logic.js] [--selfdrive prefix]\n");
        return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(420, 900, "figmaplay — design.fig + logic.js");

    auto ui = figmalib::FigmaUI::fromFile(design);
    figmalib::ScriptHost host(*ui);
    if (drivePrefix) host.eval("globalThis.SELFDRIVE = true;", "<selfdrive>");
    if (!host.runFile(script)) {
        CloseWindow();
        return 1;
    }

    figmalib::RaylibFigmaView view(*ui);
    int frame = 0;
    while (!WindowShouldClose()) {
        if ((IsKeyPressed(KEY_BACKSPACE) && !ui->focusedNode()) ||
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            ui->navigateBack();
        }
        view.resize(GetScreenWidth(), GetScreenHeight());
        host.update(GetFrameTime());  // fires ui.onUpdate handlers
        view.update();

        BeginDrawing();
        ClearBackground(Color{12, 14, 18, 255});
        view.draw();
        EndDrawing();

        if (drivePrefix) {
            ++frame;
            if (frame == 30) {
                TakeScreenshot((std::string(drivePrefix) + "_home.png").c_str());
            } else if (frame == 110) {
                TakeScreenshot((std::string(drivePrefix) + "_nav.png").c_str());
            } else if (frame >= 140) {
                break;
            }
        }
    }
    CloseWindow();
    return 0;
}
