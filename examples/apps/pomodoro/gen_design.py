#!/usr/bin/env python3
"""Generate design.json for the pomodoro benchmark app.

Single 420x900 screen, spotify tokens (design-systems/spotify/design-tokens.json).
Re-run to regenerate: python gen_design.py
"""
import json
import os

# spotify tokens
BG = "#121212"          # --bg
SURFACE = "#181818"     # --surface
FG = "#ffffff"          # --fg
MUTED = "#b3b3b3"       # --muted
ACCENT = "#1ed760"      # --accent
ACCENT_ON = "#000000"   # --accent-on
BORDER_SOFT = (1, 1, 1, 0.1)  # --border-soft
RADIUS_PILL = 999       # --radius-pill
FONT = "Segoe UI"       # Windows-present stand-in for --font-body stack

W, H = 420, 900
GUTTER = 24             # --space-6

_next_id = [1]

def nid():
    _next_id[0] += 1
    return f"20:{_next_id[0]}"

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

def pill(name, x, y, w, h, fill, stroke=None, children=None):
    node = {"id": nid(), "name": name, "type": "FRAME",
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "clipsContent": False, "cornerRadius": RADIUS_PILL,
            "fills": [solid(fill)], "children": children or []}
    if stroke is not None:
        node["strokes"] = [solid(stroke)]
        node["strokeWeight"] = 1
    return node

# ---- mode pills: On/Off variants stacked, JS toggles setVisible ----
PILL_W, PILL_H, PILL_GAP = 110, 40, 12
PILLS_X = (W - (2 * PILL_W + PILL_GAP)) // 2
PILLS_Y = 208

def mode_pill(name, label, x):
    on = pill(f"{name}On", 0, 0, PILL_W, PILL_H, ACCENT, children=[
        text(f"{name}OnLabel", 0, 0, PILL_W, PILL_H, label, 16, ACCENT_ON,
             weight=700, align="CENTER", valign="CENTER")])
    off = pill(f"{name}Off", 0, 0, PILL_W, PILL_H, SURFACE, stroke=BORDER_SOFT,
               children=[text(f"{name}OffLabel", 0, 0, PILL_W, PILL_H, label, 16,
                              MUTED, weight=600, align="CENTER", valign="CENTER")])
    return {"id": nid(), "name": name, "type": "FRAME",
            "size": {"x": PILL_W, "y": PILL_H}, "relativeTransform": xf(x, PILLS_Y),
            "clipsContent": False, "fills": [], "children": [off, on]}

# ---- round dots ----
DOT, DOT_GAP = 12, 16
DOTS_W = 4 * DOT + 3 * DOT_GAP
DOTS_X = (W - DOTS_W) // 2
DOTS_Y = 502

def dot(i):
    return {"id": nid(), "name": f"Dot{i}", "type": "RECTANGLE",
            "size": {"x": DOT, "y": DOT},
            "relativeTransform": xf(DOTS_X + (i - 1) * (DOT + DOT_GAP), DOTS_Y),
            "cornerRadius": RADIUS_PILL, "fills": [solid(ACCENT)]}

BTN_W = W - 2 * GUTTER

children = [
    text("Wordmark", GUTTER, 40, W - 2 * GUTTER, 16, "POMODORO", 12, MUTED,
         weight=600, tracking=2, align="CENTER"),
    mode_pill("PillFocus", "Focus", PILLS_X),
    mode_pill("PillBreak", "Break", PILLS_X + PILL_W + PILL_GAP),
    text("Timer", 0, 330, W, 120, "25:00", 88, FG, weight=700,
         align="CENTER", valign="CENTER", tracking=-2),
    text("RoundsLabel", 0, 470, W, 18, "Rounds", 12, MUTED, weight=600,
         tracking=1.5, align="CENTER"),
    dot(1), dot(2), dot(3), dot(4),
    pill("BtnStart", GUTTER, 636, BTN_W, 56, ACCENT, children=[
        text("BtnStartLabel", 0, 0, BTN_W, 56, "Start", 18, ACCENT_ON,
             weight=700, align="CENTER", valign="CENTER")]),
    pill("BtnReset", GUTTER, 708, BTN_W, 56, SURFACE, stroke=BORDER_SOFT, children=[
        text("BtnResetLabel", 0, 0, BTN_W, 56, "Reset", 16, FG,
             weight=600, align="CENTER", valign="CENTER")]),
]

doc = {
    "name": "Pomodoro",
    "document": {"id": "20:0", "name": "Document", "type": "DOCUMENT", "children": [
        {"id": "20:1", "name": "Page 1", "type": "CANVAS", "children": [
            {"id": nid(), "name": "Pomodoro", "type": "FRAME",
             "size": {"x": W, "y": H}, "relativeTransform": xf(0, 0),
             "clipsContent": True, "fills": [solid(BG)], "children": children}]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
