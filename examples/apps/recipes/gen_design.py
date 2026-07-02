#!/usr/bin/env python3
"""Generate design.json for the recipes benchmark app.

Two 420x900 frames ("Recipes" list, "Recipe" detail), material tokens
(design-systems/material/design-tokens.json). Material restraint: #f8fafd
page on white cards, one accent (#1a73e8) for chips/hearts/detail button,
radii {12, pill}, type on the 12/14/16/24 scale, Segoe UI.
Recipe "images" are soft tint blocks (Google container pastels) with a
two-character dish abbreviation - no fake photos, no emoji dependency.
Re-run to regenerate: python gen_design.py
"""
import json
import os

# material tokens (design-systems/material/design-tokens.json)
BG = "#f8fafd"            # --bg
SURFACE = "#ffffff"       # --surface
FG = "#202124"            # --fg
FG2 = "#3c4043"           # --fg-2
MUTED = "#5f6368"         # --muted
BORDER = "#dadce0"        # --border
BORDER_SOFT = "#edf0f2"   # --border-soft
ACCENT = "#1a73e8"        # --accent (the one accent)
ACCENT_ON = "#ffffff"     # --accent-on
R_MD = 12                 # --radius-md (cards, thumbs)
R_PILL = 999              # --radius-pill (chips, buttons)
FONT = "Segoe UI"         # Windows system font; CJK falls back per glyph
# type scale used: 12 / 14 / 16 / 24 (--text-xs/sm/base/xl)

# soft tint palette for the recipe blocks (Google container pastels,
# --surface-warm #e8f0fe is tint 0)
TINTS = ["#e8f0fe", "#fce8e6", "#fef7e0", "#e6f4ea", "#f3e8fd"]

W, H = 420, 900
GUTTER = 20
CW = W - 2 * GUTTER       # 380 content width

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
         valign="TOP", tracking=0, visible=True, line_h=0):
    style = {"fontFamily": FONT, "fontWeight": weight, "fontSize": size,
             "textAlignHorizontal": align, "textAlignVertical": valign,
             "letterSpacing": tracking}
    if line_h:
        style["lineHeightPx"] = line_h
    return {"id": nid(), "name": name, "type": "TEXT", "visible": visible,
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "characters": chars, "fills": [solid(color)], "style": style}

def frame(name, x, y, w, h, fill=None, radius=0, stroke=None, stroke_w=1,
          children=None, visible=True, clips=False, extra=None):
    n = {"id": nid(), "name": name, "type": "FRAME", "visible": visible,
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

def rect(name, x, y, w, h, fill, radius=0, visible=True):
    n = {"id": nid(), "name": name, "type": "RECTANGLE", "visible": visible,
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "fills": [solid(fill)]}
    if radius:
        n["cornerRadius"] = radius
    return n

def ellipse(name, x, y, d, fill):
    return {"id": nid(), "name": name, "type": "ELLIPSE",
            "size": {"x": d, "y": d}, "relativeTransform": xf(x, y),
            "fills": [solid(fill)]}

def tint_stack(prefix, w, h, radius):
    """Five stacked tint rects (script shows exactly one per recipe)."""
    return [rect(f"{prefix}{i}", 0, 0, w, h, c, radius=radius, visible=(i == 0))
            for i, c in enumerate(TINTS)]

# --------------------------------------------------------------- Recipes page
CATS = ["全部", "快手", "主食", "汤"]
CHIP_W, CHIP_H, CHIP_GAP = 80, 36, 10

def chip(label, i):
    on = frame("ChipOn", 0, 0, CHIP_W, CHIP_H, fill=ACCENT, radius=R_PILL,
               visible=(i == 0), children=[
        text("ChipOnLabel", 0, 0, CHIP_W, CHIP_H, label, 14, ACCENT_ON,
             weight=600, align="CENTER", valign="CENTER")])
    off = frame("ChipOff", 0, 0, CHIP_W, CHIP_H, fill=SURFACE, radius=R_PILL,
                stroke=BORDER, visible=(i != 0), children=[
        text("ChipOffLabel", 0, 0, CHIP_W, CHIP_H, label, 14, FG2,
             weight=600, align="CENTER", valign="CENTER")])
    return frame("CatChip", i * (CHIP_W + CHIP_GAP), 0, CHIP_W, CHIP_H,
                 children=[on, off])

chip_row = frame("ChipRow", GUTTER, 108, CW, CHIP_H,
                 children=[chip(c, i) for i, c in enumerate(CATS)])

# recipe row template (cloned by ui.bindList)
# NOTE: bindList switches the container to hug sizing, so the list's real
# height is always 8*ROW_H + 7*12 = 692 - sized to fit inside the 900 page
# (164 + 692 = 856) with no clipped overhang.
ROW_H = 76
THUMB = 56
recipe_row = frame(
    "RecipeRow", 0, 0, CW, ROW_H, fill=SURFACE, radius=R_MD,
    stroke=BORDER_SOFT, children=[
        frame("RecipeThumb", 10, 10, THUMB, THUMB, clips=True, radius=R_MD,
              children=tint_stack("Tint", THUMB, THUMB, R_MD) + [
                  text("ThumbAbbr", 0, 0, THUMB, THUMB, "炒蛋", 16, FG2,
                       weight=700, align="CENTER", valign="CENTER")]),
        text("RecipeName", 84, 15, 224, 24, "番茄炒蛋", 16, FG, weight=600),
        text("RecipeMeta", 84, 42, 224, 18, "10 分钟 · 简单", 12, MUTED),
        frame("FavBtn", 326, 18, 40, 40, children=[
            text("FavOff", 0, 0, 40, 40, "♡", 20, MUTED,
                 align="CENTER", valign="CENTER"),
            text("FavOn", 0, 0, 40, 40, "♥", 20, ACCENT, visible=False,
                 align="CENTER", valign="CENTER")]),
    ])

LIST_Y = 164
LIST_H = 8 * ROW_H + 7 * 12   # 692 (matches the hugged height)
recipe_list = frame(
    "RecipeList", GUTTER, LIST_Y, CW, LIST_H, clips=True,
    children=[recipe_row],
    extra={"layoutMode": "VERTICAL", "itemSpacing": 12,
           "primaryAxisSizingMode": "FIXED", "counterAxisSizingMode": "FIXED"})

recipes_page = frame("Recipes", 0, 0, W, H, fill=BG, clips=True, children=[
    text("Title", GUTTER, 56, 220, 32, "家常菜谱", 24, FG, weight=700),
    text("FavCount", GUTTER, 66, CW, 20, "已收藏 0", 14, MUTED, align="RIGHT"),
    chip_row,
    recipe_list,
])

# ---------------------------------------------------------------- Recipe page
ingredient_row = frame("IngredientRow", 0, 0, CW, 32, children=[
    ellipse("IngDot", 2, 13, 6, ACCENT),
    text("IngName", 20, 6, 220, 20, "番茄", 14, FG2),
    text("IngAmt", 240, 6, 140, 20, "2 个", 14, MUTED, align="RIGHT"),
])

ingredient_list = frame(
    "IngredientList", GUTTER, 392, CW, 6 * 32, children=[ingredient_row],
    extra={"layoutMode": "VERTICAL", "itemSpacing": 0,
           "primaryAxisSizingMode": "AUTO", "counterAxisSizingMode": "FIXED"})

steps = [text(f"Step {i + 1}", GUTTER, 640 + i * 62, CW, 50, "", 14, FG2,
              line_h=21) for i in range(4)]

detail_fav = frame("DetailFav", 288, 284, 112, 36, children=[
    frame("DetailFavOn", 0, 0, 112, 36, fill=ACCENT, radius=R_PILL,
          visible=False, children=[
        text("DetailFavOnLabel", 0, 0, 112, 36, "♥ 已收藏", 14, ACCENT_ON,
             weight=600, align="CENTER", valign="CENTER")]),
    frame("DetailFavOff", 0, 0, 112, 36, fill=SURFACE, radius=R_PILL,
          stroke=BORDER, children=[
        text("DetailFavOffLabel", 0, 0, 112, 36, "♡ 收藏", 14, ACCENT,
             weight=600, align="CENTER", valign="CENTER")]),
])

recipe_page = frame("Recipe", 520, 0, W, H, fill=BG, clips=True, children=[
    frame("Back Button", GUTTER, 52, 36, 36, fill=SURFACE, radius=R_PILL,
          stroke=BORDER, children=[
        text("Back Glyph", 0, -2, 36, 36, "‹", 20, FG,
             align="CENTER", valign="CENTER")]),
    frame("DetailThumb", GUTTER, 104, CW, 160, clips=True, radius=R_MD,
          children=tint_stack("DetailTint", CW, 160, R_MD) + [
              text("DetailAbbr", 0, 0, CW, 160, "炒蛋", 36, FG2, weight=700,
                   align="CENTER", valign="CENTER")]),
    text("DetailName", GUTTER, 284, 260, 32, "番茄炒蛋", 24, FG, weight=700),
    detail_fav,
    text("DetailMeta", GUTTER, 326, CW, 20, "10 分钟 · 简单 · 2 人份", 14,
         MUTED),
    text("IngHeader", GUTTER, 360, 120, 22, "配料", 16, FG, weight=600),
    ingredient_list,
    text("StepHeader", GUTTER, 608, 120, 22, "步骤", 16, FG, weight=600),
] + steps)

doc = {
    "name": "Recipes",
    "document": {"id": "30:0", "name": "Document", "type": "DOCUMENT",
                 "children": [
        {"id": "30:1", "name": "Page 1", "type": "CANVAS",
         "children": [recipes_page, recipe_page]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
