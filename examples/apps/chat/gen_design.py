#!/usr/bin/env python3
"""Generate design.json for the chat benchmark app (desktop, 420x900).

Single frame "Chat", linear-app dark tokens
(design-systems/linear-app/design-tokens.json). Restraint: near-black page,
surface bubbles for the peer, the ONE accent (#5e6ad2) reserved for my own
bubbles + the send button, one radius (12), type on the 12/14/18 scale,
hairline dividers. Font is Segoe UI (present on Windows).

Bubbles size to content: each bubble is a hug-both auto-layout frame whose
TEXT child is WIDTH_AND_HEIGHT auto-resize; app.js pre-wraps long messages
with \n so a bubble never exceeds ~252px. Left/right alignment comes from
counterAxisAlignItems MIN/MAX on the per-side wrapper, toggled by setVisible.

Re-run to regenerate: python gen_design.py
"""
import json
import os

# linear-app tokens
BG = "#08090a"          # --bg
SURFACE = "#191a1b"     # --surface (peer bubble)
FG = "#f7f8f8"          # --fg
FG2 = "#d0d6e0"         # --fg-2 (peer bubble copy, 11.5:1 on surface)
MUTED = "#8a8f98"       # --muted (timestamps, 6.2:1 on bg)
BORDER_A = 0.08         # --border rgba(255,255,255,.08)
ACCENT = "#5e6ad2"      # --accent (my bubble + send, the one accent)
ACCENT_ON = "#ffffff"   # --accent-on (4.7:1 on accent, AA)
SUCCESS = "#27a644"     # --success ("在线")
RADIUS = 12             # --radius-lg, the one radius
FONT = "Segoe UI"
# type scale: 12 / 14 / 18 (linear --text-xs/sm/lg)

W, H = 420, 900
GUTTER = 20
CW = W - 2 * GUTTER     # 380

_next_id = [1]

def nid():
    _next_id[0] += 1
    return f"40:{_next_id[0]}"

def hex_rgba(h, a=1.0):
    h = h.lstrip("#")
    return {"r": int(h[0:2], 16) / 255, "g": int(h[2:4], 16) / 255,
            "b": int(h[4:6], 16) / 255, "a": a}

def solid(c, a=1.0):
    return {"type": "SOLID", "color": hex_rgba(c, a)}

def xf(x, y):
    return [[1, 0, x], [0, 1, y]]

def text(name, x, y, w, h, chars, size, color, weight=400, align="LEFT",
         valign="TOP", line_h=0, auto=None):
    style = {"fontFamily": FONT, "fontWeight": weight, "fontSize": size,
             "textAlignHorizontal": align, "textAlignVertical": valign}
    if line_h:
        style["lineHeightPx"] = line_h
    if auto:
        style["textAutoResize"] = auto
    return {"id": nid(), "name": name, "type": "TEXT",
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "characters": chars, "fills": [solid(color)], "style": style}

def frame(name, x, y, w, h, fill=None, fill_a=1.0, radius=0, stroke=None,
          stroke_a=1.0, children=None, clips=False, extra=None):
    n = {"id": nid(), "name": name, "type": "FRAME",
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "clipsContent": clips,
         "fills": [solid(fill, fill_a)] if fill else [],
         "children": children or []}
    if radius:
        n["cornerRadius"] = radius
    if stroke:
        n["strokes"] = [solid(stroke, stroke_a)]
        n["strokeWeight"] = 1
    if extra:
        n.update(extra)
    return n

def hairline(name, y):
    return {"id": nid(), "name": name, "type": "RECTANGLE",
            "size": {"x": W, "y": 1}, "relativeTransform": xf(0, y),
            "fills": [solid("#ffffff", BORDER_A)]}

def cell(side, fill, txt_color, sample, sample_time, align):
    """One side's bubble + timestamp, hug both axes."""
    b = frame(f"{side}Bubble", 0, 0, 160, 41, fill=fill, radius=RADIUS,
              children=[
                  text(f"{side}Text", 14, 10, 132, 21, sample, 14, txt_color,
                       line_h=21, auto="WIDTH_AND_HEIGHT")],
              extra={"layoutMode": "VERTICAL",
                     "primaryAxisSizingMode": "AUTO",
                     "counterAxisSizingMode": "AUTO",
                     "paddingLeft": 14, "paddingRight": 14,
                     "paddingTop": 10, "paddingBottom": 10})
    t = text(f"{side}Time", 0, 0, 90, 16, sample_time, 12, MUTED,
             auto="WIDTH_AND_HEIGHT")
    return frame(f"{side}Cell", 0, 0, 176, 62, children=[b, t], extra={
        "layoutMode": "VERTICAL", "primaryAxisSizingMode": "AUTO",
        "counterAxisSizingMode": "AUTO", "counterAxisAlignItems": align,
        "itemSpacing": 5})

# One row = both prebuilt side cells in a HORIZONTAL hug-height stack.
# Engine gotcha: layout in-flow uses the AUTHORED visible flag, so a JS
# setVisible(false) hides pixels but not flow space. Workaround: app.js also
# empties the hidden cell's texts, collapsing it to a ~28px invisible ghost
# (bubble padding only) on the packed side, and flips row.primaryAlign
# min/max so the visible cell lands flush left (theirs) or right (mine).
msg_row = frame("MsgRow", 0, 0, CW, 78, children=[
    cell("Their", SURFACE, FG2, "那约个饭？", "昨天 19:05", "MIN"),
    cell("Mine", ACCENT, ACCENT_ON, "好啊", "昨天 19:06", "MAX"),
], extra={"layoutMode": "HORIZONTAL", "primaryAxisSizingMode": "FIXED",
          "counterAxisSizingMode": "AUTO", "primaryAxisAlignItems": "MIN",
          "itemSpacing": 0, "paddingBottom": 14})

LIST_Y = 88
LIST_H = 736            # 88..824; 12 messages ≈ 1130px content -> scrolls
msg_list = frame(
    "MsgList", GUTTER, LIST_Y, CW, LIST_H, clips=True, children=[msg_row],
    extra={"layoutMode": "VERTICAL", "primaryAxisSizingMode": "FIXED",
           "counterAxisSizingMode": "FIXED", "itemSpacing": 0,
           "paddingTop": 16, "paddingBottom": 4,
           "overflowDirection": "VERTICAL_SCROLLING"})

chat = frame("Chat", 0, 0, W, H, fill=BG, clips=True, children=[
    text("PeerName", GUTTER, 24, 200, 26, "阿杰", 18, FG, weight=600),
    text("PeerStatus", GUTTER, 54, 200, 16, "在线", 12, SUCCESS),
    hairline("Header Divider", LIST_Y - 1),
    msg_list,
    hairline("Input Divider", 824),
    # editable input; the frame fill makes it a hit target
    frame("InputBox", GUTTER, 838, 296, 44, fill=SURFACE, radius=RADIUS,
          stroke="#ffffff", stroke_a=BORDER_A, children=[
              text("InputText", 14, 12, 268, 20, "", 14, FG),
              text("InputHint", 14, 12, 268, 20, "发个消息", 14, MUTED)]),
    frame("SendBtn", 328, 838, 72, 44, fill=ACCENT, radius=RADIUS, children=[
        text("SendLabel", 0, 0, 72, 44, "发送", 14, ACCENT_ON, weight=600,
             align="CENTER", valign="CENTER")]),
])

doc = {
    "name": "Chat",
    "document": {"id": "40:0", "name": "Document", "type": "DOCUMENT",
                 "children": [
        {"id": "40:1", "name": "Page 1", "type": "CANVAS",
         "children": [chat]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
