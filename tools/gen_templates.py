#!/usr/bin/env python3
"""Generate the figoplay app templates' design.json from design-system tokens.

Each template is a standard app project (templates/<name>/{app.json, app.js,
design.json}). This script only (re)builds the design.json files — the app.json
and app.js are hand-written and editable. Palettes are pulled from the vendored
design-systems/<sys>/design-tokens.json so the templates inherit a real product
aesthetic (color, radius). Run from the repo root:

    python tools/gen_templates.py

Fonts: templates use "Segoe UI" (always present on Windows, and what the sample
app uses). The design-system's own font (Inter / CoinbaseText / Söhne …) needs
its .ttf bundled into the app's fonts/ dir — a later step; color/radius are the
bigger aesthetic lever and those come straight from the tokens.
"""
import json
import os
import re
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 390, 844
# Bundled open font (Poppins, weights 400/600/700) so text renders on web/android
# too — those platforms have no system fonts. Copied into each template's fonts/.
FONT = "Poppins"
FONT_FILES = ["Poppins-Regular.ttf", "Poppins-SemiBold.ttf", "Poppins-Bold.ttf"]


def tokens(system):
    p = os.path.join(ROOT, "design-systems", system, "design-tokens.json")
    d = json.load(open(p, encoding="utf-8"))
    return {t["name"]: t["value"] for t in d["tokens"]}


def rgba(hex_or_css, alpha=1.0):
    """#rrggbb / #rrggbbaa / rgba(r,g,b,a) -> figo color {r,g,b,a} in 0..1."""
    s = hex_or_css.strip()
    m = re.match(r"rgba?\(([^)]+)\)", s)
    if m:
        parts = [p.strip() for p in m.group(1).split(",")]
        r, g, b = (int(float(parts[i])) for i in range(3))
        a = float(parts[3]) if len(parts) > 3 else 1.0
        return {"r": r / 255, "g": g / 255, "b": b / 255, "a": a * alpha}
    s = s.lstrip("#")
    r, g, b = int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)
    a = int(s[6:8], 16) / 255 if len(s) >= 8 else 1.0
    return {"r": r / 255, "g": g / 255, "b": b / 255, "a": a * alpha}


def px(v):  # "12px" -> 12
    return float(str(v).replace("px", "").strip() or 0)


_id = [0]


def nid():
    _id[0] += 1
    return f"1:{_id[0]}"


def solid(color):
    return {"type": "SOLID", "color": color}


def node(name, ntype, x, y, w, h, **extra):
    n = {
        "id": nid(),
        "name": name,
        "type": ntype,
        "size": {"x": w, "y": h},
        "relativeTransform": [[1, 0, x], [0, 1, y]],
    }
    n.update(extra)
    return n


def frame(name, x, y, w, h, fill=None, radius=0, children=None, stroke=None,
          stroke_w=0, clips=True, auto=None):
    extra = {"clipsContent": clips, "children": children or []}
    if fill is not None:
        extra["fills"] = [solid(fill)]
    if radius:
        extra["cornerRadius"] = radius
    if stroke is not None:
        extra["strokes"] = [solid(stroke)]
        extra["strokeWeight"] = stroke_w or 1
        extra["strokeAlign"] = "INSIDE"
    if auto:  # Figma REST auto-layout keys (layoutMode/itemSpacing/…)
        extra.update(auto)
    return node(name, "FRAME", x, y, w, h, **extra)


# Vertical auto-layout that hugs its height (so bindList clones stack instead of
# overlapping — bindList keeps the first child as the template and re-stacks).
def vstack(spacing):
    return {"layoutMode": "VERTICAL", "itemSpacing": spacing,
            "primaryAxisSizingMode": "AUTO", "counterAxisSizingMode": "FIXED"}


def text(name, x, y, w, h, s, color, size=16, weight=400, align="LEFT",
         valign="TOP", spacing=0):
    return node(name, "TEXT", x, y, w, h, characters=s, fills=[solid(color)],
                style={"fontFamily": FONT, "fontWeight": weight, "fontSize": size,
                       "textAlignHorizontal": align, "textAlignVertical": valign,
                       "letterSpacing": spacing})


def document(*frames):
    return {"name": "Template", "document": {
        "id": nid(), "name": "Document", "type": "DOCUMENT", "children": [
            {"id": nid(), "name": "Page 1", "type": "CANVAS", "children": list(frames)}]}}


def write(name, doc):
    tdir = os.path.join(ROOT, "templates", name)
    out = os.path.join(tdir, "design.json")
    os.makedirs(tdir, exist_ok=True)
    json.dump(doc, open(out, "w", encoding="utf-8"), indent=1)
    # Bundle the fonts so the template works on web/android out of the box.
    fdir = os.path.join(tdir, "fonts")
    os.makedirs(fdir, exist_ok=True)
    src = os.path.join(ROOT, "examples", "assets", "fonts")
    for f in FONT_FILES:
        shutil.copy(os.path.join(src, f), os.path.join(fdir, f))
    print(f"  {name}/design.json  ({os.path.getsize(out)} bytes) + fonts/")


# ---------------------------------------------------------------- tab-shell ---
def tab_shell():
    t = tokens("linear-app")
    bg, surf, fg, muted, accent = (rgba(t["--bg"]), rgba(t["--surface"]),
                                   rgba(t["--fg"]), rgba(t["--muted"]),
                                   rgba(t["--accent"]))
    rad = px(t["--radius-lg"])

    def nav(active):
        items = [("tab-home", "Home"), ("tab-search", "Search"), ("tab-profile", "Profile")]
        kids = []
        for i, (nm, lbl) in enumerate(items):
            kids.append(text(nm, i * (W / 3), 26, W / 3, 20, lbl,
                             accent if nm == active else muted, size=13,
                             weight=600 if nm == active else 400, align="CENTER"))
        return frame("Bottom Nav Bar", 0, H - 72, W, 72, fill=surf, children=kids)

    def screen(name, title, active):
        cards = [frame("card", 20, 120 + i * 96, W - 40, 80, fill=surf, radius=rad,
                       children=[text("card-title", 16, 16, 200, 22, f"Item {i+1}",
                                      fg, size=16, weight=600),
                                 text("card-sub", 16, 44, 260, 18, "Tap to open",
                                      muted, size=13)])
                 for i in range(4)]
        return frame(name, 0, 0, W, H, fill=bg, children=[
            text("title", 20, 56, W - 40, 40, title, fg, size=28, weight=700),
            *cards, nav(active)])

    write("tab-shell", document(
        screen("Home", "Home", "tab-home"),
        screen("Search", "Search", "tab-search"),
        screen("Profile", "Profile", "tab-profile")))


# -------------------------------------------------------------- list-detail ---
def list_detail():
    t = tokens("coinbase")
    bg, surf, fg, muted, accent = (rgba(t["--bg"]), rgba(t["--surface"]),
                                   rgba(t["--fg"]), rgba(t["--muted"]),
                                   rgba(t["--accent"]))
    rad = px(t["--radius-md"])

    # One Card template; bindList clones it per data row and the list's vertical
    # auto-layout stacks them.
    card = frame("Card", 0, 0, W - 40, 72, fill=surf, radius=rad, children=[
        text("row-name", 16, 14, 180, 22, "Asset", fg, size=17, weight=600),
        text("row-sub", 16, 40, 180, 18, "0.00", muted, size=13),
        text("row-value", W - 40 - 140, 14, 124, 22, "$0", fg, size=17,
             weight=600, align="RIGHT")])
    list_frame = frame("List", 0, 0, W, H, fill=bg, children=[
        text("title", 20, 56, W - 40, 40, "Assets", fg, size=30, weight=700),
        frame("list", 20, 116, W - 40, H - 156, fill=None, clips=False,
              children=[card], auto=vstack(12))])

    detail = frame("Detail", 0, 0, W, H, fill=bg, children=[
        text("btn-back", 20, 56, 80, 28, "< Back", accent, size=16, weight=600),
        text("detail-title", 20, 108, W - 40, 36, "Asset", fg, size=28, weight=700),
        text("detail-value", 20, 152, W - 40, 44, "$0", fg, size=36, weight=700),
        frame("detail-card", 20, 220, W - 40, 120, fill=surf, radius=rad, children=[
            text("detail-desc", 16, 16, W - 72, 88,
                 "Detail content goes here. Bind real data in app.js.",
                 muted, size=15)])])
    write("list-detail", document(list_frame, detail))


# --------------------------------------------------------------------- form ---
def form():
    t = tokens("stripe")
    bg, fg, muted, accent, border = (rgba(t["--bg"]), rgba(t["--fg"]),
                                     rgba(t["--muted"]), rgba(t["--accent"]),
                                     rgba(t["--border"]))
    rad = px(t["--radius-md"])

    def field(name, input_name, label, placeholder, y):
        return [
            text(name + "-label", 24, y, W - 48, 18, label, muted, size=13, weight=600),
            frame(name, 24, y + 24, W - 48, 48, fill=rgba("#ffffff"), radius=rad,
                  stroke=border, stroke_w=1, children=[
                      text(input_name, 14, 14, W - 48 - 28, 20, placeholder,
                           fg, size=16)])]

    form_frame = frame("Form", 0, 0, W, H, fill=bg, children=[
        text("title", 24, 64, W - 48, 40, "Create account", fg, size=30, weight=700),
        text("subtitle", 24, 106, W - 48, 22, "Start your free trial.", muted, size=15),
        *field("field-name", "input-name", "NAME", "Ada Lovelace", 160),
        *field("field-email", "input-email", "EMAIL", "ada@example.com", 252),
        frame("btn-submit", 24, 360, W - 48, 52, fill=accent, radius=rad, children=[
            text("submit-label", 0, 0, W - 48, 52, "Create account",
                 rgba("#ffffff"), size=16, weight=600, align="CENTER", valign="CENTER")]),
        text("msg", 24, 428, W - 48, 24, "", accent, size=15, weight=600)])
    write("form", document(form_frame))


if __name__ == "__main__":
    print("generating templates:")
    tab_shell()
    list_detail()
    form()
    print("done.")
