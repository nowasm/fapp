#!/usr/bin/env python3
"""Run the benchmark app suite (ROADMAP phase 1).

Every app dir under examples/apps/ whose app.json has "benchmark": true is a
benchmark app. Contract: run with `figoplay <dir> --selfdrive <prefix>`; the
app's SELFDRIVE branch drives itself with ui.tap and must print exactly one
line "BENCH: PASS" or "BENCH: FAIL" (plus any "bench check ..." detail lines).
Screenshots land at <prefix>_home.png / <prefix>_nav.png for eyeballing.

Usage:  python tools/bench.py [--app <name>] [--keep-shots]
Exit code = number of failing apps.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
APPS = REPO / "examples" / "apps"
EXE = REPO / "build" / ("figoplay.exe" if sys.platform == "win32" else "figoplay")


def find_benchmarks(only=None):
    for d in sorted(APPS.iterdir()):
        manifest = d / "app.json"
        if not manifest.is_file():
            continue
        try:
            meta = json.loads(manifest.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            continue
        if meta.get("benchmark") and (only is None or d.name == only):
            yield d


def run_app(app_dir: Path, shots_dir: Path) -> tuple[str, str]:
    # raylib TakeScreenshot drops directory components, so shots always land
    # in the cwd — run inside shots_dir and pass a bare basename prefix.
    try:
        proc = subprocess.run(
            [str(EXE), str(app_dir), "--selfdrive", app_dir.name],
            capture_output=True, text=True, timeout=120, cwd=str(shots_dir))
    except subprocess.TimeoutExpired:
        return "TIMEOUT", ""
    out = proc.stdout + proc.stderr
    detail = "\n".join(l for l in out.splitlines() if "bench check" in l)
    if "BENCH: PASS" in out:
        return "PASS", detail
    if "BENCH: FAIL" in out:
        return "FAIL", detail
    return f"NO-VERDICT (exit {proc.returncode})", detail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", help="run a single benchmark app by dir name")
    ap.add_argument("--keep-shots", action="store_true",
                    help="keep screenshots in bench_shots/ instead of a temp dir")
    args = ap.parse_args()

    if not EXE.is_file():
        sys.exit(f"figoplay not built: {EXE}")

    apps = list(find_benchmarks(args.app))
    if not apps:
        sys.exit("no benchmark apps found (app.json needs \"benchmark\": true)")

    shots_dir = (REPO / "bench_shots") if args.keep_shots else Path(tempfile.mkdtemp())
    shots_dir.mkdir(exist_ok=True)

    failures = 0
    for app_dir in apps:
        verdict, detail = run_app(app_dir, shots_dir)
        ok = verdict == "PASS"
        failures += 0 if ok else 1
        print(f"[{'ok' if ok else 'XX'}] {app_dir.name}: {verdict}")
        if detail and not ok:
            print("     " + detail.replace("\n", "\n     "))
    print(f"\n{len(apps) - failures}/{len(apps)} passed"
          + (f", shots in {shots_dir}" if args.keep_shots else ""))
    sys.exit(failures)


if __name__ == "__main__":
    main()
