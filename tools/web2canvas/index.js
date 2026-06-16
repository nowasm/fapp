#!/usr/bin/env node
// web2canvas — render a React/HTML page in a headless browser and convert the
// computed DOM into figmalib canvas.json (the same format fig2json emits and
// figmalib / fapp2godot consume).
//
//   node index.js <url|file.html> [-o out.canvas.json] [--root SELECTOR]
//                 [--viewport WxH] [--browser msedge|chrome] [--wait MS]
//
// MVP scope: boxes (solid background, border, corner radius, drop shadow),
// text (font family/size/weight/color/alignment), and the box hierarchy with
// clip (overflow:hidden). Gradients, images, inline SVG and clip-path polygons
// come next (see plan).

const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright-core');

// ---- CLI ------------------------------------------------------------------

function parseArgs(argv) {
  const a = { input: null, out: null, root: 'body', vw: 1280, vh: 720,
              browser: 'msedge', wait: 400 };
  for (let i = 2; i < argv.length; i++) {
    const t = argv[i];
    if (t === '-o' || t === '--out') a.out = argv[++i];
    else if (t === '--root') a.root = argv[++i];
    else if (t === '--viewport') { const m = /(\d+)x(\d+)/.exec(argv[++i] || ''); if (m) { a.vw = +m[1]; a.vh = +m[2]; } }
    else if (t === '--browser') a.browser = argv[++i];
    else if (t === '--wait') a.wait = +argv[++i];
    else if (!t.startsWith('-')) a.input = t;
  }
  return a;
}

function toUrl(input) {
  if (/^https?:\/\//.test(input) || input.startsWith('file:')) return input;
  return 'file:///' + path.resolve(input).replace(/\\/g, '/');
}

// ---- browser-side collector ----------------------------------------------
// Serialized into the page; returns a plain tree of {rect, text, style, kids}.
// Keeps raw computed-style strings — the Node side normalizes them.

function collectorFn(rootSelector) {
  const root = document.querySelector(rootSelector) || document.body;

  function visible(el, cs) {
    if (cs.display === 'none' || cs.visibility === 'hidden') return false;
    if (parseFloat(cs.opacity) === 0) return false;
    const r = el.getBoundingClientRect();
    return r.width >= 1 && r.height >= 1;
  }

  // Direct (non-whitespace) text with no element children -> a text leaf.
  function directText(el) {
    if (el.children.length > 0) return null;
    const t = (el.textContent || '').replace(/\s+/g, ' ').trim();
    return t.length ? t : null;
  }

  function node(el, originX, originY) {
    const cs = getComputedStyle(el);
    if (!visible(el, cs)) return null;
    const r = el.getBoundingClientRect();
    const text = directText(el);

    const out = {
      tag: el.tagName.toLowerCase(),
      rect: { x: r.left - originX, y: r.top - originY, w: r.width, h: r.height },
      text,
      bg: cs.backgroundColor,
      bgImage: cs.backgroundImage,
      radius: [cs.borderTopLeftRadius, cs.borderTopRightRadius,
               cs.borderBottomRightRadius, cs.borderBottomLeftRadius].map(v => parseFloat(v) || 0),
      borderW: parseFloat(cs.borderTopWidth) || 0,
      borderColor: cs.borderTopColor,
      borderStyle: cs.borderTopStyle,
      shadow: cs.boxShadow,
      opacity: parseFloat(cs.opacity),
      transform: cs.transform,
      overflow: cs.overflow,
      // text-only fields
      color: cs.color,
      fontFamily: cs.fontFamily,
      fontSize: parseFloat(cs.fontSize) || 0,
      fontWeight: cs.fontWeight,
      fontStyle: cs.fontStyle,
      textAlign: cs.textAlign,
      lineHeight: cs.lineHeight,
      letterSpacing: cs.letterSpacing,
      kids: [],
    };

    if (!text) {
      for (const child of el.children) {
        const n = node(child, originX, originY);
        if (n) out.kids.push(n);
      }
    }
    return out;
  }

  const rr = root.getBoundingClientRect();
  const tree = node(root, rr.left, rr.top);
  return { tree, rootW: rr.width, rootH: rr.height };
}

// ---- Node-side mapping: collector tree -> canvas.json ---------------------

// "rgb(r, g, b)" / "rgba(r, g, b, a)" -> { hex, alpha }.
function parseColor(s) {
  if (!s) return null;
  const m = /rgba?\(([^)]+)\)/.exec(s);
  if (!m) return null;
  const p = m[1].split(',').map(x => x.trim());
  const r = Math.round(parseFloat(p[0])), g = Math.round(parseFloat(p[1])), b = Math.round(parseFloat(p[2]));
  const a = p[3] !== undefined ? parseFloat(p[3]) : 1;
  if (a === 0) return null;  // fully transparent → no paint
  const hex = '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('');
  return { hex, alpha: a };
}

function solidPaint(colorStr) {
  const c = parseColor(colorStr);
  if (!c) return null;
  const p = { type: 'SOLID', color: c.hex };
  if (c.alpha < 1) p.opacity = c.alpha;
  return p;
}

// "10px 4px 22px 0px rgba(0,0,0,0.3)" -> a DROP_SHADOW effect (first shadow).
function parseShadow(s) {
  if (!s || s === 'none') return null;
  // split on the color, then read the numeric offsets/blur/spread
  const colorMatch = /rgba?\([^)]+\)|#[0-9a-fA-F]{3,8}/.exec(s);
  const color = colorMatch ? parseColor(colorMatch[0]) : { hex: '#000000', alpha: 0.25 };
  const nums = (s.replace(/rgba?\([^)]+\)/, '').match(/-?\d*\.?\d+px/g) || []).map(parseFloat);
  if (nums.length < 2) return null;
  const [ox = 0, oy = 0, blur = 0, spread = 0] = nums;
  const hex = (color && color.hex) || '#000000';
  return {
    type: 'DROP_SHADOW',
    color: hex + Math.round(((color ? color.alpha : 0.25)) * 255).toString(16).padStart(2, '0'),
    offset: { x: ox, y: oy }, radius: blur, spread,
  };
}

function rotationDeg(transform) {
  if (!transform || transform === 'none') return 0;
  const m = /matrix\(([^)]+)\)/.exec(transform);
  if (!m) return 0;
  const v = m[1].split(',').map(parseFloat);
  return Math.atan2(v[1], v[0]) * 180 / Math.PI;
}

let nameCounter = 0;
function mapNode(n, parent) {
  const px = parent ? parent.rect.x : n.rect.x;
  const py = parent ? parent.rect.y : n.rect.y;
  const tx = +(n.rect.x - px).toFixed(2);
  const ty = +(n.rect.y - py).toFixed(2);

  const node = {
    name: n.text ? n.text.slice(0, 24) : (n.tag + '_' + (nameCounter++)),
    transform: { x: parent ? tx : 0, y: parent ? ty : 0 },
    size: { x: +n.rect.w.toFixed(2), y: +n.rect.h.toFixed(2) },
  };
  const rot = rotationDeg(n.transform);
  if (Math.abs(rot) > 0.1) node.transform.rotation = +rot.toFixed(2);

  if (n.text) {
    node.type = 'TEXT';
    node.textData = { characters: n.text };
    const fam = (n.fontFamily || 'Inter').split(',')[0].replace(/['"]/g, '').trim();
    node.fontName = { family: fam };
    node.fontSize = n.fontSize;
    const w = parseInt(n.fontWeight, 10);
    if (!isNaN(w)) node.fontWeight = w;
    if (/px/.test(n.lineHeight)) node.lineHeight = n.lineHeight;
    node.textAlignHorizontal = (n.textAlign || 'left').toUpperCase();
    node.textAutoResize = 'HEIGHT';
    const fp = solidPaint(n.color);
    if (fp) node.fillPaints = [fp];
  } else {
    node.type = 'FRAME';
    const fp = solidPaint(n.bg);
    if (fp) node.fillPaints = [fp];
    const sp = (n.borderW > 0 && n.borderStyle !== 'none') ? solidPaint(n.borderColor) : null;
    if (sp) { node.strokePaints = [sp]; node.strokeWeight = n.borderW; node.strokeAlign = 'INSIDE'; }
    const r = n.radius || [0, 0, 0, 0];
    if (r.some(v => v > 0)) {
      if (r.every(v => v === r[0])) node.cornerRadius = r[0];
      else { node.topLeftRadius = r[0]; node.topRightRadius = r[1]; node.bottomRightRadius = r[2]; node.bottomLeftRadius = r[3]; }
    }
    const eff = parseShadow(n.shadow);
    if (eff) node.effects = [eff];
    // overflow:hidden / clip → Figma frame clip (frameMaskDisabled === false)
    node.frameMaskDisabled = !(n.overflow && n.overflow !== 'visible');
  }
  if (n.opacity < 0.999) node.opacity = n.opacity;

  if (n.kids && n.kids.length) node.children = n.kids.map(k => mapNode(k, n));
  return node;
}

// ---- main -----------------------------------------------------------------

(async () => {
  const a = parseArgs(process.argv);
  if (!a.input) { console.error('usage: web2canvas <url|file.html> [-o out] [--root SEL] [--viewport WxH] [--browser msedge|chrome]'); process.exit(2); }
  const out = a.out || a.input.replace(/\.[^.]+$/, '') + '.canvas.json';

  console.log(`launching ${a.browser} ...`);
  const browser = await chromium.launch({ channel: a.browser, headless: true });
  const page = await browser.newPage({ viewport: { width: a.vw, height: a.vh },
                                       deviceScaleFactor: 1 });
  console.log(`loading ${toUrl(a.input)}`);
  await page.goto(toUrl(a.input), { waitUntil: 'networkidle' }).catch(() => {});
  await page.waitForTimeout(a.wait);
  // optional reference screenshot next to the output
  await page.screenshot({ path: out.replace(/\.canvas\.json$|\.json$/, '') + '.web.png' }).catch(() => {});

  const { tree, rootW, rootH } = await page.evaluate(collectorFn, a.root);
  await browser.close();
  if (!tree) { console.error('FAIL: nothing collected from root ' + a.root); process.exit(1); }

  nameCounter = 0;
  const rootFrame = mapNode(tree, null);
  rootFrame.name = rootFrame.name || 'Page';
  rootFrame.scrollDirection = 'VERTICAL';

  const doc = {
    document: {
      type: 'DOCUMENT',
      children: [{ type: 'CANVAS', name: 'Page 1', children: [rootFrame] }],
    },
    styles: {},
  };
  fs.writeFileSync(out, JSON.stringify(doc, null, 2));
  console.log(`RESULT: OK  ${rootW}x${rootH}  -> ${out}`);
})().catch(e => { console.error('FAIL:', e.message); process.exit(1); });
