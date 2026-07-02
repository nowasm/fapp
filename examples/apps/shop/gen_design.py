#!/usr/bin/env python3
"""Generate design.json for the shop benchmark app.

Two top-level frames ("Shop" list page, "Detail" product page), airbnb tokens
(design-systems/airbnb/design-tokens.json). Product art is a flat tint block
per product (soft mixes of the airbnb palette) with a deeper inner block --
no fake photos. Re-run to regenerate: python gen_design.py
"""
import json
import os

# airbnb tokens
BG = "#ffffff"          # --bg
FG = "#222222"          # --fg
MUTED = "#6a6a6a"       # --muted    (4.8:1 on white, AA for body text)
META = "#929292"        # --meta     (decorative only)
BORDER = "#dddddd"      # --border
BORDER_SOFT = "#ebebeb"  # --border-soft
ACCENT = "#ff385c"      # --accent   (the one accent, locked page-wide)
WHITE = "#ffffff"       # --accent-on
SUCCESS = "#008a05"
WARN = "#c47700"
DANGER = "#c13515"
RADIUS = 14             # --radius-md, the one radius
FONT = "Segoe UI"       # Windows system font (in the --font-body spirit)
# type scale: 12 / 14 / 16 / 20 / 22 / 28 (airbnb --text-*)

W, H = 420, 900
GUTTER = 24
CW = W - 2 * GUTTER      # 372 content width

_next_id = [1]

def nid():
    _next_id[0] += 1
    return f"20:{_next_id[0]}"

def hex_rgba(h, a=1.0):
    h = h.lstrip("#")
    return {"r": int(h[0:2], 16) / 255, "g": int(h[2:4], 16) / 255,
            "b": int(h[4:6], 16) / 255, "a": a}

def mix_white(h, t):
    """Mix color toward white by t (0..1). Returns hex."""
    h = h.lstrip("#")
    c = [int(h[i:i + 2], 16) for i in (0, 2, 4)]
    c = [round(v + (255 - v) * t) for v in c]
    return "#%02x%02x%02x" % tuple(c)

def solid(c, a=1.0):
    return {"type": "SOLID", "color": hex_rgba(c, a)}

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

def rect(name, x, y, w, h, fill, radius=0):
    n = {"id": nid(), "name": name, "type": "RECTANGLE",
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "fills": [solid(fill)]}
    if radius:
        n["cornerRadius"] = radius
    return n

# ---- products: base palette color -> soft tint (block) + deeper tint (inset)
PRODUCTS = [
    ("Linen Throw Pillow", ACCENT),
    ("Oak Side Table", WARN),
    ("Ceramic Table Lamp", SUCCESS),
    ("Wool Area Rug", FG),
    ("Stoneware Vase Set", DANGER),
]

def tint_stack(prefix, w, h, inset_ratio=0.42):
    """5 overlapping tint blocks (one visible at a time, JS toggles)."""
    out = []
    iw, ih = round(w * inset_ratio), round(h * inset_ratio)
    ix, iy = (w - iw) // 2, (h - ih) // 2
    for i, (_, base) in enumerate(PRODUCTS):
        out.append(frame(
            f"{prefix} {i + 1}", 0, 0, w, h, fill=mix_white(base, 0.82),
            radius=RADIUS, clips=True,
            children=[rect(f"{prefix} {i + 1} Inset", ix, iy, iw, ih,
                           mix_white(base, 0.55), radius=RADIUS // 2)]))
    return out

# ---------------------------------------------------------------- Shop page
ROW_H = 88
row_children = tint_stack("Item Tint", ROW_H, ROW_H)
row_children += [
    text("Item Name", ROW_H + 20, 20, CW - ROW_H - 60, 24,
         "Linen Throw Pillow", 16, FG, weight=600),
    text("Item Price", ROW_H + 20, 46, 120, 20, "$38", 14, MUTED, weight=600),
    text("Item Chevron", CW - 24, 32, 20, 24, "›", 20, META,
         align="CENTER"),
]
product_row = frame("Product Row", 0, 0, CW, ROW_H, children=row_children)

product_list = frame(
    "Product List", GUTTER, 168, CW, 5 * ROW_H + 4 * 16,
    children=[product_row],
    extra={"layoutMode": "VERTICAL", "primaryAxisSizingMode": "AUTO",
           "counterAxisSizingMode": "FIXED", "itemSpacing": 16})

# cart glyph: stroke-only bag body + handle loop, badge pinned top-right
cart = frame("Cart", W - GUTTER - 44, 56, 44, 44, children=[
    frame("Cart Handle", 15, 4, 14, 14, stroke=FG, stroke_w=2, radius=7),
    frame("Cart Body", 9, 11, 26, 24, fill=BG, stroke=FG, stroke_w=2, radius=5),
    frame("Cart Badge", 26, 0, 18, 18, fill=ACCENT, radius=9, children=[
        text("Cart Badge Count", 0, 0, 18, 18, "0", 12, WHITE, weight=700,
             align="CENTER", valign="CENTER")]),
])

shop = frame("Shop", 0, 0, W, H, fill=BG, clips=True, children=[
    text("Shop Title", GUTTER, 60, 260, 36, "Willow Home", 28, FG, weight=700,
         tracking=-0.5),
    text("Shop Subtitle", GUTTER, 100, 300, 20, "Home goods, made to last.",
         14, MUTED),
    cart,
    rect("Header Divider", GUTTER, 140, CW, 1, BORDER_SOFT),
    product_list,
    text("Shop Footnote", GUTTER, 846, CW, 18, "Free delivery on orders over $75",
         12, META, align="CENTER"),
])

# --------------------------------------------------------------- Detail page
HERO_H = 320
stepper_y = 660
detail = frame("Detail", 520, 0, W, H, fill=BG, clips=True, children=[
    frame("Back Button", GUTTER, 56, 40, 40, fill=BG, stroke=BORDER,
          radius=20, children=[
              text("Back Glyph", 0, -2, 40, 40, "‹", 22, FG,
                   align="CENTER", valign="CENTER")]),
    frame("Detail Hero", GUTTER, 116, CW, HERO_H,
          children=tint_stack("Detail Tint", CW, HERO_H, inset_ratio=0.4)),
    text("Detail Name", GUTTER, 460, CW, 36, "Linen Throw Pillow", 28, FG,
         weight=700, tracking=-0.5),
    text("Detail Price", GUTTER, 500, 160, 26, "$38", 20, FG, weight=600),
    text("Detail Desc", GUTTER, 536, CW, 48,
         "Washed linen cover with a hidden zip. Insert included.",
         16, MUTED),
    rect("Detail Divider", GUTTER, 606, CW, 1, BORDER_SOFT),
    text("Qty Label", GUTTER, 626, 160, 20, "Quantity", 14, FG, weight=600),
    frame("Qty Minus", GUTTER, stepper_y, 44, 44, fill=BG, stroke=BORDER,
          radius=RADIUS, children=[
              text("Qty Minus Glyph", 0, -1, 44, 44, "-", 20, FG,
                   align="CENTER", valign="CENTER")]),
    text("Qty Value", GUTTER + 44, stepper_y + 10, 56, 24, "1", 20, FG,
         weight=600, align="CENTER"),
    frame("Qty Plus", GUTTER + 100, stepper_y, 44, 44, fill=BG, stroke=BORDER,
          radius=RADIUS, children=[
              text("Qty Plus Glyph", 0, -1, 44, 44, "+", 20, FG,
                   align="CENTER", valign="CENTER")]),
    frame("Add To Cart", GUTTER, 760, CW, 56, fill=ACCENT, radius=RADIUS,
          children=[
              # 20px/700 = large text; #ff385c on white is 3.5:1 (AA large)
              text("Add To Cart Label", 0, 0, CW, 56, "Add to Cart", 20,
                   WHITE, weight=700, align="CENTER", valign="CENTER")]),
])

doc = {
    "name": "Shop",
    "document": {"id": "20:0", "name": "Document", "type": "DOCUMENT", "children": [
        {"id": "20:1", "name": "Page 1", "type": "CANVAS",
         "children": [shop, detail]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
