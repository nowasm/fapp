#!/usr/bin/env python3
"""figmanew — scaffold a new figoplay app project from a template.

An app is a directory of {app.json, design.json, app.js} (see the README's
"app 工程" section). This copies a template and personalizes its app.json, so
AI / humans start from a working app and edit, rather than generate from zero.

    python tools/figmanew.py myapp --template tab-shell
    python tools/figmanew.py myapp --template list-detail --name "My Wallet" \
        --design-system revolut
    python tools/figmanew.py --list

Then run it:  figoplay myapp
"""
import argparse
import json
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATES_DIR = os.path.join(ROOT, "templates")
SYSTEMS_DIR = os.path.join(ROOT, "design-systems")


def list_templates():
    if not os.path.isdir(TEMPLATES_DIR):
        return []
    return sorted(d for d in os.listdir(TEMPLATES_DIR)
                  if os.path.isfile(os.path.join(TEMPLATES_DIR, d, "app.json")))


def slug(name):
    s = re.sub(r"[^a-z0-9]+", "", name.lower())
    return s or "app"


def main():
    ap = argparse.ArgumentParser(description="Scaffold a figoplay app project.")
    ap.add_argument("dest", nargs="?", help="new app directory to create")
    ap.add_argument("--template", "-t", help="template name (see --list)")
    ap.add_argument("--name", "-n", help="app display name (default: dest basename)")
    ap.add_argument("--design-system", "-d",
                    help="override designSystem (see design-systems/)")
    ap.add_argument("--list", "-l", action="store_true", help="list templates and exit")
    args = ap.parse_args()

    templates = list_templates()
    if args.list or (not args.dest and not args.template):
        print("templates:")
        for t in templates:
            meta = json.load(open(os.path.join(TEMPLATES_DIR, t, "app.json"), encoding="utf-8"))
            print(f"  {t:14} {meta.get('name','')}  (designSystem: {meta.get('designSystem','-')})")
        return 0

    if not args.dest or not args.template:
        ap.error("need both <dest> and --template (or use --list)")
    if args.template not in templates:
        ap.error(f"unknown template '{args.template}'. choices: {', '.join(templates)}")
    if args.design_system and not os.path.isdir(os.path.join(SYSTEMS_DIR, args.design_system)):
        ap.error(f"unknown design system '{args.design_system}'. see design-systems/")

    dest = os.path.abspath(args.dest)
    if os.path.exists(dest) and os.listdir(dest):
        ap.error(f"destination '{args.dest}' exists and is not empty")

    shutil.copytree(os.path.join(TEMPLATES_DIR, args.template), dest, dirs_exist_ok=True)

    name = args.name or os.path.basename(dest.rstrip(os.sep))
    manifest_path = os.path.join(dest, "app.json")
    m = json.load(open(manifest_path, encoding="utf-8"))
    m["name"] = name
    m.setdefault("package", {})["id"] = f"com.example.{slug(name)}"
    if args.design_system:
        m["designSystem"] = args.design_system
    json.dump(m, open(manifest_path, "w", encoding="utf-8"), indent=2)

    rel = os.path.relpath(dest, os.getcwd())
    print(f"created '{name}' from template '{args.template}' at {rel}")
    print(f"  run:  figoplay {rel}")
    print(f"  edit: {rel}/app.js  +  the design via figoedit "
          f"(designSystem: {m.get('designSystem','-')})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
