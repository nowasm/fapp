#!/usr/bin/env python3
"""Generate design.json for the calculator benchmark app.

Deterministic grid layout, linear-app tokens (design-systems/linear-app/
design-tokens.json). Re-run to regenerate: python gen_design.py
"""
import json
import os

# linear-app tokens
BG = "#08090a"          # --bg
SURFACE = "#191a1b"     # --surface
FG = "#f7f8f8"          # --fg
MUTED = "#8a8f98"       # --muted
META = "#62666d"        # --meta
ACCENT = "#5e6ad2"      # --accent
ACCENT_HOVER = "#828fff"  # --accent-hover (op glyphs, brighter on dark)
WHITE = "#ffffff"       # --accent-on
BORDER = (1, 1, 1, 0.08)       # --border
BORDER_SOFT = (1, 1, 1, 0.05)  # --border-soft
RADIUS = 12             # --radius-lg
FONT = "Segoe UI"       # in --font-body stack, present on Windows

W, H = 420, 900
GUTTER = 24             # --space-6
GAP = 12                # --space-3
COLS = 4
KEY_W = (W - 2 * GUTTER - (COLS - 1) * GAP) // COLS  # 84
KEY_H = 72
ROWS = 5
GRID_H = ROWS * KEY_H + (ROWS - 1) * GAP             # 408
GRID_Y = H - 32 - GRID_H                             # bottom pad --space-8

_next_id = [1]

def nid():
    _next_id[0] += 1
    return f"10:{_next_id[0]}"

def hex_rgba(h, a=1.0):
    h = h.lstrip("#")
    return {"r": int(h[0:2], 16) / 255, "g": int(h[2:4], 16) / 255,
            "b": int(h[4:6], 16) / 255, "a": a}

def solid(c):
    if isinstance(c, tuple):
        return {"type": "SOLID", "color": {"r": c[0], "g": c[1], "b": c[2], "a": c[3]}}
    return {"type": "SOLID", "color": hex_rgba(c)}

def xf(x, y):
    return [[1, 0, x], [0, 1, y]]

def text(name, x, y, w, h, chars, size, color, weight=400, align="LEFT",
         valign="TOP", tracking=0):
    return {"id": nid(), "name": name, "type": "TEXT",
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "characters": chars, "fills": [solid(color)],
            "style": {"fontFamily": FONT, "fontWeight": weight, "fontSize": size,
                      "textAlignHorizontal": align, "textAlignVertical": valign,
                      "letterSpacing": tracking}}

def key(name, label, x, y, w, fill, fg):
    return {"id": nid(), "name": name, "type": "FRAME",
            "size": {"x": w, "y": KEY_H}, "relativeTransform": xf(x, y),
            "clipsContent": False, "cornerRadius": RADIUS,
            "fills": [solid(fill)], "strokes": [solid(BORDER_SOFT)],
            "strokeWeight": 1,
            "children": [text(f"{name} Label", 0, 0, w, KEY_H, label, 24, fg,
                              align="CENTER", valign="CENTER")]}

DIGIT = (SURFACE, FG)
FN = (SURFACE, MUTED)
OP = (SURFACE, ACCENT_HOVER)
EQ = (ACCENT, WHITE)

rows = [
    [("KeyClear", "C", FN), ("KeySign", "±", FN), ("KeyPct", "%", FN), ("KeyDiv", "÷", OP)],
    [("Key7", "7", DIGIT), ("Key8", "8", DIGIT), ("Key9", "9", DIGIT), ("KeyMul", "×", OP)],
    [("Key4", "4", DIGIT), ("Key5", "5", DIGIT), ("Key6", "6", DIGIT), ("KeySub", "-", OP)],
    [("Key1", "1", DIGIT), ("Key2", "2", DIGIT), ("Key3", "3", DIGIT), ("KeyAdd", "+", OP)],
    [("Key0", "0", DIGIT, 2), ("KeyDot", ".", DIGIT), ("KeyEq", "=", EQ)],
]

children = [
    text("Wordmark", GUTTER, 36, 200, 20, "Calculator", 14, META),
    text("Expression", GUTTER, 292, W - 2 * GUTTER, 28, "", 20, MUTED, align="RIGHT"),
    text("Result", GUTTER, 328, W - 2 * GUTTER, 92, "0", 72, FG, align="RIGHT",
         tracking=-1.6),
    {"id": nid(), "name": "Divider", "type": "RECTANGLE",
     "size": {"x": W - 2 * GUTTER, "y": 1},
     "relativeTransform": xf(GUTTER, GRID_Y - 16),
     "fills": [solid(BORDER)]},
]

y = GRID_Y
for row in rows:
    x = GUTTER
    for cell in row:
        name, label, kind = cell[0], cell[1], cell[2]
        span = cell[3] if len(cell) > 3 else 1
        w = KEY_W * span + GAP * (span - 1)
        children.append(key(name, label, x, y, w, kind[0], kind[1]))
        x += w + GAP
    y += KEY_H + GAP

doc = {
    "name": "Calculator",
    "document": {"id": "10:0", "name": "Document", "type": "DOCUMENT", "children": [
        {"id": "10:1", "name": "Page 1", "type": "CANVAS", "children": [
            {"id": nid(), "name": "Calculator", "type": "FRAME",
             "size": {"x": W, "y": H}, "relativeTransform": xf(0, 0),
             "clipsContent": True, "fills": [solid(BG)], "children": children}]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
