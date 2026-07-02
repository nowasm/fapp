#!/usr/bin/env python3
"""Generate design.json for the alarm benchmark app.

Apple tokens (design-systems/apple/design-tokens.json): grouped-card look,
one accent, one radius family, Segoe UI (present on Windows).
Two frames: "Alarms" (list + demo overlay) and "Picker" (two snap wheels).

Wheel-picker design notes (learned from _snap_regress):
- the scrolling frame MUST declare primaryAxisSizingMode "FIXED", otherwise
  bindList re-hugs the content and maxScroll collapses to 0;
- ui.snapTo aligns child i with the FIRST child's start, so a centered
  selection needs 2 rows of lead-in/lead-out. paddingTop works (it is part of
  the scroll extent) but paddingBottom does NOT: the scrollable extent ends at
  the last child's bottom, so trailing padding is unreachable and the last two
  values could never reach the center row. The wheels therefore use paddingTop
  + two PHANTOM rows at the end (bindList count + 2, blank text) instead of
  paddingBottom;
- hitTestFrame only hits paintable nodes (fills/strokes/text non-empty), so
  the wheels and their items carry an alpha-0 fill: invisible, but the whole
  row area stays draggable while the selection band shows through from behind.

Re-run to regenerate: python gen_design.py
"""
import json
import os

# apple tokens
BG_PAGE = "#f5f5f7"      # --surface (grouped background)
CARD = "#ffffff"         # --bg (card surface)
FG = "#1d1d1f"           # --fg
FG2 = "#424245"          # --fg-2
MUTED = "#6e6e73"        # --muted (AA on both surfaces)
BORDER = "#d2d2d7"       # --border
BORDER_SOFT = "#e8e8ed"  # --border-soft (hairlines, off track, wheel band)
ACCENT = "#0071e3"       # --accent (switch on track, primary button)
ACCENT_TEXT = "#0066cc"  # --accent-active (accent as text: AA on --surface)
WHITE = "#ffffff"
RADIUS = 12              # --radius-md, the one card radius
FONT = "Segoe UI"

W, H = 420, 900
GUTTER = 20
CARD_W = W - 2 * GUTTER  # 380

# alarm list row
ROW_H = 84
SW_W, SW_H = 51, 31      # switch track
KNOB = 27

# wheel picker
WHEEL_ROW = 56           # uniform row height = snap pitch
WHEEL_VIS = 5            # visible rows
WHEEL_H = WHEEL_ROW * WHEEL_VIS       # 280 viewport
WHEEL_PAD = 2 * WHEEL_ROW             # 112: first child starts on center row
                                      # (bottom lead-out = 2 phantom rows)
WHEEL_W = 120
WHEEL_Y = 280
HOUR_X, MIN_X = 70, 230
BAND_Y = WHEEL_Y + WHEEL_PAD          # 392 (center row)

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
         valign="TOP", tracking=0):
    return {"id": nid(), "name": name, "type": "TEXT",
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "characters": chars, "fills": [solid(color)],
            "style": {"fontFamily": FONT, "fontWeight": weight, "fontSize": size,
                      "textAlignHorizontal": align, "textAlignVertical": valign,
                      "letterSpacing": tracking}}

def rect(name, x, y, w, h, fill, radius=0, alpha=1.0):
    n = {"id": nid(), "name": name, "type": "RECTANGLE",
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "fills": [solid(fill, alpha)]}
    if radius:
        n["cornerRadius"] = radius
    return n

def frame(name, x, y, w, h, fill=None, fill_alpha=1.0, radius=0, children=None,
          clips=False, visible=True, stroke=None):
    n = {"id": nid(), "name": name, "type": "FRAME",
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "clipsContent": clips,
         "fills": [solid(fill, fill_alpha)] if fill else [],
         "children": children or []}
    if radius:
        n["cornerRadius"] = radius
    if stroke:
        n["strokes"] = [solid(stroke)]
        n["strokeWeight"] = 1
    if not visible:
        n["visible"] = False
    return n

def switch(prefix, x, y, on_visible=False):
    """Two pre-baked states, one visible at a time (JS setVisible flips)."""
    knob_y = (SW_H - KNOB) / 2  # 2
    on = frame(f"{prefix}On", x, y, SW_W, SW_H, fill=ACCENT, radius=SW_H / 2,
               visible=on_visible, children=[
                   rect(f"{prefix}OnKnob", SW_W - KNOB - 2, knob_y, KNOB, KNOB,
                        WHITE, radius=KNOB / 2)])
    off = frame(f"{prefix}Off", x, y, SW_W, SW_H, fill=BORDER_SOFT,
                radius=SW_H / 2, stroke=BORDER, visible=not on_visible,
                children=[
                    rect(f"{prefix}OffKnob", 2, knob_y, KNOB, KNOB, WHITE,
                         radius=KNOB / 2)])
    off["children"][0]["strokes"] = [solid(BORDER)]
    off["children"][0]["strokeWeight"] = 1
    return [on, off]

# ---- Alarms frame ----

def alarm_row():
    """bindList template. Time, label, switch pair, bottom hairline."""
    return frame("AlarmRow", 0, 0, CARD_W, ROW_H, fill=CARD, children=[
        text("RowTime", 20, 8, 190, 50, "06:30", 40, FG, tracking=-0.6),
        text("RowLabel", 20, 58, 220, 18, "Wake up", 14, MUTED),
        *switch("RowSw", CARD_W - 16 - SW_W, (ROW_H - SW_H) / 2, on_visible=True),
        rect("RowLine", 20, ROW_H - 1, CARD_W - 20, 1, BORDER_SOFT),
    ])

alarm_list = frame("AlarmList", GUTTER, 140, CARD_W, ROW_H * 2, fill=CARD,
                   radius=RADIUS, clips=True, children=[alarm_row()])
alarm_list["layoutMode"] = "VERTICAL"
alarm_list["itemSpacing"] = 0
alarm_list["primaryAxisSizingMode"] = "AUTO"    # hug: grows with bindList
alarm_list["counterAxisSizingMode"] = "FIXED"

demo_overlay = frame("DemoOverlay", 0, 0, W, H, fill="#000000", fill_alpha=0.42,
                     visible=False, children=[
    frame("DemoCard", 50, 366, 320, 168, fill=CARD, radius=RADIUS, children=[
        text("DemoTitle", 0, 30, 320, 24, "Alarm preview", 17, FG, weight=600,
             align="CENTER"),
        text("DemoText", 0, 66, 320, 30, "Rings in 2 seconds", 21, FG2,
             align="CENTER"),
        text("DemoHint", 0, 122, 320, 16, "Tap anywhere to dismiss", 12, MUTED,
             align="CENTER"),
    ])])

alarms_children = [
    text("Title", GUTTER, 72, CARD_W, 40, "Alarms", 28, FG, weight=700,
         tracking=-0.4),
    alarm_list,
    text("Hint", GUTTER, 778, CARD_W, 16,
         "Long-press an alarm to preview the ring", 12, MUTED, align="CENTER"),
    frame("BtnNew", GUTTER, 812, CARD_W, 52, fill=ACCENT, radius=26, children=[
        text("BtnNewLabel", 0, 0, CARD_W, 52, "New Alarm", 17, WHITE,
             weight=600, align="CENTER", valign="CENTER")]),
    demo_overlay,   # last = on top of everything
]

# ---- Picker frame ----

def wheel(name, x, item_name, text_name, sample):
    """FIXED-size scrolling frame: bindList + snapToChildren wheel column.
    Alpha-0 fills keep every pixel of the column draggable (hit-testable)
    while the selection band shows through from behind."""
    item = frame(item_name, 0, 0, WHEEL_W, WHEEL_ROW, fill=WHITE, fill_alpha=0.0,
                 children=[
                     text(text_name, 0, 0, WHEEL_W, WHEEL_ROW, sample, 28, FG,
                          align="CENTER", valign="CENTER")])
    w = frame(name, x, WHEEL_Y, WHEEL_W, WHEEL_H, clips=True, children=[item])
    w["overflowDirection"] = "VERTICAL_SCROLLING"
    w["layoutMode"] = "VERTICAL"
    w["itemSpacing"] = 0
    w["paddingTop"] = WHEEL_PAD
    # no paddingBottom: it would not extend the scroll extent (see module
    # docstring) — the app appends two phantom rows instead
    w["primaryAxisSizingMode"] = "FIXED"   # REQUIRED or bindList -> maxScroll 0
    w["counterAxisSizingMode"] = "FIXED"
    return w

picker_children = [
    frame("BtnCancel", 8, 56, 100, 44, children=[
        text("BtnCancelLabel", 12, 0, 88, 44, "Cancel", 17, ACCENT_TEXT,
             valign="CENTER")]),
    text("PickerTitle", 110, 56, 200, 44, "New Alarm", 17, FG, weight=600,
         align="CENTER", valign="CENTER"),
    frame("BtnSave", W - 8 - 100, 56, 100, 44, children=[
        text("BtnSaveLabel", 0, 0, 88, 44, "Save", 17, ACCENT_TEXT, weight=600,
             align="RIGHT", valign="CENTER")]),
    text("PreviewTime", GUTTER, 150, CARD_W, 70, "07:00", 56, FG,
         align="CENTER", tracking=-0.8),
    # selection band sits BEHIND the (alpha-0 filled) wheels
    rect("SelectionBand", 40, BAND_Y, W - 80, WHEEL_ROW, BORDER_SOFT,
         radius=RADIUS),
    wheel("HourWheel", HOUR_X, "HourItem", "HourText", "07"),
    text("Colon", 190, BAND_Y, 40, WHEEL_ROW, ":", 28, FG,
         align="CENTER", valign="CENTER"),
    wheel("MinuteWheel", MIN_X, "MinuteItem", "MinuteText", "00"),
    text("PickerHint", GUTTER, 596, CARD_W, 16, "Scroll to set the time", 12,
         MUTED, align="CENTER"),
]

doc = {
    "name": "Alarm",
    "document": {"id": "30:0", "name": "Document", "type": "DOCUMENT", "children": [
        {"id": "30:1", "name": "Page 1", "type": "CANVAS", "children": [
            {"id": nid(), "name": "Alarms", "type": "FRAME",
             "size": {"x": W, "y": H}, "relativeTransform": xf(0, 0),
             "clipsContent": True, "fills": [solid(BG_PAGE)],
             "children": alarms_children},
            {"id": nid(), "name": "Picker", "type": "FRAME",
             "size": {"x": W, "y": H}, "relativeTransform": xf(W + 40, 0),
             "clipsContent": True, "fills": [solid(BG_PAGE)],
             "children": picker_children}]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
