// Native file dialogs. Isolated in its own TU: <windows.h> redefines several
// raylib symbols (CloseWindow, DrawText, Rectangle, ...), so no raylib here.

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

namespace figmaedit {

#ifdef _WIN32

static constexpr const char* kFilter =
    "Figma files (*.fig;*.json)\0*.fig;*.json\0"
    "Figma binary (*.fig)\0*.fig\0"
    "JSON (*.json)\0*.json\0"
    "All files (*.*)\0*.*\0";

std::string showOpenFileDialog() {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Open Figma file";
    // NOCHANGEDIR: the dialog must not change the process cwd (relative
    // asset paths would silently break).
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return {};
    return path;
}

std::string showSaveFileDialog(const std::string& suggested) {
    char path[MAX_PATH] = {};
    suggested.copy(path, sizeof(path) - 1);
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = "JSON (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Save as";
    ofn.lpstrDefExt = "json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameA(&ofn)) return {};
    return path;
}

#else

std::string showOpenFileDialog() { return {}; }
std::string showSaveFileDialog(const std::string&) { return {}; }

#endif

}  // namespace figmaedit
