#!/usr/bin/env python3
"""Generate design.json for the weather benchmark app.

Single 420x900 frame, coinbase tokens (design-systems/coinbase/
design-tokens.json). Condition "icon" is a flat accent-tint badge with a
short text label -- no fake photos, one accent locked page-wide.
Re-run to regenerate: python gen_design.py
"""
import json
import os

# coinbase tokens
BG = "#ffffff"          # --bg
SURFACE = "#eef0f3"     # --surface
FG = "#0a0b0d"          # --fg
MUTED = "#5b616e"       # --muted (6.2:1 on white, AA)
ACCENT = "#0052ff"      # --accent (the one accent)
BORDER = (91 / 255, 97 / 255, 110 / 255, 0.2)  # --border
RADIUS = 16             # --radius-md, the one radius
RADIUS_SM = 8           # --radius-sm (badge only)
FONT = "Segoe UI"       # in --font-body stack, present on Windows
# type scale used: 12 / 14 / 16 / 18 / 32 / 80 (coinbase --text-*)

W, H = 420, 900
GUTTER = 24
CW = W - 2 * GUTTER      # 372 content width

_next_id = [1]

def nid():
    _next_id[0] += 1
    return f"30:{_next_id[0]}"

def hex_rgba(h, a=1.0):
    h = h.lstrip("#")
    return {"r": int(h[0:2], 16) / 255, "g": int(h[2:4], 16) / 255,
            "b": int(h[4:6], 16) / 255, "a": a}

def mix_white(h, t):
    h = h.lstrip("#")
    c = [int(h[i:i + 2], 16) for i in (0, 2, 4)]
    c = [round(v + (255 - v) * t) for v in c]
    return "#%02x%02x%02x" % tuple(c)

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

def frame(name, x, y, w, h, fill=None, radius=0, stroke=None, stroke_w=1,
          children=None, clips=False, extra=None):
    n = {"id": nid(), "name": name, "type": "FRAME",
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "clipsContent": clips, "fills": [solid(fill)] if fill else [],
         "children": children or []}
    if radius:
        n["cornerRadius"] = radius
    if stroke:
        n["strokes"] = [solid(stroke)]
        n["strokeWeight"] = stroke_w
    if extra:
        n.update(extra)
    return n

def rect(name, x, y, w, h, fill):
    return {"id": nid(), "name": name, "type": "RECTANGLE",
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "fills": [solid(fill)]}

BADGE_BG = mix_white(ACCENT, 0.88)   # soft accent tint for the condition badge

# ------------------------------------------------------------- forecast row
ROW_H = 60
forecast_row = frame("Forecast Row", 0, 0, CW, ROW_H, children=[
    text("Row Day", 0, 18, 90, 24, "Mon", 16, FG, weight=600),
    frame("Cond Badge", 104, 13, 92, 34, fill=BADGE_BG, radius=RADIUS_SM,
          children=[
              # 12px/600 accent on the 0.88 tint: ~5.4:1
              text("Cond Label", 0, 0, 92, 34, "Cloudy", 12, ACCENT, weight=600,
                   align="CENTER", valign="CENTER")]),
    text("Row High", CW - 130, 18, 60, 24, "29°", 16, FG, weight=600,
         align="RIGHT"),
    text("Row Low", CW - 60, 18, 60, 24, "22°", 16, MUTED, align="RIGHT"),
])

forecast_list = frame(
    "Forecast List", GUTTER, 396, CW, 5 * ROW_H + 4 * 12,
    children=[forecast_row],
    extra={"layoutMode": "VERTICAL", "primaryAxisSizingMode": "AUTO",
           "counterAxisSizingMode": "FIXED", "itemSpacing": 12})

# ---------------------------------------------------------------- the page
weather = frame("Weather", 0, 0, W, H, fill=BG, clips=True, children=[
    text("City Name", GUTTER, 64, 260, 40, "Shanghai", 32, FG, weight=700,
         tracking=-0.5),
    frame("Unit Toggle", W - GUTTER - 64, 64, 64, 36, fill=BG, stroke=BORDER,
          radius=RADIUS, children=[
              text("Unit Label", 0, 0, 64, 36, "°F", 14, FG, weight=600,
                   align="CENTER", valign="CENTER")]),
    text("Current Temp", GUTTER, 116, CW, 96, "26°", 80, FG, weight=600,
         tracking=-1.6),
    text("Current Desc", GUTTER, 222, CW, 26, "Partly cloudy", 18, MUTED),
    text("Today High", GUTTER, 258, 100, 24, "H 29°", 16, FG, weight=600),
    text("Today Low", GUTTER + 92, 258, 100, 24, "L 22°", 16, MUTED),
    rect("Header Divider", GUTTER, 318, CW, 1, BORDER),
    text("Forecast Title", GUTTER, 350, 240, 22, "5-day forecast", 14, FG,
         weight=600),
    forecast_list,
    text("Updated Label", GUTTER, 846, CW, 18, "Updated: sample data", 12,
         MUTED, align="CENTER"),
])

doc = {
    "name": "Weather",
    "document": {"id": "30:0", "name": "Document", "type": "DOCUMENT", "children": [
        {"id": "30:1", "name": "Page 1", "type": "CANVAS",
         "children": [weather]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
