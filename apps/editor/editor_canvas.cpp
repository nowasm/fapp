// Canvas interaction + rendering: Figma-faithful navigation, selection and
// transform gestures, with the page rasterized by figmalib/ThorVG and
// overlays (hover, selection, handles, marquee) drawn by raylib on top.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "editor.h"

namespace figmaedit {

namespace {

constexpr ::Color kAccent{13, 153, 255, 255};        // Figma selection blue
constexpr ::Color kAccentDim{13, 153, 255, 50};
inline float handleSize() { return ui(8.0f); }
constexpr double kDoubleClickSec = 0.35;

double g_lastClickTime = -1;
Vector2 g_lastClickPos{};

// ---- hit testing -----------------------------------------------------------

bool pointInNode(const Node& n, float wx, float wy) {
    const auto inv = n.absoluteTransform.inverted();
    if (!inv) return false;
    float lx, ly;
    inv->apply(wx, wy, lx, ly);
    return lx >= 0 && ly >= 0 && lx <= n.width && ly <= n.height;
}

// Deepest visible node under the point (Figma Ctrl+click deep select).
Node* deepHit(Node& root, float wx, float wy, bool isRoot = true) {
    if (!root.visible) return nullptr;
    const bool inside = pointInNode(root, wx, wy);
    if (root.clipsContent && !inside) return nullptr;
    for (auto it = root.children.rbegin(); it != root.children.rend(); ++it) {
        if (Node* hit = deepHit(**it, wx, wy, false)) return hit;
    }
    if (isRoot || !inside) return nullptr;
    const bool paintable = !root.fills.empty() || !root.strokes.empty() ||
                           !root.fillGeometry.empty() || !root.characters.empty() ||
                           !root.children.empty();
    return paintable ? &root : nullptr;
}

// Figma single click: the ancestor of the deep hit that is a direct child of
// the current scope.
Node* scopedHit(EditorState& ed, float wx, float wy) {
    Node* hit = deepHit(*ed.page, wx, wy);
    if (!hit) return nullptr;
    Node* n = hit;
    while (n->parent && n->parent != ed.scope) n = n->parent;
    return n->parent == ed.scope ? n : nullptr;
}

// ---- gesture application ----------------------------------------------------

void applyMove(EditorState& ed, float dwx, float dwy) {
    for (const NodeProps& before : ed.gestureBefore) {
        Node* n = before.node;
        float dx = dwx, dy = dwy;
        if (n->parent) {
            // World delta → parent-local delta through the parent's inverse
            // linear transform (parents are usually translation-only).
            if (auto inv = n->parent->absoluteTransform.inverted()) {
                dx = inv->m00 * dwx + inv->m01 * dwy;
                dy = inv->m10 * dwx + inv->m11 * dwy;
            }
        }
        n->relativeTransform = before.transform;
        n->relativeTransform.m02 = std::round(before.transform.m02 + dx);
        n->relativeTransform.m12 = std::round(before.transform.m12 + dy);
    }
    ed.gestureChanged = true;
    ed.markDocChanged();
}

void applyResize(EditorState& ed, float wx, float wy, bool uniform, bool centered) {
    if (ed.gestureBefore.empty()) return;
    const NodeProps& before = ed.gestureBefore[0];
    Node* n = before.node;
    if (!n->parent) return;

    // Mouse in parent space (node assumed axis-aligned in parent for v1).
    float px = wx, py = wy;
    if (auto inv = n->parent->absoluteTransform.inverted()) inv->apply(wx, wy, px, py);

    const float sx0 = before.transform.m00, sy0 = before.transform.m11;
    const float bw = before.width * (sx0 == 0 ? 1 : sx0);
    const float bh = before.height * (sy0 == 0 ? 1 : sy0);
    float x0 = before.transform.m02, y0 = before.transform.m12;
    float x1 = x0 + bw, y1 = y0 + bh;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;

    const int h = ed.resizeHandle;
    const bool left = h == 0 || h == 3 || h == 7;
    const bool right = h == 1 || h == 2 || h == 5;
    const bool top = h == 0 || h == 1 || h == 4;
    const bool bottom = h == 2 || h == 3 || h == 6;

    if (left) x0 = px;
    if (right) x1 = px;
    if (top) y0 = py;
    if (bottom) y1 = py;

    if (centered) {  // Alt: resize around the center
        if (left) x1 = 2 * cx - x0;
        if (right) x0 = 2 * cx - x1;
        if (top) y1 = 2 * cy - y0;
        if (bottom) y0 = 2 * cy - y1;
    }
    if (uniform && bw > 0 && bh > 0) {  // Shift: keep aspect ratio
        const float rw = (x1 - x0) / bw, rh = (y1 - y0) / bh;
        const float r = (left || right) && !(top || bottom) ? rw
                        : (top || bottom) && !(left || right) ? rh
                        : std::fabs(rw) > std::fabs(rh) ? rw : rh;
        const float nw = bw * r, nh = bh * r;
        if (left) x0 = x1 - nw; else x1 = x0 + nw;
        if (top) y0 = y1 - nh; else y1 = y0 + nh;
        if (centered) {
            const float hw = nw * 0.5f, hh = nh * 0.5f;
            x0 = cx - hw; x1 = cx + hw;
            y0 = cy - hh; y1 = cy + hh;
        }
    }

    float nw = x1 - x0, nh = y1 - y0;
    if (std::fabs(nw) < 1) nw = nw < 0 ? -1.0f : 1.0f;
    if (std::fabs(nh) < 1) nh = nh < 0 ? -1.0f : 1.0f;
    if (nw < 0) { x0 += nw; nw = -nw; }
    if (nh < 0) { y0 += nh; nh = -nh; }

    n->relativeTransform = before.transform;
    n->relativeTransform.m02 = std::round(x0);
    n->relativeTransform.m12 = std::round(y0);

    const bool hasGeometry = !n->fillGeometry.empty() || !n->strokeGeometry.empty();
    if (hasGeometry) {
        // Baked vector outlines: scale through the transform's linear part so
        // the path stretches with the box (local w/h stay in path units).
        n->relativeTransform.m00 = before.transform.m00 * (nw / bw);
        n->relativeTransform.m11 = before.transform.m11 * (nh / bh);
    } else {
        n->width = std::round(nw / (sx0 == 0 ? 1 : sx0));
        n->height = std::round(nh / (sy0 == 0 ? 1 : sy0));
    }
    ed.gestureChanged = true;
    ed.markDocChanged();
}

// Handle hit zones of the single-selection bounds (screen space).
int handleAt(EditorState& ed, float mx, float my) {
    if (ed.selection.size() != 1) return -1;
    const WorldRect wr = worldBounds(*ed.selection[0]);
    float x0, y0, x1, y1;
    ed.cam.worldToScreen(wr.x0, wr.y0, x0, y0);
    ed.cam.worldToScreen(wr.x1, wr.y1, x1, y1);
    const float hs = handleSize();
    const Vector2 corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(mx - corners[i].x) <= hs && std::fabs(my - corners[i].y) <= hs)
            return i;
    }
    // Edges (slightly tighter zone, corner wins).
    const float ez = ui(4);
    if (my >= y0 - ez && my <= y1 + ez) {
        if (std::fabs(mx - x0) <= ez) return 7;
        if (std::fabs(mx - x1) <= ez) return 5;
    }
    if (mx >= x0 - ez && mx <= x1 + ez) {
        if (std::fabs(my - y0) <= ez) return 4;
        if (std::fabs(my - y1) <= ez) return 6;
    }
    return -1;
}

void nudgeSelection(EditorState& ed, float dx, float dy) {
    if (ed.selection.empty()) return;
    std::vector<NodeProps> before;
    for (Node* n : ed.selection) before.push_back(NodeProps::capture(n));
    for (Node* n : ed.selection) {
        n->relativeTransform.m02 += dx;
        n->relativeTransform.m12 += dy;
    }
    ed.pushPropsUndo(std::move(before));
    ed.markDocChanged();
}

}  // namespace

void updateCanvas(EditorState& ed) {
    const float mx = static_cast<float>(GetMouseX()) - ed.viewportX();
    const float my = static_cast<float>(GetMouseY()) - ed.viewportY();
    const bool inViewport = mx >= 0 && my >= 0 && mx < ed.viewportW() && my < ed.viewportH();
    float wx, wy;
    ed.cam.screenToWorld(mx, my, wx, wy);

    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);

    // ---- keyboard ----
    ed.spaceHeld = IsKeyDown(KEY_SPACE) && !ed.textEditActive;
    if (!ed.textEditActive) {
        if (IsKeyPressed(KEY_V)) ed.tool = Tool::Move;
        if (IsKeyPressed(KEY_H)) ed.tool = Tool::Hand;
        if (ctrl && IsKeyPressed(KEY_Z)) {
            shift ? ed.redo() : ed.undo();
        }
        if (ctrl && IsKeyPressed(KEY_Y)) ed.redo();
        if (ctrl && IsKeyPressed(KEY_D)) ed.duplicateSelection();
        if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) ed.deleteSelection();

        const float step = shift ? 10.0f : 1.0f;
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) nudgeSelection(ed, -step, 0);
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) nudgeSelection(ed, step, 0);
        if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) nudgeSelection(ed, 0, -step);
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) nudgeSelection(ed, 0, step);

        if (IsKeyPressed(KEY_ESCAPE)) {
            if (ed.drag != DragMode::None) {
                // cancel gesture: restore snapshots
                for (const NodeProps& p : ed.gestureBefore) p.apply();
                ed.gestureBefore.clear();
                ed.gestureChanged = false;
                ed.drag = DragMode::None;
                ed.markDocChanged();
            } else if (!ed.selection.empty()) {
                ed.selection.clear();
                ed.scope = ed.page;
            } else {
                ed.scope = ed.page;
            }
        }
        if (IsKeyPressed(KEY_ENTER) && ed.selection.size() == 1 &&
            !ed.selection[0]->children.empty()) {
            // Figma Enter: drill into the selected container, select first child.
            ed.scope = ed.selection[0];
            ed.selection = {ed.scope->children.back().get()};
        }

        // zoom shortcuts
        const float cxv = ed.viewportW() * 0.5f, cyv = ed.viewportH() * 0.5f;
        if (ctrl && (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))) {
            ed.cam.zoomAt(cxv, cyv, 1.25f);
            ed.lastViewChange = GetTime();
            ed.viewSettled = false;
        }
        if (ctrl && (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))) {
            ed.cam.zoomAt(cxv, cyv, 0.8f);
            ed.lastViewChange = GetTime();
            ed.viewSettled = false;
        }
        if (ctrl && IsKeyPressed(KEY_ZERO)) {
            ed.cam.zoomAt(cxv, cyv, 1.0f / ed.cam.zoom);
            ed.lastViewChange = GetTime();
            ed.viewSettled = false;
        }
        if (shift && IsKeyPressed(KEY_ONE)) ed.zoomToFit();
    }

    // ---- wheel ----
    const float wheel = GetMouseWheelMove();
    if (wheel != 0 && inViewport) {
        if (ctrl) {
            ed.cam.zoomAt(mx, my, std::exp(wheel * 0.18f));
        } else if (shift) {
            ed.cam.panX += wheel * 60.0f;
        } else {
            ed.cam.panY += wheel * 60.0f;
        }
        ed.lastViewChange = GetTime();
        ed.viewSettled = false;
    }

    // ---- mouse buttons ----
    const bool panButton = IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) ||
                           (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                            (ed.spaceHeld || ed.tool == Tool::Hand));

    if (inViewport && ed.drag == DragMode::None && panButton) {
        ed.drag = DragMode::Pan;
        ed.dragStartScreen = {mx, my};
    } else if (inViewport && ed.drag == DragMode::None &&
               IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && ed.tool == Tool::Move) {
        const double now = GetTime();
        const bool isDouble = now - g_lastClickTime < kDoubleClickSec &&
                              std::fabs(mx - g_lastClickPos.x) < 4 &&
                              std::fabs(my - g_lastClickPos.y) < 4;
        g_lastClickTime = now;
        g_lastClickPos = {mx, my};

        const int handle = handleAt(ed, mx, my);
        if (handle >= 0) {
            ed.resizeHandle = handle;
            ed.drag = DragMode::Resize;
            ed.beginGesture();
        } else if (isDouble) {
            // Drill into the container under the cursor.
            Node* container = scopedHit(ed, wx, wy);
            if (container && !container->children.empty()) {
                ed.scope = container;
                Node* inner = scopedHit(ed, wx, wy);
                ed.selection = inner ? std::vector<Node*>{inner} : std::vector<Node*>{};
            }
        } else {
            Node* hit = ctrl ? deepHit(*ed.page, wx, wy) : scopedHit(ed, wx, wy);
            if (hit) {
                if (shift) {
                    if (ed.isSelected(hit)) {
                        ed.selection.erase(
                            std::remove(ed.selection.begin(), ed.selection.end(), hit),
                            ed.selection.end());
                    } else {
                        ed.selection.push_back(hit);
                    }
                } else if (!ed.isSelected(hit)) {
                    ed.selection = {hit};
                }
                if (!ed.selection.empty()) {
                    ed.drag = DragMode::MoveNodes;
                    ed.dragStartScreen = {mx, my};
                    ed.dragStartWX = wx;
                    ed.dragStartWY = wy;
                    ed.beginGesture();
                }
            } else {
                if (!shift) ed.selection.clear();
                ed.drag = DragMode::Marquee;
                ed.dragStartScreen = {mx, my};
                ed.dragStartWX = wx;
                ed.dragStartWY = wy;
            }
        }
    }

    // ---- drag update ----
    switch (ed.drag) {
    case DragMode::Pan: {
        const Vector2 d = GetMouseDelta();
        if (d.x != 0 || d.y != 0) {
            ed.cam.panX += d.x;
            ed.cam.panY += d.y;
            ed.lastViewChange = GetTime();
            ed.viewSettled = false;
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE) &&
            !IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            ed.drag = DragMode::None;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
            !IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
            ed.drag = DragMode::None;
        break;
    }
    case DragMode::MoveNodes: {
        float dwx = wx - ed.dragStartWX;
        float dwy = wy - ed.dragStartWY;
        const float distPx = std::hypot(mx - ed.dragStartScreen.x, my - ed.dragStartScreen.y);
        if (distPx > 3 || ed.gestureChanged) {  // drag threshold
            if (shift) {  // axis lock
                if (std::fabs(dwx) > std::fabs(dwy)) dwy = 0;
                else dwx = 0;
            }
            applyMove(ed, dwx, dwy);
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            ed.commitGesture();
            ed.drag = DragMode::None;
        }
        break;
    }
    case DragMode::Resize: {
        applyResize(ed, wx, wy, shift, alt);
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            ed.commitGesture();
            ed.drag = DragMode::None;
            ed.resizeHandle = -1;
        }
        break;
    }
    case DragMode::Marquee: {
        WorldRect r;
        r.x0 = std::min(ed.dragStartWX, wx);
        r.y0 = std::min(ed.dragStartWY, wy);
        r.x1 = std::max(ed.dragStartWX, wx);
        r.y1 = std::max(ed.dragStartWY, wy);
        std::vector<Node*> sel;
        for (auto& c : ed.scope->children) {
            if (!c->visible) continue;
            if (worldBounds(*c).intersects(r)) sel.push_back(c.get());
        }
        ed.selection = std::move(sel);
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) ed.drag = DragMode::None;
        break;
    }
    case DragMode::None:
    default:
        ed.hovered = inViewport && !ed.spaceHeld && ed.tool == Tool::Move
                         ? scopedHit(ed, wx, wy)
                         : nullptr;
        break;
    }

    // ---- cursor ----
    if (ed.drag == DragMode::Pan || ed.spaceHeld || ed.tool == Tool::Hand) {
        SetMouseCursor(MOUSE_CURSOR_RESIZE_ALL);
    } else if (ed.drag == DragMode::Resize || (inViewport && handleAt(ed, mx, my) >= 0)) {
        const int h = ed.drag == DragMode::Resize ? ed.resizeHandle : handleAt(ed, mx, my);
        SetMouseCursor(h == 0 || h == 2   ? MOUSE_CURSOR_RESIZE_NWSE
                       : h == 1 || h == 3 ? MOUSE_CURSOR_RESIZE_NESW
                       : h == 5 || h == 7 ? MOUSE_CURSOR_RESIZE_EW
                                          : MOUSE_CURSOR_RESIZE_NS);
    } else {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    }
}

void drawCanvas(EditorState& ed) {
    const int vx = ed.viewportX(), vy = ed.viewportY();
    const int vw = ed.viewportW(), vh = ed.viewportH();
    if (vw <= 0 || vh <= 0) return;

    // ---- re-rasterize when needed ----
    ed.renderer.setTarget(static_cast<uint32_t>(vw), static_cast<uint32_t>(vh));
    const bool settleDue = !ed.viewSettled && GetTime() - ed.lastViewChange > 0.12;
    if (ed.docDirty || settleDue || !ed.canvasTexValid) {
        ed.renderer.setViewTransform(ed.cam.view());
        if (ed.renderer.render() || !ed.canvasTexValid) {
            if (!ed.canvasTexValid ||
                ed.canvasTex.width != vw || ed.canvasTex.height != vh) {
                if (ed.canvasTexValid) UnloadTexture(ed.canvasTex);
                Image img{};
                img.data = const_cast<uint32_t*>(ed.renderer.pixels());
                img.width = vw;
                img.height = vh;
                img.mipmaps = 1;
                img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                ed.canvasTex = LoadTextureFromImage(img);
                ed.canvasTexValid = ed.canvasTex.id != 0;
            } else {
                UpdateTexture(ed.canvasTex, ed.renderer.pixels());
            }
            ed.texCam = ed.cam;
        }
        ed.docDirty = false;
        if (settleDue) ed.viewSettled = true;
    }

    // ---- background ----
    ::Color bg{30, 30, 30, 255};  // Figma dark canvas
    if (ed.page && !ed.page->fills.empty()) {
        const auto& c = ed.page->fills[0].color;
        bg = {static_cast<unsigned char>(c.r * 255), static_cast<unsigned char>(c.g * 255),
              static_cast<unsigned char>(c.b * 255), 255};
    }
    DrawRectangle(vx, vy, vw, vh, bg);

    BeginScissorMode(vx, vy, vw, vh);

    // ---- canvas texture (remapped if the view moved since the last raster) ----
    if (ed.canvasTexValid) {
        const float s = ed.cam.zoom / ed.texCam.zoom;
        // World point of the texture's (0,0) → current screen position.
        float w0x, w0y;
        ed.texCam.screenToWorld(0, 0, w0x, w0y);
        float sx, sy;
        ed.cam.worldToScreen(w0x, w0y, sx, sy);
        const Rectangle src{0, 0, static_cast<float>(ed.canvasTex.width),
                            static_cast<float>(ed.canvasTex.height)};
        const Rectangle dst{vx + sx, vy + sy, ed.canvasTex.width * s,
                            ed.canvasTex.height * s};
        DrawTexturePro(ed.canvasTex, src, dst, {0, 0}, 0, WHITE);
    }

    // ---- overlays ----
    auto drawNodeOutline = [&](const Node& n, ::Color color, float thickness) {
        const Mat23& m = n.absoluteTransform;
        Vector2 pts[5];
        const float xs[4] = {0, n.width, n.width, 0};
        const float ys[4] = {0, 0, n.height, n.height};
        for (int i = 0; i < 4; ++i) {
            float wxp, wyp, sxp, syp;
            m.apply(xs[i], ys[i], wxp, wyp);
            ed.cam.worldToScreen(wxp, wyp, sxp, syp);
            pts[i] = {vx + sxp, vy + syp};
        }
        pts[4] = pts[0];
        for (int i = 0; i < 4; ++i) DrawLineEx(pts[i], pts[i + 1], thickness, color);
    };

    if (ed.hovered && !ed.isSelected(ed.hovered)) {
        drawNodeOutline(*ed.hovered, kAccent, 1.5f);
    }
    for (Node* n : ed.selection) drawNodeOutline(*n, kAccent, 2.0f);

    // handles + size label for single selection
    if (ed.selection.size() == 1) {
        const Node& n = *ed.selection[0];
        const WorldRect wr = worldBounds(n);
        float x0, y0, x1, y1;
        ed.cam.worldToScreen(wr.x0, wr.y0, x0, y0);
        ed.cam.worldToScreen(wr.x1, wr.y1, x1, y1);
        x0 += vx; x1 += vx; y0 += vy; y1 += vy;
        const float hs = handleSize();
        const Vector2 corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        for (const Vector2& c : corners) {
            DrawRectangleRec({c.x - hs / 2, c.y - hs / 2, hs, hs}, WHITE);
            DrawRectangleLinesEx({c.x - hs / 2, c.y - hs / 2, hs, hs}, 1.5f, kAccent);
        }
        char label[64];
        std::snprintf(label, sizeof(label), "%g x %g", std::round(wr.w()), std::round(wr.h()));
        const int tw = MeasureText(label, fontS());
        const float lx = (x0 + x1) * 0.5f - tw * 0.5f;
        const float ly = y1 + ui(8);
        DrawRectangleRounded({lx - ui(6), ly - ui(3), tw + ui(12.0f), ui(18)}, 0.4f, 4,
                             kAccent);
        DrawText(label, static_cast<int>(lx), static_cast<int>(ly), fontS(), WHITE);
    }

    // debug readout (FIGMAEDIT_DEBUG=1)
    static const bool dbg = []() {
        const char* e = std::getenv("FIGMAEDIT_DEBUG");
        return e && *e == '1';
    }();
    if (dbg) {
        const float mx = static_cast<float>(GetMouseX()) - vx;
        const float my = static_cast<float>(GetMouseY()) - vy;
        float wxd, wyd;
        ed.cam.screenToWorld(mx, my, wxd, wyd);
        char info[256];
        std::snprintf(info, sizeof(info),
                      "mouse vp(%.0f,%.0f) world(%.0f,%.0f) zoom=%.2f pan(%.0f,%.0f) hover=%s",
                      mx, my, wxd, wyd, ed.cam.zoom, ed.cam.panX, ed.cam.panY,
                      ed.hovered ? ed.hovered->name.c_str() : "-");
        DrawText(info, vx + 10, vy + 10, fontS(), ::Color{255, 220, 0, 255});
    }

    // marquee
    if (ed.drag == DragMode::Marquee) {
        float ax, ay;
        ed.cam.worldToScreen(ed.dragStartWX, ed.dragStartWY, ax, ay);
        const float bx2 = static_cast<float>(GetMouseX()) - vx;
        const float by2 = static_cast<float>(GetMouseY()) - vy;
        const Rectangle r{vx + std::min(ax, bx2), vy + std::min(ay, by2),
                          std::fabs(bx2 - ax), std::fabs(by2 - ay)};
        DrawRectangleRec(r, kAccentDim);
        DrawRectangleLinesEx(r, 1, kAccent);
    }

    EndScissorMode();
}

}  // namespace figmaedit
