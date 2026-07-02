#!/usr/bin/env python3
"""Generate design.json for the expense benchmark app.

Two top-level frames ("Ledger" list page, "Add" entry page), revolut tokens
(design-systems/revolut/design-tokens.json): light dashboard bg with one dark
hero card (fg as surface), one accent, one card radius. Category color coding
is functional (3 fixed dots), amounts use --danger / --success.
Re-run to regenerate: python gen_design.py
"""
import json
import os

# revolut tokens
BG = "#f7f8fb"           # --bg
SURFACE = "#ffffff"      # --surface
SURFACE_WARM = "#eef4ff" # --surface-warm
FG = "#111827"           # --fg (also the dark hero card fill)
FG2 = "#334155"          # --fg-2
MUTED = "#64748b"        # --muted
BORDER = "#dbe3ef"       # --border
BORDER_SOFT = "#edf2f7"  # --border-soft
ACCENT = "#0666eb"       # --accent (the one accent, locked page-wide)
WHITE = "#ffffff"        # --accent-on
SUCCESS = "#16a34a"      # --success (income)
WARN = "#f59e0b"         # --warn
DANGER = "#ef4444"       # --danger (expense)
RADIUS = 12              # --radius-sm, the one card radius
PILL = 9999              # --radius-pill (buttons/pills/fab only)
FONT = "Segoe UI"        # Windows system font (in the --font-body spirit)
# type scale: 12 / 14 / 16 / 18 / 24 / 36 / 54 (revolut --text-*)

W, H = 420, 900
GUTTER = 20              # --space-5
CW = W - 2 * GUTTER      # 380 content width

# category coding: 餐饮 / 出行 / 日用 (functional color, not accent creep)
CATS = [("餐饮", WARN), ("出行", ACCENT), ("日用", MUTED)]

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
         valign="TOP", tracking=0, alpha=1.0):
    return {"id": nid(), "name": name, "type": "TEXT",
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "characters": chars, "fills": [solid(color, alpha)],
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

# --------------------------------------------------------------- Ledger page
# dark hero card: month total, revolut premium-finance feel on light bg
hero = frame("Hero Card", GUTTER, 96, CW, 148, fill=FG, radius=RADIUS, children=[
    text("Month Label", 24, 24, 200, 20, "本月支出", 14, WHITE, alpha=0.72),
    text("Total Amount", 24, 52, CW - 48, 48, "¥0.00", 36, WHITE, weight=700,
         tracking=-1.0),
    text("Count Label", 24, 108, CW - 48, 18, "共 0 笔", 12, WHITE, alpha=0.6),
])

# category summary: 3 mini cards
CAT_W = (CW - 2 * 12) // 3  # 118
cat_cards = []
for i, (name, color) in enumerate(CATS):
    x = GUTTER + i * (CAT_W + 12)
    cat_cards.append(frame(
        f"Cat Card {i + 1}", x, 264, CAT_W, 84, fill=SURFACE,
        stroke=BORDER_SOFT, radius=RADIUS, children=[
            rect(f"Cat Dot {i + 1}", 16, 18, 8, 8, color, radius=4),
            text(f"Cat Name {i + 1}", 30, 12, CAT_W - 40, 18, name, 12, MUTED),
            text(f"Cat Sum {i + 1}", 16, 42, CAT_W - 32, 24, "¥0.00", 16, FG,
                 weight=600),
        ]))

# transaction row template (bindList): color dot x3 (JS toggles one),
# note + date, amount right-aligned (expense red / income green, JS toggles)
ROW_H = 52
row_children = []
for i, (_, color) in enumerate(CATS):
    row_children.append(rect(f"Row Dot {i + 1}", 2, 21, 10, 10, color, radius=5))
row_children += [
    text("Row Note", 26, 6, 200, 22, "备注", 16, FG),
    text("Row Date", 26, 30, 200, 16, "7月1日", 12, MUTED),
    text("Amt Exp", CW - 130, 15, 130, 22, "-0.00", 16, DANGER, weight=600,
         align="RIGHT"),
    text("Amt Inc", CW - 130, 15, 130, 22, "+0.00", 16, SUCCESS, weight=600,
         align="RIGHT"),
]
tx_row = frame("Tx Row", 0, 0, CW, ROW_H, children=row_children)

tx_list = frame(
    "Tx List", GUTTER, 400, CW, 6 * ROW_H + 5 * 8, children=[tx_row],
    extra={"layoutMode": "VERTICAL", "primaryAxisSizingMode": "AUTO",
           "counterAxisSizingMode": "FIXED", "itemSpacing": 8})

fab = frame("Fab", W - GUTTER - 56, 816, 56, 56, fill=ACCENT, radius=PILL,
            children=[text("Fab Glyph", 0, -2, 56, 56, "+", 28, WHITE,
                           align="CENTER", valign="CENTER")])

ledger = frame("Ledger", 0, 0, W, H, fill=BG, clips=True, children=[
    text("Ledger Title", GUTTER, 52, 200, 26, "支出账本", 18, FG, weight=700,
         tracking=-0.3),
    text("Ledger Sub", GUTTER + 260, 58, 100, 18, "记录每一笔", 12, MUTED,
         align="RIGHT"),
    hero,
    *cat_cards,
    text("Tx Label", GUTTER, 368, 200, 20, "流水", 14, MUTED, weight=600),
    tx_list,
    fab,
])

# ------------------------------------------------------------------ Add page
KEY_W, KEY_GAP = 118, 13   # 3*118 + 2*13 = 380
KEY_H, KEY_VGAP = 64, 12
GRID_Y = 504               # 4 rows end at 796, save at 816

def keypad():
    keys = [["Key1", "Key2", "Key3"], ["Key4", "Key5", "Key6"],
            ["Key7", "Key8", "Key9"], ["KeyDot", "Key0", "KeyDel"]]
    label = {"KeyDot": ".", "KeyDel": "⌫"}
    out = []
    for r, row in enumerate(keys):
        for c, name in enumerate(row):
            x = GUTTER + c * (KEY_W + KEY_GAP)
            y = GRID_Y + r * (KEY_H + KEY_VGAP)
            ch = label.get(name, name[3:])
            col = MUTED if name == "KeyDel" else FG
            out.append(frame(name, x, y, KEY_W, KEY_H, fill=SURFACE,
                             stroke=BORDER_SOFT, radius=RADIUS, children=[
                text(f"{name} Label", 0, 0, KEY_W, KEY_H, ch, 24, col,
                     weight=500, align="CENTER", valign="CENTER")]))
    return out

# category pills: On/Off pair per pill, JS toggles visible
PILL_W = (CW - 2 * 10) // 3  # 120
pills = []
PILL_NAMES = ["Pill Food", "Pill Transit", "Pill Daily"]
for i, ((cat, _), pname) in enumerate(zip(CATS, PILL_NAMES)):
    x = GUTTER + i * (PILL_W + 10)
    on = frame(f"{pname} On", 0, 0, PILL_W, 44, fill=ACCENT, radius=PILL,
               children=[text(f"{pname} On Label", 0, 0, PILL_W, 44, cat, 14,
                              WHITE, weight=600, align="CENTER", valign="CENTER")])
    off = frame(f"{pname} Off", 0, 0, PILL_W, 44, fill=SURFACE, stroke=BORDER,
                radius=PILL,
                children=[text(f"{pname} Off Label", 0, 0, PILL_W, 44, cat, 14,
                               FG2, align="CENTER", valign="CENTER")])
    pills.append(frame(pname, x, 300, PILL_W, 44, children=[on, off]))

add = frame("Add", 520, 0, W, H, fill=BG, clips=True, children=[
    frame("Close Button", GUTTER, 48, 40, 40, fill=SURFACE, stroke=BORDER,
          radius=PILL, children=[
              text("Close Glyph", 0, -2, 40, 40, "×", 20, FG,
                   align="CENTER", valign="CENTER")]),
    text("Add Title", 0, 58, W, 24, "记一笔", 16, FG, weight=600,
         align="CENTER"),
    # amount entry: ¥ mark left, live entry right
    text("Currency Mark", GUTTER, 158, 60, 40, "¥", 24, MUTED),
    text("Amount Display", GUTTER + 40, 128, CW - 40, 68, "0", 54, FG,
         weight=700, align="RIGHT", tracking=-1.6),
    rect("Amount Rule", GUTTER, 212, CW, 1, BORDER),
    text("Cat Label", GUTTER, 272, 200, 18, "分类", 12, MUTED, weight=600),
    *pills,
    frame("Note Row", GUTTER, 372, CW, 56, fill=SURFACE, stroke=BORDER_SOFT,
          radius=RADIUS, children=[
              text("Note Label", 16, 9, 60, 16, "备注", 12, MUTED),
              text("Note Value", 16, 27, 240, 20, "楼下便利店", 14, FG),
              text("Note Cycle Hint", CW - 96, 19, 80, 18, "换一个", 12,
                   ACCENT, align="RIGHT"),
          ]),
    *keypad(),
    frame("Save Button", GUTTER, 816, CW, 56, fill=ACCENT, radius=PILL,
          children=[
              text("Save Label", 0, 0, CW, 56, "保存", 16, WHITE, weight=600,
                   align="CENTER", valign="CENTER")]),
])

doc = {
    "name": "Expense",
    "document": {"id": "30:0", "name": "Document", "type": "DOCUMENT", "children": [
        {"id": "30:1", "name": "Page 1", "type": "CANVAS",
         "children": [ledger, add]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
