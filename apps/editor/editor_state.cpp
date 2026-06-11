#include <cmath>
#include <cstdlib>

#include "editor.h"

namespace figmaedit {

float gUiScale = 1.0f;
int kToolbarH = 44;
int kLayersW = 260;
int kInspectorW = 280;

void initUiScale() {
    // GLFW only reports a DPI scale with FLAG_WINDOW_HIGHDPI; infer from the
    // monitor's physical resolution instead (1080p ≈ 1x, 4K ≈ 2x).
    const Vector2 dpi = GetWindowScaleDPI();
    const float fromMonitor =
        static_cast<float>(GetMonitorWidth(GetCurrentMonitor())) / 1920.0f;
    gUiScale = std::max({1.0f, dpi.x, fromMonitor});
    gUiScale = std::min(gUiScale, 3.0f);
    if (const char* env = std::getenv("FIGMAEDIT_SCALE"); env && *env) {
        gUiScale = std::max(0.5f, std::min(4.0f, static_cast<float>(std::atof(env))));
    }
    kToolbarH = static_cast<int>(44 * gUiScale);
    kLayersW = static_cast<int>(260 * gUiScale);
    kInspectorW = static_cast<int>(280 * gUiScale);
}

NodeProps NodeProps::capture(Node* n) {
    NodeProps p;
    p.node = n;
    p.transform = n->relativeTransform;
    p.width = n->width;
    p.height = n->height;
    p.opacity = n->opacity;
    p.cornerRadius = n->cornerRadius;
    p.visible = n->visible;
    p.fills = n->fills;
    p.characters = n->characters;
    return p;
}

void NodeProps::apply() const {
    node->relativeTransform = transform;
    node->width = width;
    node->height = height;
    node->opacity = opacity;
    node->cornerRadius = cornerRadius;
    node->visible = visible;
    node->fills = fills;
    node->characters = characters;
}

WorldRect worldBounds(const Node& n) {
    const Mat23& m = n.absoluteTransform;
    const float xs[4] = {0, n.width, n.width, 0};
    const float ys[4] = {0, 0, n.height, n.height};
    WorldRect r;
    r.x0 = r.y0 = 1e30f;
    r.x1 = r.y1 = -1e30f;
    for (int i = 0; i < 4; ++i) {
        float x, y;
        m.apply(xs[i], ys[i], x, y);
        r.x0 = std::min(r.x0, x);
        r.y0 = std::min(r.y0, y);
        r.x1 = std::max(r.x1, x);
        r.y1 = std::max(r.y1, y);
    }
    return r;
}

void EditorState::setStatus(const std::string& s, double seconds) {
    status = s;
    statusUntil = GetTime() + seconds;
}

void EditorState::selectPage(int index) {
    auto& pages = file.document->root->children;
    std::vector<Node*> canvases;
    for (auto& c : pages)
        if (c->type == figmalib::NodeType::Canvas) canvases.push_back(c.get());
    if (canvases.empty()) {  // bare tree: treat root as the page
        page = file.document->root.get();
    } else {
        pageIndex = ((index % static_cast<int>(canvases.size())) +
                     static_cast<int>(canvases.size())) %
                    static_cast<int>(canvases.size());
        page = canvases[pageIndex];
    }
    scope = page;
    selection.clear();
    hovered = nullptr;
    renderer.setFrame(page);
    docDirty = true;
    zoomToFit();
}

void EditorState::zoomToFit() {
    if (!page) return;
    // Bounds of all top-level frames (canvas coordinates).
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (auto& c : page->children) {
        if (!c->visible) continue;
        const Mat23& m = c->relativeTransform;
        x0 = std::min(x0, m.m02);
        y0 = std::min(y0, m.m12);
        x1 = std::max(x1, m.m02 + c->width);
        y1 = std::max(y1, m.m12 + c->height);
    }
    if (x1 <= x0) {
        x0 = y0 = 0;
        x1 = y1 = 1000;
    }
    const float pad = 48;
    const float vw = static_cast<float>(viewportW()), vh = static_cast<float>(viewportH());
    const float z = std::min((vw - pad * 2) / (x1 - x0), (vh - pad * 2) / (y1 - y0));
    cam.zoom = std::max(kZoomMin, std::min(kZoomMax, z));
    cam.panX = (vw - (x1 - x0) * cam.zoom) * 0.5f - x0 * cam.zoom;
    cam.panY = (vh - (y1 - y0) * cam.zoom) * 0.5f - y0 * cam.zoom;
    lastViewChange = GetTime();
    viewSettled = false;
}

bool EditorState::isSelected(Node* n) const {
    return std::find(selection.begin(), selection.end(), n) != selection.end();
}

void EditorState::setSelection(std::vector<Node*> sel) { selection = std::move(sel); }

void EditorState::beginGesture() {
    gestureBefore.clear();
    gestureChanged = false;
    for (Node* n : selection) gestureBefore.push_back(NodeProps::capture(n));
}

void EditorState::commitGesture() {
    if (!gestureChanged || gestureBefore.empty()) {
        gestureBefore.clear();
        return;
    }
    UndoEntry e;
    e.before = std::move(gestureBefore);
    for (const NodeProps& b : e.before) e.after.push_back(NodeProps::capture(b.node));
    undoStack.push_back(std::move(e));
    redoStack.clear();
    gestureBefore.clear();
    gestureChanged = false;
    unsaved = true;
}

void EditorState::pushPropsUndo(std::vector<NodeProps> before) {
    if (before.empty()) return;
    UndoEntry e;
    e.before = std::move(before);
    for (const NodeProps& b : e.before) e.after.push_back(NodeProps::capture(b.node));
    undoStack.push_back(std::move(e));
    redoStack.clear();
    unsaved = true;
}

void EditorState::deleteSelection() {
    if (selection.empty()) return;
    UndoEntry e;
    for (Node* n : selection) {
        if (!n->parent) continue;  // cannot delete a page/root
        auto& siblings = n->parent->children;
        for (size_t i = 0; i < siblings.size(); ++i) {
            if (siblings[i].get() != n) continue;
            TreeChange ch;
            ch.isInsert = false;
            ch.parent = n->parent;
            ch.index = i;
            ch.node = n;
            ch.detached = std::move(siblings[i]);
            siblings.erase(siblings.begin() + static_cast<long long>(i));
            e.tree.push_back(std::move(ch));
            break;
        }
    }
    if (e.tree.empty()) return;
    undoStack.push_back(std::move(e));
    redoStack.clear();
    selection.clear();
    hovered = nullptr;
    markDocChanged();
    unsaved = true;
}

namespace {
std::unique_ptr<Node> cloneNode(const Node& src, Node* parent) {
    auto n = std::make_unique<Node>();
    Node* raw = n.get();
    // Copy all value members, then rebuild children with fresh parent links.
    *raw = Node{};
    raw->id = src.id + "-copy";
    raw->name = src.name;
    raw->type = src.type;
    raw->visible = src.visible;
    raw->opacity = src.opacity;
    raw->relativeTransform = src.relativeTransform;
    raw->width = src.width;
    raw->height = src.height;
    raw->clipsContent = src.clipsContent;
    raw->fills = src.fills;
    raw->strokes = src.strokes;
    raw->strokeWeight = src.strokeWeight;
    raw->strokeAlign = src.strokeAlign;
    raw->strokeDashes = src.strokeDashes;
    raw->strokeCap = src.strokeCap;
    raw->strokeJoin = src.strokeJoin;
    raw->cornerRadius = src.cornerRadius;
    raw->rectangleCornerRadii = src.rectangleCornerRadii;
    raw->fillGeometry = src.fillGeometry;
    raw->strokeGeometry = src.strokeGeometry;
    raw->effects = src.effects;
    raw->characters = src.characters;
    raw->textStyle = src.textStyle;
    raw->textRuns = src.textRuns;
    raw->parent = parent;
    for (const auto& c : src.children) raw->children.push_back(cloneNode(*c, raw));
    return n;
}
}  // namespace

void EditorState::duplicateSelection() {
    if (selection.empty()) return;
    UndoEntry e;
    std::vector<Node*> fresh;
    for (Node* n : selection) {
        if (!n->parent) continue;
        auto copy = cloneNode(*n, n->parent);
        copy->relativeTransform.m02 += 10;  // Figma offsets duplicates slightly
        copy->relativeTransform.m12 += 10;
        Node* raw = copy.get();
        n->parent->children.push_back(std::move(copy));
        TreeChange ch;
        ch.isInsert = true;
        ch.parent = n->parent;
        ch.index = n->parent->children.size() - 1;
        ch.node = raw;
        e.tree.push_back(std::move(ch));
        fresh.push_back(raw);
    }
    if (fresh.empty()) return;
    undoStack.push_back(std::move(e));
    redoStack.clear();
    selection = fresh;
    markDocChanged();
    unsaved = true;
}

namespace {

void applyTreeUndo(std::vector<TreeChange>& changes, bool undoing) {
    // Walk in reverse for undo so indices stay valid.
    if (undoing) {
        for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
            TreeChange& ch = *it;
            const bool reinsert = !ch.isInsert;  // undo a removal → insert back
            auto& siblings = ch.parent->children;
            if (reinsert) {
                const size_t at = std::min(ch.index, siblings.size());
                siblings.insert(siblings.begin() + static_cast<long long>(at),
                                std::move(ch.detached));
            } else {  // undo an insertion → detach
                for (size_t i = 0; i < siblings.size(); ++i) {
                    if (siblings[i].get() == ch.node) {
                        ch.detached = std::move(siblings[i]);
                        siblings.erase(siblings.begin() + static_cast<long long>(i));
                        break;
                    }
                }
            }
        }
    } else {
        for (auto& ch : changes) {
            auto& siblings = ch.parent->children;
            if (ch.isInsert) {
                const size_t at = std::min(ch.index, siblings.size());
                siblings.insert(siblings.begin() + static_cast<long long>(at),
                                std::move(ch.detached));
            } else {
                for (size_t i = 0; i < siblings.size(); ++i) {
                    if (siblings[i].get() == ch.node) {
                        ch.detached = std::move(siblings[i]);
                        siblings.erase(siblings.begin() + static_cast<long long>(i));
                        break;
                    }
                }
            }
        }
    }
}

}  // namespace

void EditorState::undo() {
    if (undoStack.empty()) return;
    UndoEntry e = std::move(undoStack.back());
    undoStack.pop_back();
    for (const NodeProps& p : e.before) p.apply();
    applyTreeUndo(e.tree, true);
    selection.clear();
    hovered = nullptr;
    redoStack.push_back(std::move(e));
    markDocChanged();
    unsaved = true;
}

void EditorState::redo() {
    if (redoStack.empty()) return;
    UndoEntry e = std::move(redoStack.back());
    redoStack.pop_back();
    for (const NodeProps& p : e.after) p.apply();
    applyTreeUndo(e.tree, false);
    selection.clear();
    hovered = nullptr;
    undoStack.push_back(std::move(e));
    markDocChanged();
    unsaved = true;
}

void EditorState::markDocChanged() {
    docDirty = true;
    renderer.markDirty();
}

}  // namespace figmaedit
