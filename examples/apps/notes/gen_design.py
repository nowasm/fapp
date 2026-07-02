#!/usr/bin/env python3
"""Generate design.json for the notes benchmark app.

Two top-level frames ("Notes" list, "Editor" editing page), notion tokens
(design-systems/notion/design-tokens.json). Notion restraint: white page,
hairline dividers instead of cards, one accent (#0075de) reserved for the
new-note action, one radius (4px), type on the 12/14/16/26 scale.
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
BORDER_A = 0.10           # --border rgba(0,0,0,.1) hairlines
ACCENT = "#0075de"        # --accent (the one accent: the new-note button)
DANGER = "#dc2626"        # --danger (delete)
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

# ---------------------------------------------------------------- Notes page
ROW_H = 76
# NoteRow needs the BG fill: an unfilled frame is not a hit target in the
# gaps between its texts, so taps at the row center would fall through.
note_row = frame("NoteRow", 0, 0, CW, ROW_H, fill=BG, children=[
    text("NoteTitle", 0, 14, 264, 22, "周末采购清单", 16, FG, weight=600,
         alpha=FG_A),
    text("NoteDate", 276, 18, 96, 16, "6月28日", 12, MUTED, align="RIGHT"),
    text("NotePreview", 0, 42, CW, 20, "牛奶两盒、鸡蛋一打", 14, MUTED),
    hairline("NoteDivider", 0, ROW_H - 1, CW),
])

LIST_Y = 152
LIST_H = 724              # scrolls once the notes outgrow it
note_list = frame(
    "NoteList", GUTTER, LIST_Y, CW, LIST_H, clips=True,
    children=[note_row],
    extra={"layoutMode": "VERTICAL", "primaryAxisSizingMode": "FIXED",
           "counterAxisSizingMode": "FIXED", "itemSpacing": 0,
           "overflowDirection": "VERTICAL_SCROLLING"})

notes = frame("Notes", 0, 0, W, H, fill=BG, clips=True, children=[
    text("Title", GUTTER, 56, 200, 34, "笔记", 26, FG, weight=700,
         tracking=-0.8, alpha=FG_A),
    text("NoteCount", GUTTER, 96, 200, 16, "3 条笔记", 12, MUTED),
    frame("NewBtn", 304, 56, 92, 36, fill=BG, stroke="#000000",
          stroke_a=BORDER_A, radius=RADIUS, children=[
              text("NewBtn Label", 0, 0, 92, 36, "＋ 新建", 14, ACCENT,
                   weight=600, align="CENTER", valign="CENTER")]),
    hairline("Header Divider", GUTTER, 131, CW),
    note_list,
])

# --------------------------------------------------------------- Editor page
editor = frame("Editor", 520, 0, W, H, fill=BG, clips=True, children=[
    frame("BackBtn", GUTTER, 52, 36, 36, fill=BG, stroke="#000000",
          stroke_a=BORDER_A, radius=RADIUS, children=[
              text("Back Glyph", 0, -2, 36, 36, "‹", 20, FG,
                   align="CENTER", valign="CENTER", alpha=FG_A)]),
    frame("DeleteBtn", 330, 52, 66, 36, fill=BG, stroke="#000000",
          stroke_a=BORDER_A, radius=RADIUS, children=[
              text("DeleteBtn Label", 0, 0, 66, 36, "删除", 14, DANGER,
                   weight=600, align="CENTER", valign="CENTER")]),
    text("EditorTitle", GUTTER, 116, CW, 36, "标题", 26, FG, weight=700,
         tracking=-0.8, alpha=FG_A),
    hairline("Title Divider", GUTTER, 166, CW),
    # the editable body: 26 lines at 24px leading
    text("EditorBody", GUTTER, 186, CW, 624, "", 16, FG2, line_h=24),
    hairline("Footer Divider", GUTTER, 828, CW),
    text("CharCount", GUTTER, 844, CW, 16, "0 字", 12, MUTED, align="RIGHT"),
])

doc = {
    "name": "Notes",
    "document": {"id": "30:0", "name": "Document", "type": "DOCUMENT",
                 "children": [
        {"id": "30:1", "name": "Page 1", "type": "CANVAS",
         "children": [notes, editor]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
