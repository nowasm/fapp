#!/usr/bin/env python3
"""Generate design.json for the habits benchmark app.

Single 420x900 screen, duolingo tokens (design-systems/duolingo/
design-tokens.json). Header (date + progress), 5-segment progress bar,
vertical auto-layout list with one HabitRow template (cloned by
ui.bindList), footer badge card. Re-run: python gen_design.py
"""
import json
import os

# duolingo tokens
BG = "#ffffff"            # --bg
SURFACE = "#f7f7f7"       # --surface
FG = "#3c3c3c"            # --fg
MUTED = "#777777"         # --muted
BORDER = "#e5e5e5"        # --border
ACCENT = "#58cc02"        # --accent
ACCENT_ACTIVE = "#58a700" # --accent-active (text-safe green, >= 3:1 on white)
ACCENT_ON = "#ffffff"     # --accent-on
WARN = "#ffc800"          # --warn
DANGER = "#ff4b4b"        # --danger
INFO = "#1cb0f6"          # --focus-ring base (system blue)
R_SM = 12                 # --radius-sm
R_MD = 16                 # --radius-md
FONT = "Segoe UI"         # present on Windows; CJK falls back per-glyph

W, H = 420, 900
GUTTER = 24               # --space-6
CONTENT_W = W - 2 * GUTTER  # 372

_next_id = [1]

def nid():
    _next_id[0] += 1
    return f"20:{_next_id[0]}"

def hex_rgba(h, a=1.0):
    h = h.lstrip("#")
    return {"r": int(h[0:2], 16) / 255, "g": int(h[2:4], 16) / 255,
            "b": int(h[4:6], 16) / 255, "a": a}

def solid(c, a=1.0):
    return {"type": "SOLID", "color": hex_rgba(c, a)}

def xf(x, y):
    return [[1, 0, x], [0, 1, y]]

def text(name, x, y, w, h, chars, size, color, weight=400, align="LEFT",
         valign="TOP", tracking=0, visible=True):
    return {"id": nid(), "name": name, "type": "TEXT", "visible": visible,
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "characters": chars, "fills": [solid(color)],
            "style": {"fontFamily": FONT, "fontWeight": weight, "fontSize": size,
                      "textAlignHorizontal": align, "textAlignVertical": valign,
                      "letterSpacing": tracking}}

def frame(name, x, y, w, h, fill=None, radius=0, stroke=None, stroke_w=0,
          children=None, visible=True, extra=None):
    n = {"id": nid(), "name": name, "type": "FRAME", "visible": visible,
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "clipsContent": False, "cornerRadius": radius,
         "fills": [solid(fill)] if fill else [],
         "children": children or []}
    if stroke:
        n["strokes"] = [solid(stroke)]
        n["strokeWeight"] = stroke_w
    if extra:
        n.update(extra)
    return n

def rect(name, x, y, w, h, fill, radius=0, visible=True):
    return {"id": nid(), "name": name, "type": "RECTANGLE", "visible": visible,
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "cornerRadius": radius, "fills": [solid(fill)]}

def ellipse(name, x, y, d, fill=None, stroke=None, stroke_w=0, visible=True):
    n = {"id": nid(), "name": name, "type": "ELLIPSE", "visible": visible,
         "size": {"x": d, "y": d}, "relativeTransform": xf(x, y),
         "fills": [solid(fill)] if fill else []}
    if stroke:
        n["strokes"] = [solid(stroke)]
        n["strokeWeight"] = stroke_w
    return n

# ---- header ----
children = [
    text("Kicker", GUTTER, 52, 200, 18, "今日习惯", 13, MUTED, weight=700,
         tracking=2),
    text("DateLabel", GUTTER, 76, 260, 38, "7月2日 周四", 28, FG, weight=700),
    text("ProgressCaption", GUTTER, 52, CONTENT_W, 18, "已完成", 13, MUTED,
         align="RIGHT"),
    text("ProgressLabel", GUTTER, 76, CONTENT_W, 38, "0/5", 28, ACCENT_ACTIVE,
         weight=700, align="RIGHT"),
]

# ---- 5-segment progress bar (SegFill toggled by JS) ----
SEG_GAP = 8
SEG_W = (CONTENT_W - 4 * SEG_GAP) // 5  # 68
SEG_Y = 138
for i in range(5):
    x = GUTTER + i * (SEG_W + SEG_GAP)
    children.append(rect(f"SegBase{i}", x, SEG_Y, SEG_W, 12, BORDER, radius=6))
    children.append(rect(f"SegFill{i}", x, SEG_Y, SEG_W, 12, ACCENT, radius=6,
                         visible=False))

# ---- habit list: vertical auto-layout container + one row template ----
ROW_H = 84
ICONS = [  # (glyph, block fill, glyph color) — system palette only
    ("水", INFO, ACCENT_ON),
    ("书", WARN, FG),
    ("跑", DANGER, ACCENT_ON),
    ("眠", ACCENT_ACTIVE, ACCENT_ON),
    ("禅", ACCENT, ACCENT_ON),
]

row_children = []
for i, (glyph, fill, fg) in enumerate(ICONS):
    row_children.append(frame(
        f"Icon{i}", 18, 20, 44, 44, fill=fill, radius=R_SM + 2, visible=(i == 0),
        children=[text(f"Icon{i} Glyph", 0, 0, 44, 44, glyph, 18, fg, weight=700,
                       align="CENTER", valign="CENTER")]))
row_children += [
    text("HabitName", 78, 19, 190, 24, "习惯", 17, FG, weight=700),
    text("StreakLabel", 78, 46, 220, 18, "连续 0 天", 13, MUTED),
    ellipse("CheckOff", 322, 27, 30, fill=BG, stroke=BORDER, stroke_w=2),
    frame("CheckOn", 322, 27, 30, 30, fill=ACCENT, radius=15, visible=False,
          children=[text("CheckOn Glyph", 0, 0, 30, 30, "✓", 16, ACCENT_ON,
                         weight=700, align="CENTER", valign="CENTER")]),
]

habit_row = frame("HabitRow", 0, 0, CONTENT_W, ROW_H, fill=SURFACE, radius=R_MD,
                  stroke=BORDER, stroke_w=2, children=row_children)

habit_list = frame(
    "HabitList", GUTTER, 176, CONTENT_W, 5 * ROW_H + 4 * 12, children=[habit_row],
    extra={"layoutMode": "VERTICAL", "itemSpacing": 12,
           "primaryAxisSizingMode": "AUTO", "counterAxisSizingMode": "FIXED",
           "paddingLeft": 0, "paddingRight": 0, "paddingTop": 0,
           "paddingBottom": 0})
children.append(habit_list)

# ---- footer badge card (lights up at 5/5) ----
children.append(frame(
    "BadgeCard", GUTTER, 688, CONTENT_W, ROW_H, fill=SURFACE, radius=R_MD,
    stroke=BORDER, stroke_w=2, children=[
        ellipse("BadgeOff", 20, 22, 40, fill=BG, stroke=BORDER, stroke_w=2),
        frame("BadgeOn", 20, 22, 40, 40, fill=WARN, radius=20, visible=False,
              children=[text("BadgeOn Glyph", 0, 0, 40, 40, "✓", 20, FG,
                             weight=700, align="CENTER", valign="CENTER")]),
        text("BadgeTitle", 80, 19, 200, 24, "今日全勤", 16, FG, weight=700),
        text("BadgeHint", 80, 46, 260, 18, "完成全部 5 项习惯点亮徽章", 13, MUTED),
    ]))

doc = {
    "name": "Habits",
    "document": {"id": "20:0", "name": "Document", "type": "DOCUMENT", "children": [
        {"id": "20:1", "name": "Page 1", "type": "CANVAS", "children": [
            frame("Habits", 0, 0, W, H, fill=BG,
                  children=children, extra={"clipsContent": True})]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
