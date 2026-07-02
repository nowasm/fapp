#!/usr/bin/env python3
"""Generate design.json for the news benchmark app.

Two top-level frames ("Feed" story list, "Article" reading page), notion
tokens (design-systems/notion/design-tokens.json). Notion restraint: white
page, one accent (#0075de) reserved for the category tag, hairline dividers
instead of cards, one radius (4px), type on the 12/14/16/26 scale.
Re-run to regenerate: python gen_design.py
"""
import json
import os

# notion tokens
BG = "#ffffff"            # --bg
FG = "#000000"            # --fg is rgba(0,0,0,0.95); emitted with a=0.95
FG_A = 0.95
FG2 = "#31302e"           # --fg-2 (body copy)
MUTED = "#615d59"         # --muted (7.0:1 on white, AA)
META = "#a39e98"          # --meta (decorative only)
BORDER_A = 0.10           # --border rgba(0,0,0,.1) hairlines
ACCENT = "#0075de"        # --accent (the one accent: category tags)
RADIUS = 4                # --radius-sm, the one radius
FONT = "Segoe UI"         # Windows system font (in the --font-body spirit)
# type scale: 12 / 14 / 16 / 26 (notion --text-xs/sm/base/xl)

W, H = 420, 900
GUTTER = 24
CW = W - 2 * GUTTER       # 372 content width

_next_id = [1]

def nid():
    _next_id[0] += 1
    return f"30:{_next_id[0]}"

def hex_rgba(h, a=1.0):
    h = h.lstrip("#")
    return {"r": int(h[0:2], 16) / 255, "g": int(h[2:4], 16) / 255,
            "b": int(h[4:6], 16) / 255, "a": a}

def solid(c, a=1.0):
    return {"type": "SOLID", "color": hex_rgba(c, a)}

def xf(x, y):
    return [[1, 0, x], [0, 1, y]]

def text(name, x, y, w, h, chars, size, color, weight=400, align="LEFT",
         valign="TOP", tracking=0, alpha=1.0, line_h=0):
    style = {"fontFamily": FONT, "fontWeight": weight, "fontSize": size,
             "textAlignHorizontal": align, "textAlignVertical": valign,
             "letterSpacing": tracking}
    if line_h:
        style["lineHeightPx"] = line_h
    return {"id": nid(), "name": name, "type": "TEXT",
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "characters": chars, "fills": [solid(color, alpha)],
            "style": style}

def frame(name, x, y, w, h, fill=None, radius=0, stroke=None, stroke_a=1.0,
          stroke_w=1, children=None, clips=False, extra=None):
    n = {"id": nid(), "name": name, "type": "FRAME",
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "clipsContent": clips, "fills": [solid(fill)] if fill else [],
         "children": children or []}
    if radius:
        n["cornerRadius"] = radius
    if stroke:
        n["strokes"] = [solid(stroke, stroke_a)]
        n["strokeWeight"] = stroke_w
    if extra:
        n.update(extra)
    return n

def hairline(name, x, y, w):
    return {"id": nid(), "name": name, "type": "RECTANGLE",
            "size": {"x": w, "y": 1}, "relativeTransform": xf(x, y),
            "fills": [solid("#000000", BORDER_A)]}

# ----------------------------------------------------------------- Feed page
ROW_H = 124
story_row = frame("Story Row", 0, 0, CW, ROW_H, children=[
    text("Story Tag", 0, 18, 220, 16, "DEV TOOLS", 12, ACCENT, weight=600,
         tracking=0.8),
    text("Story Title", 0, 40, CW, 46,
         "Torch build cache cuts cold CI runs to under four minutes",
         16, FG, weight=600, alpha=FG_A, line_h=22),
    text("Story Meta", 0, 94, CW, 16, "The Build Log · 1h ago", 12, MUTED),
    hairline("Story Divider", 0, ROW_H - 1, CW),
])

LIST_Y = 144
LIST_H = 720            # 6 rows x 124 = 744 > 720: vertical scroll kicks in
story_list = frame(
    "Story List", GUTTER, LIST_Y, CW, LIST_H, clips=True,
    children=[story_row],
    extra={"layoutMode": "VERTICAL", "primaryAxisSizingMode": "FIXED",
           "counterAxisSizingMode": "FIXED", "itemSpacing": 0,
           "overflowDirection": "VERTICAL_SCROLLING"})

feed = frame("Feed", 0, 0, W, H, fill=BG, clips=True, children=[
    text("Masthead", GUTTER, 60, 260, 34, "Dispatch", 26, FG, weight=700,
         tracking=-0.8, alpha=FG_A),
    text("Feed Date", GUTTER, 100, 300, 16, "WEDNESDAY, JULY 2", 12, MUTED,
         weight=600, tracking=0.8),
    hairline("Header Divider", GUTTER, 131, CW),
    story_list,
])

# -------------------------------------------------------------- Article page
PARA_Y0 = 316
PARA_H = 100            # 4 lines at 16px / 24px leading
PARA_GAP = 20
paras = [
    text(f"Article Para {i + 1}", GUTTER, PARA_Y0 + i * (PARA_H + PARA_GAP),
         CW, PARA_H, "", 16, FG2, line_h=24)
    for i in range(4)
]

article = frame("Article", 520, 0, W, H, fill=BG, clips=True, children=[
    frame("Back Button", GUTTER, 52, 36, 36, fill=BG, stroke="#000000",
          stroke_a=BORDER_A, radius=RADIUS, children=[
              text("Back Glyph", 0, -2, 36, 36, "‹", 20, FG,
                   align="CENTER", valign="CENTER", alpha=FG_A)]),
    text("Article Tag", GUTTER, 120, 220, 16, "DEV TOOLS", 12, ACCENT,
         weight=600, tracking=0.8),
    text("Article Title", GUTTER, 144, CW, 100,
         "Torch build cache cuts cold CI runs to under four minutes",
         26, FG, weight=700, tracking=-0.8, alpha=FG_A, line_h=32),
    text("Article Byline", GUTTER, 258, CW, 20, "The Build Log · 1h ago",
         14, MUTED),
    hairline("Article Divider", GUTTER, 292, CW),
] + paras)

doc = {
    "name": "News",
    "document": {"id": "30:0", "name": "Document", "type": "DOCUMENT",
                 "children": [
        {"id": "30:1", "name": "Page 1", "type": "CANVAS",
         "children": [feed, article]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
