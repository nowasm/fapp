// figmaedit — Figma-style editor built on figmalib + raylib + raygui.
//
//   figmaedit [file.fig | canvas.json | file.json]
//
// Drag & drop a file onto the window to open it. Ctrl+S saves a figmalib
// JSON next to the original (never overwrites the .fig).

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

#include "editor.h"

#include <raygui.h>

#ifndef ASSETS_DIR
#define ASSETS_DIR "."
#endif

namespace fs = std::filesystem;
using namespace figmaedit;

namespace {

void registerConventionFonts(EditorState& ed, const std::string& inputPath) {
    std::error_code ec;
    ed.fontDirs.clear();
    const fs::path sibling = fs::path(inputPath).parent_path() / "fonts";
    if (fs::is_directory(sibling, ec)) ed.fontDirs.push_back(sibling.string());
    if (const char* env = std::getenv("FIGMALIB_FONTS_DIR"); env && *env) {
        if (fs::is_directory(env, ec)) ed.fontDirs.push_back(env);
    }
    for (const auto& dir : ed.fontDirs) ed.renderer.registerFontsFromDirectory(dir);
}

bool openFile(EditorState& ed, const std::string& path) {
    try {
        figmalib::LoadedFile loaded = figmalib::loadFigmaFile(path);
        ed.invalidateCache();  // before the old document (and its nodes) dies
        ed.file = std::move(loaded);
        ed.filePath = path;
        ed.savePath = path + ".figmalib.json";
        ed.imageDir = ed.file.imageDirectory;
        if (!ed.imageDir.empty()) ed.renderer.setImageDirectory(ed.imageDir);
        registerConventionFonts(ed, path);
        ed.undoStack.clear();
        ed.redoStack.clear();
        ed.expanded.clear();
        ed.unsaved = false;
        ed.selectPage(0);
        ed.updateAbsoluteTransforms();
        ed.setStatus("Opened " + path);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "figmaedit: %s\n", e.what());
        ed.setStatus(std::string("Open failed: ") + e.what(), 5);
        return false;
    }
}

}  // namespace

// Headless logic check: document mutations, undo/redo, tree ops, save
// round-trip — everything below the mouse wiring. Run: figmaedit --selftest [file]
static int selftest(const std::string& path) {
    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    };

    EditorState ed;
    figmalib::LoadedFile loaded = figmalib::loadFigmaFile(path);
    ed.file = std::move(loaded);
    Node* page = nullptr;
    for (auto& c : ed.file.document->root->children) {
        if (c->type == figmalib::NodeType::Canvas) {
            page = c.get();
            break;
        }
    }
    if (!page) page = ed.file.document->root.get();
    ed.page = page;
    ed.scope = page;
    expect(!page->children.empty(), "page has frames");
    Node* frame = page->children.front().get();

    // move + undo
    const float x0 = frame->relativeTransform.m02;
    ed.selection = {frame};
    ed.beginGesture();
    frame->relativeTransform.m02 = x0 + 100;
    ed.gestureChanged = true;
    ed.commitGesture();
    expect(frame->relativeTransform.m02 == x0 + 100, "move applied");
    ed.undo();
    expect(frame->relativeTransform.m02 == x0, "move undone");
    ed.redo();
    expect(frame->relativeTransform.m02 == x0 + 100, "move redone");
    ed.undo();

    // duplicate + delete + undo
    const size_t count = page->children.size();
    ed.selection = {frame};
    ed.duplicateSelection();
    expect(page->children.size() == count + 1, "duplicate adds a sibling");
    ed.deleteSelection();  // deletes the duplicate (now selected)
    expect(page->children.size() == count, "delete removes it");
    ed.undo();
    expect(page->children.size() == count + 1, "undo restores deleted node");
    ed.undo();
    expect(page->children.size() == count, "undo removes the duplicate");

    // save round-trip
    const std::string tmp = path + ".selftest.json";
    expect(figmalib::saveDocumentFile(*ed.file.document, tmp), "save");
    auto reloaded = figmalib::loadFigmaFile(tmp);
    Node* rpage = nullptr;
    for (auto& c : reloaded.document->root->children) {
        if (c->type == figmalib::NodeType::Canvas) {
            rpage = c.get();
            break;
        }
    }
    expect(rpage && rpage->children.size() == count, "round-trip frame count");
    if (rpage && !rpage->children.empty()) {
        Node* rframe = rpage->children.front().get();
        expect(std::fabs(rframe->relativeTransform.m02 - x0) < 0.01f &&
                   rframe->width == frame->width && rframe->name == frame->name,
               "round-trip frame props");
    }
    std::remove(tmp.c_str());
    std::printf(failures ? "SELFTEST: %d failure(s)\n" : "SELFTEST: OK\n", failures);
    return failures;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--selftest") {
        const std::string file = argc > 2 ? argv[2] : ASSETS_DIR "/sample_ui.json";
        return selftest(file);
    }
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1440, 900, "figmaedit");
    SetExitKey(0);  // Esc is a selection command, not quit
    SetTargetFPS(120);
    figmaedit::initUiScale();
    GuiSetStyle(DEFAULT, TEXT_SIZE, figmaedit::fontM());
    // Make the window comfortable on the current monitor.
    {
        const int mon = GetCurrentMonitor();
        const int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
        SetWindowSize(static_cast<int>(mw * 0.85f), static_cast<int>(mh * 0.85f));
        SetWindowPosition(static_cast<int>(mw * 0.06f), static_cast<int>(mh * 0.05f));
    }

    EditorState ed;
    const std::string initial = argc > 1 ? argv[1] : ASSETS_DIR "/sample_ui.json";
    if (!openFile(ed, initial)) {
        // keep the window open so a file can be dropped in
        ed.setStatus("Drop a .fig / .json file to open", 10);
    }

    while (!WindowShouldClose()) {
        // drag & drop
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            if (dropped.count > 0) openFile(ed, dropped.paths[0]);
            UnloadDroppedFiles(dropped);
        }
        // save
        const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (ctrl && IsKeyPressed(KEY_S) && ed.file.document) {
            if (figmalib::saveDocumentFile(*ed.file.document, ed.savePath)) {
                ed.unsaved = false;
                ed.setStatus("Saved " + ed.savePath);
            } else {
                ed.setStatus("Save FAILED: " + ed.savePath, 5);
            }
        }

        if (ed.file.document && ed.page) updateCanvas(ed);

        BeginDrawing();
        ClearBackground(::Color{30, 30, 30, 255});
        if (ed.file.document && ed.page) drawCanvas(ed);
        drawToolbar(ed);
        drawLayersPanel(ed);
        drawInspector(ed);
        EndDrawing();
    }

    ed.invalidateCache();
    CloseWindow();
    return 0;
}
