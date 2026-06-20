#!/usr/bin/env python3
"""figmapack — one command, multiple platform packages for a figoplay app.

An app is a standard app project dir (app.json + design + app.js [+ fonts/];
see the README "app 工程" section). This stages it and builds packages for the
targets buildable on Windows: win / web / android. (iOS/macOS need a Mac +
Xcode + codesign — out of scope here; the app.json `package` block is reserved
for them.)

    python tools/figmapack.py <app-dir> --target web
    python tools/figmapack.py examples/apps/sample --target all
    python tools/figmapack.py myapp -t android --out dist

Output lands in <out>/<app-slug>/<target>/. Metadata (package id / version /
name) comes from app.json's `package` block.

Notes
- web/android have no system fonts: the app must bundle the fonts its design
  uses in its fonts/ dir, or text renders blank. (Desktop uses system fonts.)
- a .fig design is converted with fig2json at pack time (web/android can't
  convert in-place); canvas.json / REST .json designs are staged as-is.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def log(msg):
    print(f"[figmapack] {msg}")


def die(msg):
    print(f"[figmapack] error: {msg}", file=sys.stderr)
    sys.exit(1)


def slug(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "app"


def resolve_icon(app_dir, m):
    """Absolute path to package.icon (a square PNG), or None."""
    icon = m.get("package", {}).get("icon")
    if not icon:
        return None
    p = os.path.join(app_dir, icon)
    if not os.path.isfile(p):
        die(f"package.icon not found: {p}")
    return os.path.abspath(p)


VCVARS = os.environ.get(
    "VCVARS",
    r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat")


def resize_png(src, dst, size):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        from PIL import Image
    except ImportError:  # no Pillow: ship the source as-is (browsers/android scale)
        shutil.copy(src, dst)
        return
    Image.open(src).convert("RGBA").resize((size, size), Image.LANCZOS).save(dst)


def make_ico(src, dst):
    """Multi-resolution .ico for the Windows exe."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    from PIL import Image  # Pillow required for .ico
    Image.open(src).convert("RGBA").save(
        dst, format="ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


def splash_color(m):
    """package.splashColor (#rrggbb) or a sensible default."""
    c = m.get("package", {}).get("splashColor", "#0b0e14")
    return c if re.match(r"^#[0-9a-fA-F]{6}$", c) else "#0b0e14"


def text_on(hex_color):
    """Readable text color (#fff/#111) for a given background."""
    r, g, b = (int(hex_color[i:i + 2], 16) for i in (1, 3, 5))
    return "#111418" if (0.299 * r + 0.587 * g + 0.114 * b) > 150 else "#ffffff"


def load_app(app_dir):
    mp = os.path.join(app_dir, "app.json")
    if not os.path.isfile(mp):
        die(f"no app.json in {app_dir}")
    m = json.load(open(mp, encoding="utf-8"))
    m.setdefault("design", "design.json")
    m.setdefault("script", "app.js")
    m.setdefault("name", os.path.basename(os.path.abspath(app_dir)))
    pkg = m.setdefault("package", {})
    pkg.setdefault("id", f"com.example.{slug(m['name'])}")
    pkg.setdefault("version", "1.0.0")
    return m


def version_code(ver):
    # "1.2.3" -> 10203  (android needs a monotonic integer)
    parts = (ver.split(".") + ["0", "0", "0"])[:3]
    try:
        a, b, c = (int(p) for p in parts)
        return a * 10000 + b * 100 + c
    except ValueError:
        return 1


def run_fig2json(fig_path, out_dir):
    exe = os.environ.get("FIGO_FIG2JSON",
                         os.path.join(ROOT, "..", "fig2json", "target", "release", "fig2json.exe"))
    if not os.path.isfile(exe):
        die(f"design is a .fig but fig2json not found at {exe} (set FIGO_FIG2JSON)")
    log(f"fig2json {os.path.basename(fig_path)} -> canvas.json")
    subprocess.check_call([exe, fig_path, out_dir])


def stage_app(app_dir, m, stage_root):
    """Copy the app into stage_root with a normalized, web/android-ready layout:
    app.json + design (canvas.json/REST json) + app.js + fonts/. Returns the
    staged app.json (possibly with design rewritten to canvas.json)."""
    stage = os.path.join(stage_root, slug(m["name"]))
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    sm = dict(m)

    design = os.path.join(app_dir, m["design"])
    if not os.path.exists(design):
        die(f"design not found: {design}")
    if design.lower().endswith(".fig"):
        run_fig2json(os.path.abspath(design), stage)  # writes canvas.json + images/
        sm["design"] = "canvas.json"
    else:
        shutil.copy(design, os.path.join(stage, os.path.basename(design)))
        sm["design"] = os.path.basename(design)

    script = os.path.join(app_dir, m["script"])
    if not os.path.isfile(script):
        die(f"script not found: {script}")
    shutil.copy(script, os.path.join(stage, "app.js"))
    sm["script"] = "app.js"

    fonts = m.get("fonts")
    if fonts and os.path.isdir(os.path.join(app_dir, fonts)):
        shutil.copytree(os.path.join(app_dir, fonts), os.path.join(stage, "fonts"))
        sm["fonts"] = "fonts"
    else:
        sm.pop("fonts", None)

    json.dump(sm, open(os.path.join(stage, "app.json"), "w", encoding="utf-8"), indent=2)
    return stage, sm


def cmd_wrapper(lines):
    """Run a cmd.exe script (for tools needing a sourced env, e.g. emsdk).
    Mirrors the vcvars wrapper pattern in CLAUDE.md."""
    bat = os.path.join(ROOT, "build", f"_figmapack_{os.getpid()}.cmd")
    os.makedirs(os.path.dirname(bat), exist_ok=True)
    with open(bat, "w", encoding="ascii") as f:
        f.write("\r\n".join(lines) + "\r\n")
    try:
        return subprocess.call(["cmd", "/c", bat])
    finally:
        os.remove(bat)


def embed_win_icon(ico):
    """Relink figoplay.exe with the app icon as a resource (reuses build/ — only
    figoplay relinks, seconds), then clear the cache var so the dev build is
    unaffected. Returns True on success."""
    build = os.path.join(ROOT, "build")
    if not os.path.isfile(os.path.join(build, "CMakeCache.txt")):
        log("win icon: no build/ cache — skipping embed (run cmake -B build once)")
        return False
    icop = ico.replace("\\", "/")
    rc = cmd_wrapper([
        f'call "{VCVARS}" >nul 2>&1',
        f'cd /d "{build}"',
        f'cmake -DFIGO_WIN_ICON={icop} . >nul',
        'cmake --build . --config Release --target figoplay',
    ])
    cmd_wrapper([  # restore: drop the icon resource from the dev build's cache
        f'call "{VCVARS}" >nul 2>&1',
        f'cd /d "{build}"',
        'cmake -DFIGO_WIN_ICON= . >nul',
    ])
    return rc == 0


# ----------------------------------------------------------------------- win --
def pack_win(stage, m, out, icon):
    exe = os.path.join(ROOT, "build", "figoplay.exe")
    if icon:  # embed the icon into the exe (else ship it alongside as icon.png)
        ico = os.path.join(os.path.dirname(stage), "app.ico")
        make_ico(icon, ico)
        if not embed_win_icon(ico):
            log("win icon: embed failed — shipping icon.png alongside")
    if not os.path.isfile(exe):
        die("build/figoplay.exe missing — build the desktop target first "
            "(cmake --build build --target figoplay)")
    dst = os.path.join(out, "win")
    os.makedirs(dst, exist_ok=True)
    shutil.copy(exe, os.path.join(dst, "figoplay.exe"))
    app_dst = os.path.join(dst, "app")
    if os.path.isdir(app_dst):
        shutil.rmtree(app_dst)
    shutil.copytree(stage, app_dst)
    with open(os.path.join(dst, "run.cmd"), "w", encoding="ascii") as f:
        f.write('@echo off\r\n"%~dp0figoplay.exe" "%~dp0app"\r\n')
    if icon:
        resize_png(icon, os.path.join(dst, "icon.png"), 256)
    log(f"win -> {os.path.relpath(dst, os.getcwd())}  (run.cmd)")


# ----------------------------------------------------------------------- web --
def pack_web(stage, m, out, icon):
    build_web = os.path.join(ROOT, "build_web")
    extra = ""
    for dep in ("raylib", "quickjs"):
        src = os.path.join(ROOT, "build", "_deps", f"{dep}-src", "CMakeLists.txt")
        if os.path.isfile(src):
            extra += f" -DFETCHCONTENT_SOURCE_DIR_{dep.upper()}={os.path.dirname(src).replace(chr(92),'/')}"
    app = stage.replace("\\", "/")
    emsdk = os.environ.get("EMSDK_HOME", "D:\\devlib\\emsdk")
    rc = cmd_wrapper([
        f'call "{emsdk}\\emsdk_env.bat" >nul 2>&1',
        f'cd /d "{ROOT}"',
        f'call emcmake cmake -B build_web -G Ninja -DCMAKE_BUILD_TYPE=Release '
        f'-DFIGO_WEB_APP_DIR={app}{extra}',
        'if errorlevel 1 exit /b 1',
        'ninja -C build_web figoplay',
    ])
    if rc:
        die("web build failed")
    dst = os.path.join(out, "web")
    os.makedirs(dst, exist_ok=True)
    for ext, name in (("html", "index.html"), ("js", "figoplay.js"),
                      ("wasm", "figoplay.wasm"), ("data", "figoplay.data")):
        src = os.path.join(build_web, f"figoplay.{ext}")
        if os.path.isfile(src):
            shutil.copy(src, os.path.join(dst, name))
    # Page title + favicon / apple-touch-icon.
    html_path = os.path.join(dst, "index.html")
    html = open(html_path, encoding="utf-8").read()
    title = (m["name"].replace("&", "&amp;").replace("<", "&lt;"))
    if re.search(r"<title>.*?</title>", html, re.S):
        html = re.sub(r"<title>.*?</title>", f"<title>{title}</title>", html,
                      count=1, flags=re.S)
    head = ""
    if icon:
        resize_png(icon, os.path.join(dst, "favicon.png"), 64)
        resize_png(icon, os.path.join(dst, "apple-touch-icon.png"), 180)
        head = ('<link rel="icon" type="image/png" href="favicon.png">'
                '<link rel="apple-touch-icon" href="apple-touch-icon.png">')
    if head and "</head>" in html:
        html = html.replace("</head>", head + "</head>", 1)
    # Branded splash over the wasm load (fades out shortly after window load).
    bg, fg = splash_color(m), text_on(splash_color(m))
    img = ('<img src="apple-touch-icon.png" alt="" style="width:96px;height:96px;'
           'border-radius:22px">') if icon else ""
    splash = (
        f'<div id="figmasplash" style="position:fixed;inset:0;z-index:99999;display:flex;'
        f'flex-direction:column;align-items:center;justify-content:center;background:{bg};'
        f'transition:opacity .45s">{img}'
        f'<div style="margin-top:18px;font:600 18px system-ui,-apple-system,sans-serif;'
        f'color:{fg}">{title}</div>'
        f'<div style="margin-top:22px;width:26px;height:26px;border-radius:50%;'
        f'border:3px solid {fg}40;border-top-color:{fg};animation:fpspin .8s linear infinite">'
        f'</div></div><style>@keyframes fpspin{{to{{transform:rotate(360deg)}}}}</style>')
    hide = ('<script>(function(){var s=document.getElementById("figmasplash");if(!s)return;'
            'function h(){s.style.opacity=0;setTimeout(function(){if(s.parentNode)'
            's.parentNode.removeChild(s)},500)}window.addEventListener("load",function(){'
            'setTimeout(h,800)})})();</script>')
    if "</body>" in html:
        html = html.replace("</body>", splash + hide + "</body>", 1)
    open(html_path, "w", encoding="utf-8").write(html)
    log(f"web -> {os.path.relpath(dst, os.getcwd())}  (serve index.html)")


# ------------------------------------------------------------------- android --
def pack_android(stage, m, out, icon):
    ps1 = os.path.join(ROOT, "tools", "build_android.ps1")
    pkg = m["package"]
    dst = os.path.join(out, "android")
    os.makedirs(dst, exist_ok=True)
    args = [
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1,
        "-AppDir", stage,
        "-PackageId", pkg["id"],
        "-AppName", m["name"],
        "-VersionName", pkg["version"],
        "-VersionCode", str(version_code(pkg["version"])),
        "-OutApk", os.path.join(dst, f"{slug(m['name'])}.apk"),
    ]
    if icon:  # res/mipmap-<density>/ic_launcher.png + a splash windowBackground
        res_dir = os.path.join(os.path.dirname(stage), "android-res")
        if os.path.isdir(res_dir):
            shutil.rmtree(res_dir)
        for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96),
                              ("xxhdpi", 144), ("xxxhdpi", 192)):
            resize_png(icon, os.path.join(res_dir, f"mipmap-{density}",
                                          "ic_launcher.png"), size)
        # Splash: a theme windowBackground (solid color + centered icon) shown
        # while the NativeActivity loads, before the first frame.
        os.makedirs(os.path.join(res_dir, "drawable"), exist_ok=True)
        os.makedirs(os.path.join(res_dir, "values"), exist_ok=True)
        with open(os.path.join(res_dir, "drawable", "splash.xml"), "w", encoding="utf-8") as f:
            f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                    '<layer-list xmlns:android="http://schemas.android.com/apk/res/android">\n'
                    f'  <item><shape android:shape="rectangle"><solid android:color="{splash_color(m)}"/></shape></item>\n'
                    '  <item><bitmap android:src="@mipmap/ic_launcher" android:gravity="center"/></item>\n'
                    '</layer-list>\n')
        with open(os.path.join(res_dir, "values", "styles.xml"), "w", encoding="utf-8") as f:
            f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                    '  <style name="AppSplash" parent="@android:style/Theme.NoTitleBar.Fullscreen">\n'
                    '    <item name="android:windowBackground">@drawable/splash</item>\n'
                    '  </style>\n</resources>\n')
        args += ["-ResDir", res_dir]
    rc = subprocess.call(args)
    if rc:
        die("android build failed")
    log(f"android -> {os.path.relpath(dst, os.getcwd())}")


TARGETS = {"win": pack_win, "web": pack_web, "android": pack_android}


def main():
    ap = argparse.ArgumentParser(description="Package a figoplay app for win/web/android.")
    ap.add_argument("app_dir", help="app project directory (with app.json)")
    ap.add_argument("--target", "-t", default="all",
                    help="win | web | android | all (comma-separated ok)")
    ap.add_argument("--out", "-o", default="dist", help="output dir (default: dist)")
    args = ap.parse_args()

    if not os.path.isdir(args.app_dir):
        die(f"not a directory: {args.app_dir}")
    targets = list(TARGETS) if args.target == "all" else \
        [t.strip() for t in args.target.split(",")]
    for t in targets:
        if t not in TARGETS:
            die(f"unknown target '{t}'. choices: {', '.join(TARGETS)}, all")

    m = load_app(args.app_dir)
    icon = resolve_icon(args.app_dir, m)
    out = os.path.join(os.path.abspath(args.out), slug(m["name"]))
    os.makedirs(out, exist_ok=True)
    log(f"app '{m['name']}'  (id {m['package']['id']} v{m['package']['version']})  "
        f"targets: {', '.join(targets)}{'  +icon' if icon else ''}")
    stage, sm = stage_app(args.app_dir, m, os.path.join(out, ".stage"))
    log(f"staged -> {os.path.relpath(stage, os.getcwd())}  (design: {sm['design']}"
        f"{', fonts' if sm.get('fonts') else ', NO fonts — text may be blank on web/android'})")

    for t in targets:
        TARGETS[t](stage, m, out, icon)
    log(f"done -> {os.path.relpath(out, os.getcwd())}")


if __name__ == "__main__":
    main()
