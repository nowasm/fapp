#!/usr/bin/env node
// html2godot — one command: a React/HTML page -> a Godot 4 project.
// Runs web2canvas (DOM -> canvas.json + images) then fapp2godot (canvas.json
// -> .tscn + sprites + manifest), wiring the intermediate paths together.
//
//   node html2godot.js <url|file.html> --out <godotDir>
//        [--states "a,b,c"] [--fonts DIR] [--root SEL] [--viewport WxH]
//        [--wait MS] [--browser msedge|chrome] [--fapp2godot <exe>]
//
// Result: <godotDir>/ is an openable Godot project — one .tscn per screen,
// deduped sprites, bundled fonts, manifest.json, project.godot.

const path = require('path');
const fs = require('fs');
const { execFileSync } = require('child_process');

function arg(name, def) {
  const i = process.argv.indexOf(name);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}
const input = process.argv[2];
const out = arg('--out', null);
if (!input || input.startsWith('-') || !out) {
  console.error('usage: html2godot <url|file.html> --out <godotDir> [--states ...] [--fonts DIR] [--root SEL] [--viewport WxH] [--wait MS] [--browser ...] [--fapp2godot <exe>]');
  process.exit(2);
}

const here = __dirname;
const inter = path.join(path.resolve(out), '.web2canvas');
fs.mkdirSync(inter, { recursive: true });
const canvas = path.join(inter, 'design.canvas.json');

// default fapp2godot: the repo's build output (tools/web2canvas -> ../../build_godot)
let fapp2godot = arg('--fapp2godot', null);
if (!fapp2godot) {
  for (const c of ['build_godot/fapp2godot.exe', 'build/fapp2godot.exe', 'build_godot/fapp2godot']) {
    const p = path.resolve(here, '..', '..', c);
    if (fs.existsSync(p)) { fapp2godot = p; break; }
  }
}
if (!fapp2godot || !fs.existsSync(fapp2godot)) {
  console.error('FAIL: fapp2godot executable not found; pass --fapp2godot <path>');
  process.exit(1);
}

const fonts = arg('--fonts', null);
const passthrough = [];
for (const k of ['--states', '--root', '--viewport', '--wait', '--browser', '--scale']) {
  const v = arg(k, null);
  if (v != null) passthrough.push(k, v);
}
if (fonts) passthrough.push('--fonts', fonts);

console.log('=== web2canvas ===');
execFileSync(process.execPath, [path.join(here, 'index.js'), input, '-o', canvas, ...passthrough],
             { stdio: 'inherit' });

console.log('=== fapp2godot ===');
const f2gArgs = [canvas, path.resolve(out)];
if (fonts) f2gArgs.push('--fonts', fonts);
execFileSync(fapp2godot, f2gArgs, { stdio: 'inherit' });

console.log(`\nRESULT: OK -> ${path.resolve(out)} (open in Godot 4)`);
