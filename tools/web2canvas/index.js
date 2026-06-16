#!/usr/bin/env node
// web2canvas — render a React/HTML page in a headless browser and convert the
// computed DOM into figmalib canvas.json (the same format fig2json emits and
// figmalib / fapp2godot consume).
//
//   node index.js <url|file.html> [-o out.canvas.json] [--root SELECTOR]
//                 [--viewport WxH] [--browser msedge|chrome] [--wait MS]
//                 [--scale N]
//
// Solid boxes, borders, corner radius, drop shadow and text map to native
// canvas.json nodes; direct text is measured precisely (a Range over the text
// node) so an element with BOTH a box and text keeps both (box FRAME + TEXT
// child). Anything a flat fill can't reproduce — CSS gradients, background-
// images, <img>, inline <svg>, clip-path cut-corners — is captured as a
// per-element PNG and emitted as an IMAGE fill.

const fs = require('fs');
const path = require('path');
const http = require('http');
const { chromium } = require('playwright-core');

// ---- CLI ------------------------------------------------------------------

function parseArgs(argv) {
  const a = { input: null, out: null, root: 'body', vw: 1280, vh: 720,
              browser: 'msedge', wait: 400, scale: 2, fonts: null, states: null, navFn: '__nav' };
  for (let i = 2; i < argv.length; i++) {
    const t = argv[i];
    if (t === '-o' || t === '--out') a.out = argv[++i];
    else if (t === '--root') a.root = argv[++i];
    else if (t === '--viewport') { const m = /(\d+)x(\d+)/.exec(argv[++i] || ''); if (m) { a.vw = +m[1]; a.vh = +m[2]; } }
    else if (t === '--browser') a.browser = argv[++i];
    else if (t === '--wait') a.wait = +argv[++i];
    else if (t === '--scale') a.scale = Math.max(1, +argv[++i]);
    else if (t === '--fonts') a.fonts = argv[++i];
    else if (t === '--states') a.states = argv[++i];
    else if (t === '--nav-fn') a.navFn = argv[++i];
    else if (!t.startsWith('-')) a.input = t;
  }
  return a;
}

const MIME = { '.js': 'application/javascript', '.mjs': 'application/javascript',
               '.jsx': 'application/javascript', '.css': 'text/css', '.json': 'application/json',
               '.map': 'application/json', '.html': 'text/html', '.htm': 'text/html',
               '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
               '.gif': 'image/gif', '.webp': 'image/webp', '.svg': 'image/svg+xml',
               '.ttf': 'font/ttf', '.otf': 'font/otf', '.woff': 'font/woff', '.woff2': 'font/woff2',
               '.mp4': 'video/mp4' };

// Babel-in-browser fetches .jsx via XHR, which file:// blocks (CORS). Serve the
// input's directory over a throwaway local HTTP server so real React apps load.
function startStaticServer(rootDir) {
  const base = path.resolve(rootDir);
  return new Promise((resolve) => {
    const server = http.createServer((req, res) => {
      try {
        const rel = decodeURIComponent((req.url || '/').split('?')[0]);
        const file = path.normalize(path.join(base, rel));
        if (!file.startsWith(base)) { res.writeHead(403); return res.end(); }
        const body = fs.readFileSync(file);
        res.writeHead(200, { 'content-type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
                             'access-control-allow-origin': '*' });
        res.end(body);
      } catch (e) { res.writeHead(404); res.end('not found'); }
    });
    server.listen(0, '127.0.0.1', () => resolve(server));
  });
}

// Serve CDN scripts from vendored node_modules (byte-identical → SRI passes),
// abort web fonts so networkidle settles (figmalib supplies its own fonts).
async function setupCdnRoutes(page) {
  const nm = path.join(__dirname, 'node_modules');
  await page.route(/(unpkg\.com|cdn\.jsdelivr\.net\/npm)\//, (route) => {
    try {
      const u = new URL(route.request().url());
      const segs = u.pathname.replace(/^\/(npm\/)?/, '').split('/');
      let pkg, rest;
      if (segs[0].startsWith('@')) { pkg = segs[0] + '/' + segs[1].replace(/@.*/, ''); rest = segs.slice(2).join('/'); }
      else { pkg = segs[0].replace(/@.*/, ''); rest = segs.slice(1).join('/'); }
      const file = path.join(nm, pkg, rest);
      route.fulfill({ status: 200, contentType: MIME[path.extname(file)] || 'application/octet-stream', body: fs.readFileSync(file) });
    } catch (e) { route.abort(); }
  });
  await page.route(/fonts\.(googleapis|gstatic)\.com/, r => r.abort());
}

// ---- browser-side collector ----------------------------------------------

function collectorFn(rootSelector) {
  const root = document.querySelector(rootSelector) || document.body;
  let rid = 0;
  // Normalize any CSS color (incl. oklch/oklab/color-mix, which getComputedStyle
  // and canvas fillStyle preserve as-is) to plain rgba by rasterizing one pixel
  // and reading it back — forces the browser's own sRGB conversion.
  const ncx = document.createElement('canvas').getContext('2d', { willReadFrequently: true });
  function norm(c) {
    try {
      ncx.clearRect(0, 0, 1, 1);
      ncx.fillStyle = c;
      ncx.fillRect(0, 0, 1, 1);
      const d = ncx.getImageData(0, 0, 1, 1).data;
      return `rgba(${d[0]}, ${d[1]}, ${d[2]}, ${(d[3] / 255).toFixed(3)})`;
    } catch (e) { return c; }
  }

  function visible(el, cs) {
    if (cs.display === 'none' || cs.visibility === 'hidden') return false;
    if (parseFloat(cs.opacity) === 0) return false;
    const r = el.getBoundingClientRect();
    return r.width >= 1 && r.height >= 1;
  }
  function hasRealBg(cs) { return cs.backgroundImage && cs.backgroundImage !== 'none'; }
  // A uniform solid border maps to a Figma stroke; anything else (dashed,
  // dotted, or only some sides — e.g. a border-bottom separator) can't, so
  // rasterize the element to keep it faithful.
  // Parse a box-shadow / filter:drop-shadow into a structured effect with the
  // color normalized (handles oklch glows that a plain rgb parser would drop).
  function splitTopComma(s) {
    const out = []; let d = 0, last = 0;
    for (let i = 0; i < s.length; i++) { const c = s[i]; if (c === '(') d++; else if (c === ')') d--; else if (c === ',' && d === 0) { out.push(s.slice(last, i)); last = i + 1; } }
    out.push(s.slice(last)); return out;
  }
  function colorOf(str) { const c = str.replace(/\binset\b/g, '').trim(); return c ? norm(c) : null; }
  function shadowFromBox(s) {
    if (!s || s === 'none') return null;
    s = splitTopComma(s)[0];
    const nums = (s.match(/-?[\d.]+px/g) || []).map(parseFloat);
    if (nums.length < 2) return null;
    const color = colorOf(s.replace(/-?[\d.]+px/g, ''));
    if (!color) return null;
    const [ox = 0, oy = 0, blur = 0, spread = 0] = nums;
    return { ox, oy, blur, spread, color };
  }
  function shadowFromFilter(f) {
    if (!f || f === 'none') return null;
    const i = f.indexOf('drop-shadow(');
    if (i < 0) return null;
    let depth = 0, start = i + 12, j = start;
    for (; j < f.length; j++) { if (f[j] === '(') depth++; else if (f[j] === ')') { if (depth === 0) break; depth--; } }
    const inner = f.slice(start, j);
    const nums = (inner.match(/-?[\d.]+px/g) || []).map(parseFloat);
    if (nums.length < 2) return null;
    const color = colorOf(inner.replace(/-?[\d.]+px/g, '')) || 'rgba(0,0,0,0.5)';
    const [ox = 0, oy = 0, blur = 0] = nums;
    return { ox, oy, blur, spread: 0, color };
  }
  function fancyBorder(cs) {
    const sides = ['Top', 'Right', 'Bottom', 'Left'];
    const w0 = parseFloat(cs.borderTopWidth) || 0, c0 = cs.borderTopColor;
    let present = 0, uniformSolid = true;
    for (const s of sides) {
      const w = parseFloat(cs['border' + s + 'Width']) || 0, st = cs['border' + s + 'Style'];
      if (w > 0 && st !== 'none') present++;
      if (!(w === w0 && st === 'solid' && cs['border' + s + 'Color'] === c0)) uniformSolid = false;
    }
    return present > 0 && !uniformSolid;
  }
  // getBoundingClientRect gives a rotated/skewed/scaled element's axis-aligned
  // bbox, losing its real shape (a thin rotated line becomes a square). Such
  // elements must be rasterized so the browser draws them correctly.
  function isTransformed(cs) {
    const t = cs.transform;
    if (!t || t === 'none') return false;
    const m = /matrix\(([^)]+)\)/.exec(t);
    if (!m) return false;
    const v = m[1].split(',').map(parseFloat);  // a,b,c,d,e,f
    return Math.abs(v[0] - 1) > 0.01 || Math.abs(v[1]) > 0.01 ||
           Math.abs(v[2]) > 0.01 || Math.abs(v[3] - 1) > 0.01;  // non-translation
  }

  // Exact rect of an element's own (non-whitespace) text, via a Range — handles
  // padding, alignment and wrapping. Returns null when there is no direct text.
  function measureDirectText(el, ox, oy) {
    let text = '', rects = [];
    for (const ch of el.childNodes) {
      if (ch.nodeType === 3 && ch.textContent && ch.textContent.trim()) {
        const r = document.createRange();
        r.selectNodeContents(ch);
        const rc = r.getBoundingClientRect();
        if (rc.width >= 1 && rc.height >= 1) { rects.push(rc); text += ch.textContent; }
      }
    }
    if (!rects.length) return null;
    const x0 = Math.min(...rects.map(r => r.left)), y0 = Math.min(...rects.map(r => r.top));
    const x1 = Math.max(...rects.map(r => r.right)), y1 = Math.max(...rects.map(r => r.bottom));
    return { text: text.replace(/\s+/g, ' ').trim(), rect: { x: x0 - ox, y: y0 - oy, w: x1 - x0, h: y1 - y0 } };
  }

  function node(el, ox, oy) {
    const cs = getComputedStyle(el);
    if (!visible(el, cs)) return null;
    const r = el.getBoundingClientRect();
    const tag = el.tagName.toLowerCase();
    const ti = measureDirectText(el, ox, oy);

    const wholeRaster = (tag === 'img' || tag === 'svg' || tag === 'canvas' || tag === 'video') || isTransformed(cs);
    const clipped = cs.clipPath && cs.clipPath !== 'none';
    const bgRaster = hasRealBg(cs) || clipped || fancyBorder(cs);
    let raster = null, rasterWhole = false, rasterHideContent = false;
    if (wholeRaster) { raster = ++rid; rasterWhole = true; }
    else if (bgRaster) { raster = ++rid; rasterHideContent = true; }
    if (raster) el.setAttribute('data-w2c', String(raster));

    const out = {
      tag,
      rect: { x: r.left - ox, y: r.top - oy, w: r.width, h: r.height },
      text: ti ? ti.text : null,
      textRect: ti ? ti.rect : null,
      bg: norm(cs.backgroundColor),
      radius: [cs.borderTopLeftRadius, cs.borderTopRightRadius,
               cs.borderBottomRightRadius, cs.borderBottomLeftRadius].map(v => parseFloat(v) || 0),
      borderW: parseFloat(cs.borderTopWidth) || 0,
      borderColor: norm(cs.borderTopColor),
      borderStyle: cs.borderTopStyle,
      effect: shadowFromBox(cs.boxShadow) || shadowFromFilter(cs.filter),
      opacity: parseFloat(cs.opacity),
      transform: cs.transform,
      overflow: cs.overflow,
      color: norm(cs.color),
      fontFamily: cs.fontFamily,
      fontSize: parseFloat(cs.fontSize) || 0,
      fontWeight: cs.fontWeight,
      textAlign: cs.textAlign,
      lineHeight: cs.lineHeight,
      raster, rasterWhole, rasterHideContent,
      kids: [],
    };
    if (!rasterWhole) {
      for (const child of el.children) {
        const n = node(child, ox, oy);
        if (n) out.kids.push(n);
      }
    }
    return out;
  }

  const rr = root.getBoundingClientRect();
  const tree = node(root, rr.left, rr.top);
  return { tree, rootW: rr.width, rootH: rr.height };
}

// Hide a tagged element's content (children + its own text) for a bg-only shot.
function setBgOnlyFn({ id, on }) {
  const el = document.querySelector(`[data-w2c="${id}"]`);
  if (!el) return;
  if (on) {
    el.setAttribute('data-w2c-c', el.style.color || ' ');
    el.style.color = 'transparent';
    for (const c of el.children) c.style.visibility = 'hidden';
  } else {
    const saved = el.getAttribute('data-w2c-c');
    el.style.color = (saved === ' ' || saved == null) ? '' : saved;
    el.removeAttribute('data-w2c-c');
    for (const c of el.children) c.style.visibility = '';
  }
}

// ---- Node-side mapping ----------------------------------------------------

function parseColor(s) {
  if (!s) return null;
  s = s.trim();
  if (s[0] === '#') {  // #rgb / #rrggbb / #rrggbbaa (canvas-normalized opaque colors)
    let h = s.slice(1);
    if (h.length === 3) h = h.split('').map(c => c + c).join('');
    if (h.length !== 6 && h.length !== 8) return null;
    const r = parseInt(h.slice(0, 2), 16), g = parseInt(h.slice(2, 4), 16), b = parseInt(h.slice(4, 6), 16);
    const a = h.length === 8 ? parseInt(h.slice(6, 8), 16) / 255 : 1;
    if (a === 0) return null;
    return { hex: '#' + h.slice(0, 6).toLowerCase(), alpha: a };
  }
  const m = /rgba?\(([^)]+)\)/.exec(s);
  if (!m) return null;
  const p = m[1].split(',').map(x => x.trim());
  const r = Math.round(parseFloat(p[0])), g = Math.round(parseFloat(p[1])), b = Math.round(parseFloat(p[2]));
  const a = p[3] !== undefined ? parseFloat(p[3]) : 1;
  if (a === 0) return null;
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
function rotationDeg(transform) {
  if (!transform || transform === 'none') return 0;
  const m = /matrix\(([^)]+)\)/.exec(transform);
  if (!m) return 0;
  const v = m[1].split(',').map(parseFloat);
  return Math.atan2(v[1], v[0]) * 180 / Math.PI;
}

let nameCounter = 0;
let statePrefix = '';  // per-screen raster filename namespace (multi-state)
function makeTextNode(n, base) {
  const tr = n.textRect;
  const node = {
    name: (n.text || 'text').slice(0, 24),
    type: 'TEXT',
    transform: { x: +(tr.x - base.x).toFixed(2), y: +(tr.y - base.y).toFixed(2) },
    size: { x: +tr.w.toFixed(2), y: +tr.h.toFixed(2) },
    textData: { characters: n.text },
  };
  const fam = (n.fontFamily || 'Inter').split(',')[0].replace(/['"]/g, '').trim();
  node.fontName = { family: fam };
  node.fontSize = n.fontSize;
  const w = parseInt(n.fontWeight, 10);
  if (!isNaN(w)) node.fontWeight = w;
  if (/px/.test(n.lineHeight)) node.lineHeight = n.lineHeight;
  const al = (n.textAlign || 'left').toLowerCase();
  node.textAlignHorizontal = al === 'start' ? 'LEFT' : al === 'end' ? 'RIGHT'
    : al === 'justify' ? 'JUSTIFIED' : al.toUpperCase();
  // Only mark wrapping (HEIGHT) when the measured text actually spans 2+ lines
  // (vs its line-height, NOT font-size — a generous line-height on one line
  // must not look multi-line). Single-line stays NONE so a downstream
  // font-width mismatch can't wrap-and-clip it.
  const lh = /px/.test(n.lineHeight || '') ? parseFloat(n.lineHeight) : n.fontSize * 1.3;
  node.textAutoResize = (tr.h > lh * 1.6) ? 'HEIGHT' : 'NONE';
  const fp = solidPaint(n.color);
  if (fp) node.fillPaints = [fp];
  return node;
}

function mapNode(n, parent) {
  const base = parent ? parent.rect : n.rect;
  const hasText = !!(n.text && n.textRect);
  const kids = n.kids || [];
  const solid = solidPaint(n.bg);
  const hasBorder = n.borderW > 0 && n.borderStyle !== 'none';
  const hasRadius = (n.radius || []).some(v => v > 0);
  const boxVisual = !!solid || !!n.raster || hasBorder || hasRadius || !!n.effect;

  // Pure text (no box, no children) -> a single TEXT node.
  if (parent && hasText && !boxVisual && kids.length === 0) {
    const t = makeTextNode(n, base);
    if (n.opacity < 0.999) t.opacity = n.opacity;
    return t;
  }

  // Otherwise a FRAME box; text (if any) and children go inside it.
  const node = {
    name: n.tag + '_' + (nameCounter++),
    type: 'FRAME',
    transform: { x: parent ? +(n.rect.x - base.x).toFixed(2) : 0, y: parent ? +(n.rect.y - base.y).toFixed(2) : 0 },
    size: { x: +n.rect.w.toFixed(2), y: +n.rect.h.toFixed(2) },
  };
  if (!n.raster) { const rot = rotationDeg(n.transform); if (Math.abs(rot) > 0.1) node.transform.rotation = +rot.toFixed(2); }
  const fills = [];
  if (solid) fills.push(solid);
  if (n.raster) fills.push({ type: 'IMAGE', image: { filename: `images/w2c_${statePrefix}${n.raster}.png` }, scaleMode: 'FILL' });
  if (fills.length) node.fillPaints = fills;
  // A rasterized element already has its border (and its rounding) baked into
  // the screenshot; a separate stroke would double-draw it as a square.
  if (!n.raster && hasBorder) { const sp = solidPaint(n.borderColor); if (sp) { node.strokePaints = [sp]; node.strokeWeight = n.borderW; node.strokeAlign = 'INSIDE'; } }
  if (!n.raster && hasRadius) {
    const r = n.radius;
    if (r.every(v => v === r[0])) node.cornerRadius = r[0];
    else { node.topLeftRadius = r[0]; node.topRightRadius = r[1]; node.bottomRightRadius = r[2]; node.bottomLeftRadius = r[3]; }
  }
  if (n.effect) {
    const c = parseColor(n.effect.color) || { hex: '#000000', alpha: 0.5 };
    node.effects = [{
      type: 'DROP_SHADOW',
      color: c.hex + Math.round(c.alpha * 255).toString(16).padStart(2, '0'),
      offset: { x: n.effect.ox, y: n.effect.oy }, radius: n.effect.blur, spread: n.effect.spread,
    }];
  }
  node.frameMaskDisabled = !(n.overflow && n.overflow !== 'visible');
  if (n.opacity < 0.999) node.opacity = n.opacity;

  const children = [];
  if (hasText) children.push(makeTextNode(n, n.rect));  // text relative to this frame
  for (const k of kids) children.push(mapNode(k, n));
  if (children.length) node.children = children;
  return node;
}

function rasterMarks(n, acc) {
  if (n.raster && n.rasterHideContent) acc.push({ id: n.raster });
  for (const k of (n.kids || [])) rasterMarks(k, acc);
  return acc;
}

// ---- main -----------------------------------------------------------------

(async () => {
  const a = parseArgs(process.argv);
  if (!a.input) { console.error('usage: web2canvas <url|file.html> [-o out] [--root SEL] [--viewport WxH] [--browser msedge|chrome] [--scale N]'); process.exit(2); }
  const out = a.out || a.input.replace(/\.[^.]+$/, '') + '.canvas.json';
  const outDir = path.dirname(path.resolve(out));
  const imagesDir = path.join(outDir, 'images');

  console.log(`launching ${a.browser} ...`);
  const browser = await chromium.launch({ channel: a.browser, headless: true });
  const page = await browser.newPage({ viewport: { width: a.vw, height: a.vh }, deviceScaleFactor: a.scale });
  await setupCdnRoutes(page);

  let server = null, pageUrl, fontsCssUrl = null;
  if (/^https?:\/\//.test(a.input)) pageUrl = a.input;
  else {
    const abs = path.resolve(a.input);
    let root = path.dirname(abs), inputRel = path.basename(abs);
    if (a.fonts) {  // serve a common root so the real fonts.css is reachable
      const fAbs = path.resolve(a.fonts);
      const da = path.dirname(abs).split(/[\\/]/), db = fAbs.split(/[\\/]/);
      let i = 0; while (i < da.length && i < db.length && da[i].toLowerCase() === db[i].toLowerCase()) i++;
      root = da.slice(0, i).join('/');
      inputRel = path.relative(root, abs).replace(/\\/g, '/');
      const fontsRel = path.relative(root, fAbs).replace(/\\/g, '/');
      if (fs.existsSync(path.join(fAbs, 'fonts.css'))) fontsCssUrl = fontsRel + '/fonts.css';
    }
    server = await startStaticServer(root);
    const enc = s => s.split('/').map(encodeURIComponent).join('/');
    pageUrl = `http://127.0.0.1:${server.address().port}/${enc(inputRel)}`;
    if (fontsCssUrl) fontsCssUrl = `http://127.0.0.1:${server.address().port}/${enc(fontsCssUrl)}`;
  }
  console.log(`loading ${pageUrl}`);
  await page.goto(pageUrl, { waitUntil: 'networkidle' }).catch(() => {});
  await page.waitForTimeout(a.wait);
  // Load the project's real fonts so text is measured at true widths (Google
  // Fonts are aborted; without this, fallback-font widths cause overlaps).
  if (fontsCssUrl) {
    try {
      await page.addStyleTag({ url: fontsCssUrl });
      await page.evaluate(() => document.fonts.ready);
      await page.waitForTimeout(500);
    } catch (e) { console.error('fonts:', e.message); }
  }
  // States: each drives window[navFn](state) and becomes one top-level frame.
  // No --states → a single capture of the current screen (backward compatible).
  const states = a.states ? a.states.split(',').map(s => s.trim()).filter(Boolean) : [null];
  const multi = states.length > 1 || (states[0] && a.states);
  const frames = [];
  let totShot = 0, totMarks = 0, offsetX = 0;

  for (let si = 0; si < states.length; si++) {
    const st = states[si];
    if (st) {
      await page.evaluate(({ s, fn }) => { if (typeof window[fn] === 'function') window[fn](s); }, { s: st, fn: a.navFn });
      await page.waitForTimeout(a.wait);
    }
    if (si === 0) await page.screenshot({ path: out.replace(/\.canvas\.json$|\.json$/, '') + '.web.png' }).catch(() => {});

    const res = await page.evaluate(collectorFn, a.root);
    if (!res.tree) { console.error('WARN: nothing collected for state ' + st); continue; }
    const tree = res.tree;
    statePrefix = multi ? (si + '_') : '';

    const marks = rasterMarks(tree, []);
    (function whole(n) { if (n.raster && !n.rasterHideContent) marks.push({ id: n.raster, whole: true }); for (const k of (n.kids || [])) whole(k); })(tree);
    if (marks.length) fs.mkdirSync(imagesDir, { recursive: true });
    for (const m of marks) {
      const loc = page.locator(`[data-w2c="${m.id}"]`);
      try {
        if (!m.whole) await page.evaluate(setBgOnlyFn, { id: m.id, on: true });
        await loc.screenshot({ path: path.join(imagesDir, `w2c_${statePrefix}${m.id}.png`), omitBackground: true });
        totShot++;
      } catch (e) { /* not screenshot-able */ }
      finally { if (!m.whole) await page.evaluate(setBgOnlyFn, { id: m.id, on: false }); }
    }
    totMarks += marks.length;

    nameCounter = 0;
    const frame = mapNode(tree, null);
    frame.name = st || 'Page';
    frame.scrollDirection = 'VERTICAL';
    frame.transform = { x: offsetX, y: 0 };
    offsetX += (frame.size.x || res.rootW) + 60;
    frames.push(frame);
    console.log(`  captured ${frame.name} (${marks.length} rasters)`);
  }
  await browser.close();
  if (server) server.close();
  if (!frames.length) { console.error('FAIL: no frames captured'); process.exit(1); }

  const doc = {
    document: { type: 'DOCUMENT', children: [{ type: 'CANVAS', name: 'Page 1', children: frames }] },
    styles: {},
  };
  fs.writeFileSync(out, JSON.stringify(doc, null, 2));
  console.log(`RESULT: OK  ${frames.length} frame(s)  ${totShot}/${totMarks} rasters -> ${out}`);
})().catch(e => { console.error('FAIL:', e.message); process.exit(1); });
