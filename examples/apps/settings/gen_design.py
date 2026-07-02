#!/usr/bin/env python3
"""Generate design.json for the settings benchmark app.

Apple tokens (design-systems/apple/design-tokens.json): grouped-list look,
one accent, one radius, Segoe UI (present on Windows).
Re-run to regenerate: python gen_design.py
"""
import json
import os

# apple tokens
BG_PAGE = "#f5f5f7"      # --surface (grouped background)
CARD = "#ffffff"         # --bg (card surface)
FG = "#1d1d1f"           # --fg
FG2 = "#424245"          # --fg-2
MUTED = "#6e6e73"        # --muted (passes AA on both surfaces; --meta does not)
BORDER = "#d2d2d7"       # --border
BORDER_SOFT = "#e8e8ed"  # --border-soft (hairlines, off track)
ACCENT = "#0071e3"       # --accent (switch on track)
ACCENT_TEXT = "#0066cc"  # --accent-active (accent as text: 5.2:1 on --surface)
WHITE = "#ffffff"
RADIUS = 12              # --radius-md
FONT = "Segoe UI"

W, H = 420, 900
GUTTER = 20
CARD_W = W - 2 * GUTTER  # 380
ROW_H = 56
SW_W, SW_H = 51, 31      # switch track
KNOB = 27

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
         valign="TOP", tracking=0):
    return {"id": nid(), "name": name, "type": "TEXT",
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "characters": chars, "fills": [solid(color)],
            "style": {"fontFamily": FONT, "fontWeight": weight, "fontSize": size,
                      "textAlignHorizontal": align, "textAlignVertical": valign,
                      "letterSpacing": tracking}}

def rect(name, x, y, w, h, fill, radius=0):
    n = {"id": nid(), "name": name, "type": "RECTANGLE",
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "fills": [solid(fill)]}
    if radius:
        n["cornerRadius"] = radius
    return n

def frame(name, x, y, w, h, fill=None, radius=0, children=None, clips=False,
          visible=True, stroke=None):
    n = {"id": nid(), "name": name, "type": "FRAME",
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "clipsContent": clips, "fills": [solid(fill)] if fill else [],
         "children": children or []}
    if radius:
        n["cornerRadius"] = radius
    if stroke:
        n["strokes"] = [solid(stroke)]
        n["strokeWeight"] = 1
    if not visible:
        n["visible"] = False
    return n

def switch(prefix, x, y):
    """Two pre-baked states, one visible at a time (JS setVisible toggles)."""
    knob_y = (SW_H - KNOB) / 2  # 2
    on = frame(f"{prefix}On", x, y, SW_W, SW_H, fill=ACCENT, radius=SW_H / 2,
               visible=False, children=[
                   rect(f"{prefix}OnKnob", SW_W - KNOB - 2, knob_y, KNOB, KNOB,
                        WHITE, radius=KNOB / 2)])
    off = frame(f"{prefix}Off", x, y, SW_W, SW_H, fill=BORDER_SOFT,
                radius=SW_H / 2, stroke=BORDER, children=[
                    rect(f"{prefix}OffKnob", 2, knob_y, KNOB, KNOB, WHITE,
                         radius=KNOB / 2)])
    off["children"][0]["strokes"] = [solid(BORDER)]
    off["children"][0]["strokeWeight"] = 1
    return [on, off]

SW_X = CARD_W - 16 - SW_W          # 313
SW_Y = (ROW_H - SW_H) / 2          # 12.5

def switch_row(row_name, label, sw_prefix, y):
    return frame(row_name, 0, y, CARD_W, ROW_H, fill=CARD, children=[
        text(f"{row_name} Label", 16, 0, 220, ROW_H, label, 17, FG,
             valign="CENTER"),
        *switch(sw_prefix, SW_X, SW_Y)])

def hairline(y):
    return rect("Hairline", 16, y, CARD_W - 16, 1, BORDER_SOFT)

# ---- Settings (home) frame ----
card1_y = 154
card2_y = card1_y + 3 * ROW_H + 52  # 374

settings_children = [
    text("Title", GUTTER, 72, CARD_W, 40, "Settings", 28, FG, weight=700,
         tracking=-0.4),
    text("Section General", GUTTER + 16, card1_y - 24, 300, 16, "GENERAL", 12,
         MUTED, weight=600, tracking=0.8),
    frame("Card General", GUTTER, card1_y, CARD_W, 3 * ROW_H, fill=CARD,
          radius=RADIUS, clips=True, children=[
              switch_row("Row Notifications", "Notifications", "SwitchNotif", 0),
              switch_row("Row Dark Mode", "Dark Mode", "SwitchDark", ROW_H),
              switch_row("Row Sounds", "Sounds", "SwitchSound", 2 * ROW_H),
              hairline(ROW_H),
              hairline(2 * ROW_H)]),
    text("Section About", GUTTER + 16, card2_y - 24, 300, 16, "ABOUT", 12,
         MUTED, weight=600, tracking=0.8),
    frame("Card About", GUTTER, card2_y, CARD_W, ROW_H, fill=CARD,
          radius=RADIUS, clips=True, children=[
              frame("Row About", 0, 0, CARD_W, ROW_H, fill=CARD, children=[
                  text("Row About Label", 16, 0, 220, ROW_H, "About", 17, FG,
                       valign="CENTER"),
                  text("Chevron", CARD_W - 16 - 24, 0, 24, ROW_H, "›", 21,
                       MUTED, align="RIGHT", valign="CENTER")])]),
    text("Footer", GUTTER, card2_y + ROW_H + 28, CARD_W, 16,
         "Settings 1.0.0", 12, MUTED, align="CENTER"),
]

# ---- About frame ----
about_children = [
    frame("BackBtn", 8, 56, 110, 44, children=[
        text("BackBtn Label", 12, 0, 98, 44, "‹ Back", 17, ACCENT_TEXT,
             weight=600, valign="CENTER")]),
    frame("About Card", GUTTER, 300, CARD_W, 220, fill=CARD, radius=RADIUS,
          clips=True, children=[
              text("App Name", 0, 44, CARD_W, 36, "Settings", 28, FG,
                   weight=700, align="CENTER", tracking=-0.4),
              text("Version", 0, 92, CARD_W, 20, "Version 1.0.0", 14, MUTED,
                   align="CENTER"),
              text("Blurb", 30, 130, CARD_W - 60, 60,
                   "A small settings screen built with figo. Preferences are "
                   "saved on the device and restored on the next launch.",
                   14, FG2, align="CENTER")]),
]

doc = {
    "name": "Settings",
    "document": {"id": "20:0", "name": "Document", "type": "DOCUMENT", "children": [
        {"id": "20:1", "name": "Page 1", "type": "CANVAS", "children": [
            {"id": nid(), "name": "Settings", "type": "FRAME",
             "size": {"x": W, "y": H}, "relativeTransform": xf(0, 0),
             "clipsContent": True, "fills": [solid(BG_PAGE)],
             "children": settings_children},
            {"id": nid(), "name": "About", "type": "FRAME",
             "size": {"x": W, "y": H}, "relativeTransform": xf(W + 40, 0),
             "clipsContent": True, "fills": [solid(BG_PAGE)],
             "children": about_children}]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
