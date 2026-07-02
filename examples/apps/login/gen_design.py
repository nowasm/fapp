#!/usr/bin/env python3
"""Generate design.json for the login benchmark app.

Two 420x900 frames ("SignIn" + "Welcome"), stripe tokens
(design-systems/stripe/design-tokens.json). Restrained stripe form:
one accent (--accent), one radius (--radius-lg = 8), labels 12px /
inputs 16px on the --text-* scale, hairline underlines with --border.
Re-run to regenerate: python gen_design.py
"""
import json
import os

# stripe tokens
BG = "#ffffff"        # --bg
FG = "#061b31"        # --fg
MUTED = "#64748d"     # --muted (4.75:1 on white, AA)
BORDER = "#e5edf5"    # --border (hairlines only)
ACCENT = "#533afd"    # --accent (6.19:1 on white, AA) - the one accent
# --danger is #ea2261, but that is only 4.29:1 on white and FAILS WCAG AA
# for 12px error text. Darkened along the same hue until it passes:
DANGER = "#d81b57"    # 4.98:1 on white, AA
RADIUS = 8            # --radius-lg, the one radius
FONT = "Segoe UI"     # Windows-real stand-in for the --font-body stack
# type scale used: 12 / 14 / 16 / 32 (--text-xs/sm/base/2xl)

W, H = 420, 900
GUTTER = 24           # --container-gutter-tablet
CW = W - 2 * GUTTER   # 372 content width

_next_id = [1]

def nid():
    _next_id[0] += 1
    return f"40:{_next_id[0]}"

def hex_rgba(h, a=1.0):
    h = h.lstrip("#")
    return {"r": int(h[0:2], 16) / 255, "g": int(h[2:4], 16) / 255,
            "b": int(h[4:6], 16) / 255, "a": a}

def lum(h):
    c = [hex_rgba(h)[k] for k in "rgb"]
    c = [v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4 for v in c]
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]

def contrast(a, b):
    la, lb = sorted((lum(a), lum(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)

assert contrast(DANGER, BG) >= 4.5, "danger must pass AA for 12px error text"
assert contrast(MUTED, BG) >= 4.5 and contrast(ACCENT, BG) >= 4.5

def mix_white(h, t):
    h = h.lstrip("#")
    c = [int(h[i:i + 2], 16) for i in (0, 2, 4)]
    c = [round(v + (255 - v) * t) for v in c]
    return "#%02x%02x%02x" % tuple(c)

def solid(c, a=1.0):
    return {"type": "SOLID", "color": hex_rgba(c, a)}

def xf(x, y):
    return [[1, 0, x], [0, 1, y]]

def text(name, x, y, w, h, chars, size, color, weight=400, align="LEFT",
         valign="TOP", tracking=0, visible=True):
    n = {"id": nid(), "name": name, "type": "TEXT",
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "characters": chars, "fills": [solid(color)],
         "style": {"fontFamily": FONT, "fontWeight": weight, "fontSize": size,
                   "textAlignHorizontal": align, "textAlignVertical": valign,
                   "letterSpacing": tracking}}
    if not visible:
        n["visible"] = False
    return n

def frame(name, x, y, w, h, fill=None, radius=0, stroke=None, stroke_w=1,
          children=None, clips=False):
    n = {"id": nid(), "name": name, "type": "FRAME",
         "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
         "clipsContent": clips, "fills": [solid(fill)] if fill else [],
         "children": children or []}
    if radius:
        n["cornerRadius"] = radius
    if stroke:
        n["strokes"] = [solid(stroke)]
        n["strokeWeight"] = stroke_w
    return n

def rect(name, x, y, w, h, fill):
    return {"id": nid(), "name": name, "type": "RECTANGLE",
            "size": {"x": w, "y": h}, "relativeTransform": xf(x, y),
            "fills": [solid(fill)]}

# --------------------------------------------------------------- SignIn page
TOGGLE_W = 48
signin = frame("SignIn", 0, 0, W, H, fill=BG, clips=True, children=[
    text("Brand", GUTTER, 72, 160, 30, "figo", 22, ACCENT, weight=700,
         tracking=-0.4),
    text("Title", GUTTER, 148, CW, 42, "欢迎回来", 32, FG, weight=700,
         tracking=-0.64),
    text("Subtitle", GUTTER, 198, CW, 20, "登录你的账户以继续", 14, MUTED),

    # email field
    text("Email Label", GUTTER, 276, 200, 16, "邮箱", 12, MUTED, weight=600),
    text("Email Input", GUTTER, 298, CW, 24, "", 16, FG),
    rect("Email Line", GUTTER, 330, CW, 1, BORDER),
    text("Email Error", GUTTER, 338, CW, 16, "请输入有效的邮箱地址", 12,
         DANGER, visible=False),

    # password field (input leaves room for the show/hide toggle)
    text("Password Label", GUTTER, 380, 200, 16, "密码", 12, MUTED, weight=600),
    text("Password Input", GUTTER, 402, CW - TOGGLE_W - 12, 24, "", 16, FG),
    frame("Toggle Mask", GUTTER + CW - TOGGLE_W, 398, TOGGLE_W, 28, children=[
        text("Toggle Label", 0, 0, TOGGLE_W, 28, "显示", 12, ACCENT,
             weight=600, align="RIGHT", valign="CENTER")]),
    rect("Password Line", GUTTER, 434, CW, 1, BORDER),
    text("Password Error", GUTTER, 442, CW, 16, "密码至少需要 8 位", 12,
         DANGER, visible=False),

    # actions
    frame("Sign In Button", GUTTER, 506, CW, 48, fill=ACCENT, radius=RADIUS,
          children=[
              text("Sign In Label", 0, 0, CW, 48, "登录", 16, BG, weight=600,
                   align="CENTER", valign="CENTER")]),
    text("Forgot Link", GUTTER, 578, CW, 20, "忘记密码", 14, ACCENT,
         weight=600, align="CENTER"),
])

# -------------------------------------------------------------- Welcome page
BADGE = 56
welcome = frame("Welcome", 480, 0, W, H, fill=BG, clips=True, children=[
    text("Brand", GUTTER, 72, 160, 30, "figo", 22, ACCENT, weight=700,
         tracking=-0.4),
    frame("Welcome Badge", (W - BADGE) // 2, 280, BADGE, BADGE,
          fill=mix_white(ACCENT, 0.9), radius=BADGE // 2, children=[
              text("Badge Mark", 0, 0, BADGE, BADGE, "✓", 24, ACCENT,
                   weight=700, align="CENTER", valign="CENTER")]),
    text("Welcome Title", GUTTER, 372, CW, 40, "你好", 32, FG, weight=700,
         tracking=-0.64, align="CENTER"),
    text("Welcome Email", GUTTER, 422, CW, 20, "you@example.com", 14, MUTED,
         align="CENTER"),
    frame("Sign Out Button", GUTTER, 494, CW, 48, fill=BG, radius=RADIUS,
          stroke=BORDER, children=[
              text("Sign Out Label", 0, 0, CW, 48, "退出", 16, FG, weight=600,
                   align="CENTER", valign="CENTER")]),
])

doc = {
    "name": "Login",
    "document": {"id": "40:0", "name": "Document", "type": "DOCUMENT",
                 "children": [
                     {"id": "40:1", "name": "Page 1", "type": "CANVAS",
                      "children": [signin, welcome]}]},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "design.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print(f"wrote {out}")
