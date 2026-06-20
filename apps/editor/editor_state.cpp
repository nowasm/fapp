#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "editor.h"

namespace figoedit {

float gUiScale = 1.0f;
int kToolbarH = 44;
int kLayersW = 260;
int kInspectorW = 280;

void initUiScale() {
    // Window units are physical pixels (GLFW makes the process DPI-aware on
    // Windows), so scale the UI by the monitor's physical resolution.
    gUiScale = std::max(
        1.0f, static_cast<float>(GetMonitorWidth(GetCurrentMonitor())) / 1920.0f);
    gUiScale = std::min(gUiScale, 3.0f);
    if (const char* env = std::getenv("FIGOEDIT_SCALE"); env && *env) {
        gUiScale = std::max(0.5f, std::min(4.0f, static_cast<float>(std::atof(env))));
    }
    kToolbarH = static_cast<int>(44 * gUiScale);
    kLayersW = static_cast<int>(260 * gUiScale);
    kInspectorW = static_cast<int>(280 * gUiScale);
}

// ---- UI font -----------------------------------------------------------------

Font gUiFont{};
static bool gUiFontLoaded = false;

namespace {

// Decode UTF-8, collecting unique codepoints.
void collectCodepoints(const std::string& s, std::unordered_set<int>& out, bool& hasCjk) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int cp = 0;
        size_t len = 1;
        if (c < 0x80) { cp = c; }
        else if ((c >> 5) == 0x6 && i + 1 < s.size()) {
            cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            len = 2;
        } else if ((c >> 4) == 0xE && i + 2 < s.size()) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            len = 3;
        } else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) |
                 (s[i + 3] & 0x3F);
            len = 4;
        }
        if (cp >= 32) {
            out.insert(cp);
            // CJK proper (not emoji/symbols): radicals..unified, kana, fullwidth.
            if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                (cp >= 0xFF00 && cp <= 0xFFEF)) {
                hasCjk = true;
            }
        }
        i += len;
    }
}

void loadUiFont(const std::vector<int>& codepoints, bool wantCjk) {
    // Semibold for Latin (seguisb), bold YaHei for CJK — the UI reads better
    // a touch heavier at these sizes. raylib's LoadFontEx (stb_truetype) reads
    // face 0 at file offset 0, so it can't open .ttc collections — use .ttf
    // files on macOS (Arial Unicode covers Latin+CJK in one face).
#ifdef __APPLE__
    const char* candidates[] = {
        wantCjk ? "/Library/Fonts/Arial Unicode.ttf"
                : "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
#else
    const char* candidates[] = {
        wantCjk ? "C:/Windows/Fonts/msyhbd.ttc" : "C:/Windows/Fonts/seguisb.ttf",
        wantCjk ? "C:/Windows/Fonts/msyh.ttc" : "C:/Windows/Fonts/segoeui.ttf",
        wantCjk ? "C:/Windows/Fonts/simhei.ttf" : "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
    };
#endif
    for (const char* path : candidates) {
        Font f = LoadFontEx(path, fontM(), const_cast<int*>(codepoints.data()),
                            static_cast<int>(codepoints.size()));
        if (f.texture.id != 0 && f.glyphCount > 0) {
            if (gUiFontLoaded) UnloadFont(gUiFont);
            gUiFont = f;
            gUiFontLoaded = true;
            SetTextureFilter(gUiFont.texture, TEXTURE_FILTER_BILINEAR);
            std::fprintf(stderr, "[font] loaded %s (%d glyphs, cjk=%d)\n", path,
                         f.glyphCount, wantCjk ? 1 : 0);
            return;
        }
        std::fprintf(stderr, "[font] failed: %s (%zu codepoints)\n", path,
                     codepoints.size());
    }
    if (!gUiFontLoaded) gUiFont = GetFontDefault();  // last resort
}

}  // namespace

void initUiFont() {
    std::vector<int> ascii;
    for (int cp = 32; cp < 127; ++cp) ascii.push_back(cp);
    loadUiFont(ascii, false);
}

void rebuildUiFontFor(const figo::Document& doc) {
    std::unordered_set<int> cps;
    for (int cp = 32; cp < 127; ++cp) cps.insert(cp);
    bool hasCjk = false;
    if (doc.root) {
        const_cast<Node*>(doc.root.get())->visit([&](Node& n) {
            collectCodepoints(n.name, cps, hasCjk);
            collectCodepoints(n.characters, cps, hasCjk);
            return cps.size() < 4096;  // atlas safety cap
        });
    }
    std::vector<int> list(cps.begin(), cps.end());
    loadUiFont(list, hasCjk);
}

void uiText(const char* text, float x, float y, int size, ::Color color) {
    DrawTextEx(gUiFont, text, {x, y}, static_cast<float>(size), 0, color);
}

float uiMeasure(const char* text, int size) {
    return MeasureTextEx(gUiFont, text, static_cast<float>(size), 0).x;
}

NodeProps NodeProps::capture(Node* n) {
    NodeProps p;
    p.node = n;
    p.type = n->type;
    p.transform = n->relativeTransform;
    p.width = n->width;
    p.height = n->height;
    p.opacity = n->opacity;
    p.cornerRadius = n->cornerRadius;
    p.visible = n->visible;
    p.fills = n->fills;
    p.strokes = n->strokes;
    p.strokeWeight = n->strokeWeight;
    p.strokeAlign = n->strokeAlign;
    p.effects = n->effects;
    p.textStyle = n->textStyle;
    p.characters = n->characters;
    p.name = n->name;
    p.clipsContent = n->clipsContent;
    p.rectangleCornerRadii = n->rectangleCornerRadii;
    p.strokeDashes = n->strokeDashes;
    p.textRuns = n->textRuns;
    p.fillGeometry = n->fillGeometry;
    p.constraintH = n->constraintH;
    p.constraintV = n->constraintV;
    p.autoLayout = n->autoLayout;
    p.layoutGrow = n->layoutGrow;
    p.layoutAlignStretch = n->layoutAlignStretch;
    p.layoutAbsolute = n->layoutAbsolute;
    return p;
}

void NodeProps::apply() const {
    node->type = type;
    node->relativeTransform = transform;
    node->width = width;
    node->height = height;
    node->opacity = opacity;
    node->cornerRadius = cornerRadius;
    node->visible = visible;
    node->fills = fills;
    node->strokes = strokes;
    node->strokeWeight = strokeWeight;
    node->strokeAlign = strokeAlign;
    node->effects = effects;
    node->textStyle = textStyle;
    node->characters = characters;
    node->name = name;
    node->clipsContent = clipsContent;
    node->rectangleCornerRadii = rectangleCornerRadii;
    node->strokeDashes = strokeDashes;
    node->textRuns = textRuns;
    node->fillGeometry = fillGeometry;
    node->constraintH = constraintH;
    node->constraintV = constraintV;
    node->autoLayout = autoLayout;
    node->layoutGrow = layoutGrow;
    node->layoutAlignStretch = layoutAlignStretch;
    node->layoutAbsolute = layoutAbsolute;
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
        if (c->type == figo::NodeType::Canvas) canvases.push_back(c.get());
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
    updateAbsoluteTransforms();
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
std::unique_ptr<Node> cloneSubtree(const Node& src, Node* parent) {
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
    for (const auto& c : src.children) raw->children.push_back(cloneSubtree(*c, raw));
    return n;
}
}  // namespace

void EditorState::duplicateSelection() {
    if (selection.empty()) return;
    UndoEntry e;
    std::vector<Node*> fresh;
    for (Node* n : selection) {
        if (!n->parent) continue;
        auto copy = cloneSubtree(*n, n->parent);
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

// A reparent (MCP move_node) records two changes for the same node — a
// removal then an insertion — so the detached unique_ptr may be held by the
// sibling change rather than the one being replayed.
std::unique_ptr<Node> takeDetached(std::vector<TreeChange>& changes, TreeChange& ch) {
    if (ch.detached) return std::move(ch.detached);
    for (auto& other : changes) {
        if (other.node == ch.node && other.detached) return std::move(other.detached);
    }
    return nullptr;
}

void applyTreeUndo(std::vector<TreeChange>& changes, bool undoing) {
    auto detach = [](TreeChange& ch) {
        auto& siblings = ch.parent->children;
        for (size_t i = 0; i < siblings.size(); ++i) {
            if (siblings[i].get() == ch.node) {
                ch.detached = std::move(siblings[i]);
                siblings.erase(siblings.begin() + static_cast<long long>(i));
                break;
            }
        }
    };
    auto reinsert = [&changes](TreeChange& ch) {
        auto holder = takeDetached(changes, ch);
        if (!holder) return;  // defensive: node lost (should not happen)
        auto& siblings = ch.parent->children;
        const size_t at = std::min(ch.index, siblings.size());
        siblings.insert(siblings.begin() + static_cast<long long>(at), std::move(holder));
        ch.node->parent = ch.parent;  // reparent moves need the link restored
    };
    // Walk in reverse for undo so indices stay valid.
    if (undoing) {
        for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
            if (it->isInsert) detach(*it);   // undo an insertion → detach
            else reinsert(*it);              // undo a removal → insert back
        }
    } else {
        for (auto& ch : changes) {
            if (ch.isInsert) reinsert(ch);
            else detach(ch);
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

void EditorState::alignSelection(Align op) {
    if (selection.empty()) return;
    std::vector<NodeProps> before;
    for (Node* n : selection) before.push_back(NodeProps::capture(n));

    // Target box in world space.
    WorldRect target;
    if (selection.size() == 1) {
        Node* parent = selection[0]->parent;
        if (!parent || parent->width <= 0 || parent->height <= 0) return;
        target = worldBounds(*parent);
    } else {
        target.x0 = target.y0 = 1e30f;
        target.x1 = target.y1 = -1e30f;
        for (Node* n : selection) {
            const WorldRect b = worldBounds(*n);
            target.x0 = std::min(target.x0, b.x0);
            target.y0 = std::min(target.y0, b.y0);
            target.x1 = std::max(target.x1, b.x1);
            target.y1 = std::max(target.y1, b.y1);
        }
    }

    bool changed = false;
    for (Node* n : selection) {
        const WorldRect b = worldBounds(*n);
        float dx = 0, dy = 0;
        switch (op) {
        case Align::Left: dx = target.x0 - b.x0; break;
        case Align::HCenter:
            dx = (target.x0 + target.x1) * 0.5f - (b.x0 + b.x1) * 0.5f;
            break;
        case Align::Right: dx = target.x1 - b.x1; break;
        case Align::Top: dy = target.y0 - b.y0; break;
        case Align::VCenter:
            dy = (target.y0 + target.y1) * 0.5f - (b.y0 + b.y1) * 0.5f;
            break;
        case Align::Bottom: dy = target.y1 - b.y1; break;
        }
        if (dx == 0 && dy == 0) continue;
        // World delta → parent-local delta (parents usually translation-only).
        float lx = dx, ly = dy;
        if (n->parent) {
            if (auto inv = n->parent->absoluteTransform.inverted()) {
                lx = inv->m00 * dx + inv->m01 * dy;
                ly = inv->m10 * dx + inv->m11 * dy;
            }
        }
        n->relativeTransform.m02 = std::round(n->relativeTransform.m02 + lx);
        n->relativeTransform.m12 = std::round(n->relativeTransform.m12 + ly);
        bumpNode(n);
        changed = true;
    }
    if (changed) pushPropsUndo(std::move(before));
}

void EditorState::markDocChanged() {
    docDirty = true;
    renderer.markDirty();
    bumpAllFrames();
    updateAbsoluteTransforms();
}

uint64_t EditorState::versionOf(Node* topChild) {
    auto it = frameVersion.find(topChild);
    return it == frameVersion.end() ? 0 : it->second;
}

void EditorState::bumpNode(Node* n) {
    Node* top = n;
    while (top && top->parent && top->parent != page) top = top->parent;
    if (top) ++frameVersion[top];
    docDirty = true;
    updateAbsoluteTransforms();
}

void EditorState::bumpAllFrames() {
    if (!page) return;
    for (auto& c : page->children) ++frameVersion[c.get()];
}

void EditorState::invalidateCache() {
    for (auto& [node, entry] : frameCache) {
        if (entry.texValid) UnloadTexture(entry.tex);
    }
    frameCache.clear();
    frameVersion.clear();
    docDirty = true;
}

namespace {
void updateAbs(Node& n, const Mat23& parentAbs) {
    n.absoluteTransform = parentAbs * n.relativeTransform;
    for (auto& c : n.children) updateAbs(*c, n.absoluteTransform);
}
}  // namespace

void EditorState::updateAbsoluteTransforms() {
    if (!page) return;
    // Page space is world space: the page itself sits at identity.
    page->absoluteTransform = Mat23::identity();
    for (auto& c : page->children) updateAbs(*c, Mat23::identity());
}

}  // namespace figoedit
