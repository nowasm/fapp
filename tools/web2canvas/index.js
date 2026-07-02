#!/usr/bin/env node
// web2canvas — render a React/HTML page in a headless browser and convert the
// computed DOM into figo canvas.json (the same format fig2json emits and
// figo / figo2godot consume).
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
const { spawnSync, spawn } = require('child_process');
const { chromium } = require('playwright-core');

// ---- CLI ------------------------------------------------------------------

function parseArgs(argv) {
  const a = { input: null, out: null, root: 'body', vw: 1280, vh: 720,
              browser: 'msedge', wait: 400, scale: 2, fonts: null, states: null,
              flows: null, navFn: '__nav', navReset: '__w2c_reset__', aiName: false,
              pick: false, pickKey: 'f', pickAt: null, pickFreezeAt: null, open: null };
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
    else if (t === '--flows') a.flows = argv[++i];
    else if (t === '--nav-fn') a.navFn = argv[++i];
    else if (t === '--nav-reset') a.navReset = argv[++i];
    else if (t === '--ai-name') a.aiName = true;
    else if (t === '--pick') a.pick = true;
    else if (t === '--pick-key') a.pickKey = (argv[++i] || 'f').slice(0, 1).toLowerCase();
    else if (t === '--pick-at') { const m = /(-?\d+)\s*,\s*(-?\d+)/.exec(argv[++i] || ''); if (m) { a.pick = true; a.pickAt = { x: +m[1], y: +m[2] }; } }
    else if (t === '--pick-freeze') { const m = /(-?\d+)\s*,\s*(-?\d+)/.exec(argv[++i] || ''); if (m) a.pickFreezeAt = { x: +m[1], y: +m[2] }; }
    else if (t === '--open') a.open = true;
    else if (t === '--no-open') a.open = false;
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
// abort web fonts so networkidle settles (figo supplies its own fonts).
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

function collectorFn({ rootSelector, aiName }) {
  const root = document.querySelector(rootSelector) || document.body;
  const rootRect = root.getBoundingClientRect();
  const rootArea = rootRect.width * rootRect.height;
  let rid = 0, candId = 0;

  // ── CSS animation capture (opacity + 2D scale only) ──────────────────────────
  // Index every @keyframes rule by name so an animated element's resolved frames
  // can be read. Static-snapshot caveat: only looping/transform-scale/opacity
  // animations replay in the engine; height/clip-path/color morphs do not.
  const kfMap = {};
  (function indexRules(rules) {
    if (!rules) return;
    for (const r of rules) {
      if (r.type === 7 /* CSSKeyframesRule */) kfMap[r.name] = r;
      else if (r.cssRules) indexRules(r.cssRules);  // recurse @media / @supports
    }
  })((() => { const out = []; for (const ss of document.styleSheets) {
      try { if (ss.cssRules) for (const r of ss.cssRules) out.push(r); } catch (e) {} }
      return out; })());
  function parseScale(tf) {
    if (!tf || tf === 'none') return null;
    let m;
    if ((m = /scale\(\s*([-\d.]+)\s*(?:,\s*([-\d.]+)\s*)?\)/.exec(tf)))
      return [parseFloat(m[1]), m[2] !== undefined ? parseFloat(m[2]) : parseFloat(m[1])];
    if ((m = /scaleX\(\s*([-\d.]+)\s*\)/.exec(tf))) return [parseFloat(m[1]), 1];
    if ((m = /scaleY\(\s*([-\d.]+)\s*\)/.exec(tf))) return [1, parseFloat(m[1])];
    return null;
  }
  // Compose a CSS transform string into a 2×3 affine matrix [a,b,c,d,e,f] (CSS
  // column-vector convention). Used to extract the net 2D TRANSLATION (e,f) of a
  // keyframe — so a `rotate(θ) translateX(t)` slash sweep resolves to the screen
  // displacement (t·cosθ, t·sinθ) without needing a separate rotation track.
  function transformMat(tf) {
    let m = [1, 0, 0, 1, 0, 0];
    if (!tf || tf === 'none') return m;
    const mul = (n) => { m = [
      m[0]*n[0]+m[2]*n[1], m[1]*n[0]+m[3]*n[1],
      m[0]*n[2]+m[2]*n[3], m[1]*n[2]+m[3]*n[3],
      m[0]*n[4]+m[2]*n[5]+m[4], m[1]*n[4]+m[3]*n[5]+m[5] ]; };
    const re = /(\w+)\(([^)]*)\)/g; let mm;
    while ((mm = re.exec(tf))) {
      const fn = mm[1], a = mm[2].split(',').map(s => parseFloat(s) || 0);
      if (fn === 'matrix' && a.length === 6) mul(a);
      else if (fn === 'translate') mul([1,0,0,1, a[0], a[1] || 0]);
      else if (fn === 'translateX') mul([1,0,0,1, a[0], 0]);
      else if (fn === 'translateY') mul([1,0,0,1, 0, a[0]]);
      else if (fn === 'rotate') { const r = a[0]*Math.PI/180, c = Math.cos(r), s = Math.sin(r); mul([c,s,-s,c,0,0]); }
      else if (fn === 'scale') mul([a[0]||1,0,0, a.length>1?a[1]:(a[0]||1), 0,0]);
      else if (fn === 'scaleX') mul([a[0]||1,0,0,1,0,0]);
      else if (fn === 'scaleY') mul([1,0,0,a[0]||1,0,0]);
    }
    return m;
  }
  function animOf(cs, r) {
    const name = (cs.animationName || 'none').split(',')[0].trim();
    if (name === 'none' || !kfMap[name]) return null;
    const dur = parseFloat(cs.animationDuration) || 0;       // "1.1s" → 1.1
    if (dur <= 0) return null;
    const iterRaw = (cs.animationIterationCount || '1').split(',')[0].trim();
    const iter = iterRaw === 'infinite' ? 0 : (parseInt(iterRaw, 10) || 1);
    // transform-origin is computed to px relative to the element box.
    let pivot = [0.5, 0.5];
    const to = (cs.transformOrigin || '').split(' ').map(parseFloat);
    if (to.length >= 2 && r.width > 0 && r.height > 0 && isFinite(to[0]) && isFinite(to[1]))
      pivot = [to[0] / r.width, to[1] / r.height];
    // Translate (position) keyframes are replayed only for FINITE animations:
    // they compose with the node's CAPTURED base box, which for a finished
    // `both`-fill animation equals the last keyframe — so positions are emitted
    // rest-relative to that (subtracted below). An infinite loop has no settled
    // rest box to anchor against, so its translate is skipped (only opacity/
    // scale loop via the phase-baking path).
    const finite = iter !== 0;
    const keys = [];
    let sawOpacity = false, sawScale = false, sawHeight = false, sawPos = false;
    for (const rule of kfMap[name].cssRules) {
      if (!rule.keyText) continue;
      for (const kt of rule.keyText.split(',')) {
        const s = kt.trim();
        const t = s === 'from' ? 0 : s === 'to' ? 1 : parseFloat(s) / 100;
        if (!isFinite(t)) continue;
        const k = { t };
        if (rule.style.opacity !== '') { k.opacity = parseFloat(rule.style.opacity); sawOpacity = true; }
        const sc = parseScale(rule.style.transform);
        if (sc) { k.scale = sc; sawScale = true; }
        if (finite && rule.style.transform && rule.style.transform !== 'none') {
          // Record pos on EVERY transform keyframe, including zero translation —
          // a slide-in's resting `translateY(0)` key must exist or the track has
          // a single key and gets dropped downstream (and the rest-anchor below
          // would pick the wrong key). sawPos only when some key actually moves;
          // pure scale/rotate anims get their no-op pos stripped after the loop.
          const mat = transformMat(rule.style.transform);
          k.pos = [mat[4], mat[5]];
          if (Math.abs(mat[4]) > 0.01 || Math.abs(mat[5]) > 0.01) sawPos = true;
        }
        // height keyframes (equalizer/voiceprint bars) → scaleY about the
        // element's CAPTURED box height, so the absolute px land correctly
        // regardless of which animation phase the snapshot caught.
        const h = rule.style.height;
        if (h && h.endsWith('px') && r.height > 0) {
          const sy = parseFloat(h) / r.height;
          if (k.scale) k.scale[1] = sy; else k.scale = [1, sy];
          sawScale = true; sawHeight = true;
        }
        if ('opacity' in k || 'scale' in k || 'pos' in k) keys.push(k);
      }
    }
    if (!sawPos) {  // no key actually translates — drop the no-op pos entries
      for (const k of keys) delete k.pos;
      for (let i = keys.length - 1; i >= 0; i--)
        if (!('opacity' in keys[i]) && !('scale' in keys[i])) keys.splice(i, 1);
    }
    if (!keys.length) return null;
    // A height-grow (bar) animation rises from its baseline, so pivot at the
    // bottom edge; a transform-origin (if any) still wins for transform scales.
    if (sawHeight && (cs.transformOrigin || '').indexOf('px') < 0) pivot = [0.5, 1];
    else if (sawHeight) pivot = [pivot[0], 1];
    // CSS fills missing 0%/100% from the element's resting value (opacity 1,
    // scale 1). Synthesize endpoints so the engine track spans the full length.
    const ensure = (tt) => {
      if (keys.some(k => Math.abs(k.t - tt) < 1e-4)) return;
      const e = { t: tt };
      if (sawOpacity) e.opacity = 1;
      if (sawScale) e.scale = [1, 1];
      if (sawPos) e.pos = [0, 0];
      keys.push(e);
    };
    ensure(0); ensure(1);
    keys.sort((a, b) => a.t - b.t);
    // Position is rest-relative: the captured box already sits at the resting
    // (last-keyframe) translate, so subtract it — the engine then animates the
    // delta around the node's exported offset, landing back at rest at t=1.
    if (sawPos) {
      const restKeys = keys.filter(k => k.pos);
      const rest = restKeys[restKeys.length - 1].pos;
      for (const k of keys) if (k.pos) k.pos = [k.pos[0] - rest[0], k.pos[1] - rest[1]];
    }

    const delay = parseFloat(cs.animationDelay) || 0;
    // animation-delay on an infinite loop is a phase offset (the staggered
    // voiceprint bars, the second alarm ring). Engine players all autoplay at
    // t=0, so bake the phase in by shifting the keyframes by -delay. Crucially,
    // sampling stays WITHIN the [0,1] segments — never across the period-boundary
    // snap (a ring's 1.35→0.7 jump each cycle) — or the snap would smear into a
    // ramp and reverse the visible direction (outward expand → inward). The snap
    // is re-emitted as a pair of keys at the seam.
    let outKeys = keys;
    const sFrac = ((((delay % dur) / dur) % 1) + 1) % 1;
    if (iter === 0 && sFrac > 1e-4) {
      const lerpV = (a, b, u) => Array.isArray(a) ? a.map((av, i) => av + (b[i] - av) * u) : a + (b - a) * u;
      // value of `field` at phase p∈[0,1]; phase 0/1 return the first/last key
      // (the snap endpoints), interpolation only inside real segments.
      const sample = (field, phase) => {
        const ks = keys.filter(k => field in k);
        if (!ks.length) return undefined;
        if (ks.length === 1) return ks[0][field];
        const p = Math.min(1, Math.max(0, phase));
        for (let i = 0; i < ks.length - 1; i++) {
          const a = ks[i], b = ks[i + 1];
          if (p >= a.t && p <= b.t) return lerpV(a[field], b[field], b.t > a.t ? (p - a.t) / (b.t - a.t) : 0);
        }
        return ks[ks.length - 1][field];
      };
      // GAP separates the two seam keys (pre-snap, post-snap). It must survive
      // figo2godot's 3-decimal (0.001s) time formatting after ×duration, or the
      // keys collapse onto one timestamp and the snap plays backwards — size it
      // in real time: ≥6ms regardless of period length. DET is the seam detector.
      const GAP = Math.max(0.008, 0.006 / dur), DET = 1e-4;
      const times = new Set([0, 1, sFrac]);
      for (const k of keys) times.add(((((k.t + sFrac) % 1) + 1) % 1));
      outKeys = [];
      for (const t of [...times].filter(x => x >= 0 && x <= 1).sort((x, y) => x - y)) {
        if (Math.abs(t - sFrac) < DET && t > GAP && t < 1 - GAP) {
          // the period-boundary snap lands here: end-of-curve then start-of-curve.
          const pre = { t: +(t - GAP).toFixed(5) }, post = { t: +t.toFixed(5) };
          if (sawOpacity) { pre.opacity = +sample('opacity', 1).toFixed(4); post.opacity = +sample('opacity', 0).toFixed(4); }
          if (sawScale)   { pre.scale = sample('scale', 1).map(x => +x.toFixed(4)); post.scale = sample('scale', 0).map(x => +x.toFixed(4)); }
          outKeys.push(pre, post);
        } else {
          const src = ((((t - sFrac) % 1) + 1) % 1);
          const o = { t: +t.toFixed(5) };
          if (sawOpacity) { const v = sample('opacity', src); if (v !== undefined) o.opacity = +v.toFixed(4); }
          if (sawScale)   { const v = sample('scale', src);   if (v !== undefined) o.scale = v.map(x => +x.toFixed(4)); }
          outKeys.push(o);
        }
      }
    }
    // First timing function, paren-aware: a naive split(',') would truncate
    // "cubic-bezier(.7,0,.3,1)" to "cubic-bezier(.7" and lose the curve.
    const easeRaw = cs.animationTimingFunction || 'linear';
    const easeM = /^\s*([a-z-]+\([^)]*\)|[a-z-]+)/i.exec(easeRaw);
    return { dur, delay, iter, pivot,
             ease: easeM ? easeM[1] : 'linear', keys: outKeys };
  }
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

  // Semantic name: nearest React component (via fiber), else first CSS class,
  // else the tag. Gives meaningful node/sprite names (PlayerCard, BigButton)
  // instead of div_0 — the "naming spine" for the prefab workflow.
  function reactName(el) {
    const k = Object.keys(el).find(k => k.startsWith('__reactFiber$') || k.startsWith('__reactInternalInstance$'));
    if (!k) return null;
    let f = el[k];
    while (f) {
      const t = f.type;
      if (typeof t === 'function') { const n = t.displayName || t.name; if (n && n.length > 1 && n !== '_default') return n; }
      f = f.return;
    }
    return null;
  }
  function semanticName(el) {
    const cn = (typeof el.className === 'string' && el.className.trim()) ? el.className.trim().split(/\s+/)[0] : null;
    return reactName(el) || cn || null;
  }

  // Context hints for naming a TEXT node by ROLE (not its literal text):
  //  - clickable: the text lives inside a <button>/role=button -> button label.
  //  - nearAvatar: a circular/image sibling within a couple of levels -> the
  //    text is a person name (username) rather than a static label.
  // An avatar = a real photo/portrait, NOT an inline icon/emblem (those are svg,
  // small, and sit next to plain labels — counting them floods false usernames).
  function isAvatarish(e) {
    if (!e || e.nodeType !== 1) return false;
    const tag = e.tagName.toLowerCase();
    if (tag === 'image-slot') return true;  // an explicit avatar slot, any size
    const r = e.getBoundingClientRect();
    const portraitSize = r.width >= 24 && r.width <= 72 && Math.abs(r.width - r.height) <= 12;
    if ((tag === 'img' || tag === 'canvas') && portraitSize) return true;  // a small square photo
    if (!portraitSize) return false;
    const cs = getComputedStyle(e);
    const brs = cs.borderTopLeftRadius || '';
    if (brs.includes('%') ? parseFloat(brs) >= 40 : parseFloat(brs) >= r.width * 0.4) return true;  // circular portrait
    if (cs.backgroundImage && cs.backgroundImage.includes('url(')) return true;  // real photo bg (not a gradient)
    if (r.width >= 40 && e.querySelector('img,canvas,image-slot')) return true;  // large slot wrapper (not an svg emblem)
    return false;
  }
  function nearAvatar(el) {
    let cur = el;
    for (let up = 0; up < 3 && cur; up++) {
      const par = cur.parentElement;
      if (par) for (const s of par.children) {
        if (s === cur) continue;
        if (isAvatarish(s)) return true;
        for (const c of s.children) if (isAvatarish(c)) return true;
      }
      cur = par;
    }
    return false;
  }

  // Merge sibling decoration leaves that visually form ONE unit into a single
  // baked sprite. A "deco leaf" has no text and no children, would be baked
  // anyway (raster or a glow/shadow effect), and is SMALL — e.g. the two arms of
  // an L-shaped corner accent (separate <span>s overlapping at the corner).
  // The size cap is what keeps functional multi-part widgets apart: a slider's
  // long track/fill exceed it, so its small knob is left alone (group of 1) and
  // the slider stays addressable instead of collapsing into one static image.
  const DECO_MAX = 40;
  function isDecoLeaf(n) {
    return !n.text && (!n.kids || n.kids.length === 0) && (n.raster || n.effect)
      && Math.max(n.rect.w, n.rect.h) <= DECO_MAX;
  }
  function rectsTouch(a, b) {  // overlapping or within 2px
    return !(a.x > b.x + b.w + 2 || b.x > a.x + a.w + 2 ||
             a.y > b.y + b.h + 2 || b.y > a.y + a.h + 2);
  }
  function mergeDecoGroups(pairs, ox, oy) {
    const result = [];
    let i = 0;
    while (i < pairs.length) {
      if (!isDecoLeaf(pairs[i].n)) { result.push(pairs[i].n); i++; continue; }
      let j = i + 1, bb = { ...pairs[i].n.rect };
      while (j < pairs.length && isDecoLeaf(pairs[j].n) && rectsTouch(bb, pairs[j].n.rect)) {
        const r = pairs[j].n.rect;
        const x1 = Math.min(bb.x, r.x), y1 = Math.min(bb.y, r.y);
        const x2 = Math.max(bb.x + bb.w, r.x + r.w), y2 = Math.max(bb.y + bb.h, r.y + r.h);
        bb = { x: x1, y: y1, w: x2 - x1, h: y2 - y1 };
        j++;
      }
      if (j - i >= 2) {
        const members = pairs.slice(i, j), gid = ++rid;
        members.forEach(m => m.el.setAttribute('data-w2c-grp', String(gid)));
        // Expand the clip by each member's glow/shadow reach so an outer glow
        // isn't sliced off at the union bbox (the box-shadow paints beyond it).
        let m = 0;
        for (const mem of members) {
          const e = mem.n.effect;
          if (e) m = Math.max(m, Math.ceil(Math.max(Math.abs(e.ox || 0), Math.abs(e.oy || 0)) + (e.blur || 0) + (e.spread || 0)));
        }
        // Clamp the absolute clip to the viewport; keep node rect in lockstep.
        const absX = bb.x + ox - m, absY = bb.y + oy - m;
        const x0 = Math.max(0, absX), y0 = Math.max(0, absY);
        const x1 = Math.min(window.innerWidth, bb.x + ox + bb.w + m);
        const y1 = Math.min(window.innerHeight, bb.y + oy + bb.h + m);
        const cw = x1 - x0, ch = y1 - y0;
        result.push({
          tag: 'div', cname: members[0].n.cname || 'Accent',
          rect: { x: x0 - ox, y: y0 - oy, w: cw, h: ch },
          text: null, textRect: null, bg: 'rgba(0, 0, 0, 0.000)',
          radius: [0, 0, 0, 0], borderW: 0, borderColor: null, borderStyle: 'none',
          effect: null, opacity: 1, transform: 'none', overflow: 'visible',
          color: null, fontFamily: '', fontSize: 0, fontWeight: '400', textAlign: 'left', lineHeight: 'normal',
          raster: gid, rasterWhole: true, rasterHideContent: false,
          rasterGroup: true, groupClip: { x: x0, y: y0, width: cw, height: ch },
          z: members[members.length - 1].n.z, clickable: false, nearAvatar: false, kids: [],
        });
      } else result.push(pairs[i].n);
      i = j;
    }
    return result;
  }

  // A child that fills the whole root with a (near-)opaque solid backdrop hides
  // everything painted under it. The alpha of a normalized rgba string:
  function bgAlpha(n) {
    const mm = /,\s*([0-9.]+)\s*\)\s*$/.exec(n.bg || '');
    return mm ? parseFloat(mm[1]) : 0;
  }
  function occludesRoot(n) {
    return n.rect && n.rect.x <= 1 && n.rect.y <= 1 &&
      n.rect.w >= rootRect.width - 2 && n.rect.h >= rootRect.height - 2 &&
      bgAlpha(n) >= 0.85;
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

  // A standalone symbol/pictograph glyph (⤢ maximize, ✕ close, ❯ chevron, an
  // emoji…) has no reliable coverage in the bundled text fonts — JetBrains Mono,
  // Noto Sans and Oswald all lack e.g. U+2922 (⤢). A Label would then fall back
  // to an arbitrary OS glyph whose metrics differ from the browser's fallback,
  // and the two arms of the arrow drift out of alignment. So rasterize such a
  // glyph span and let Godot blit the exact browser pixels instead. Only fires
  // when the element is a text LEAF whose ENTIRE text is symbols — mixed text
  // like "🔒 加入" stays as a recolorable TEXT node. Basic arrows (U+2190–21FF)
  // and geometric shapes (U+25xx) ARE covered by Noto, so they're left as text.
  function isSymbolGlyphRun(s) {
    if (!s) return false;
    let sawSym = false;
    for (const ch of s) {
      const c = ch.codePointAt(0);
      if (c === 0x20 || c === 0xFE0F || c === 0x200D) continue;  // space, VS16, ZWJ
      const sym = (c >= 0x2300 && c <= 0x27BF) ||   // misc technical / symbols / dingbats
                  (c >= 0x2900 && c <= 0x29FF) ||   // supplemental arrows-B / math-B
                  (c >= 0x2B00 && c <= 0x2BFF) ||   // misc symbols and arrows
                  (c >= 0x1F000 && c <= 0x1FAFF);   // emoji / pictographs
      if (!sym) return false;   // any normal letter/digit/CJK/punct -> keep as text
      sawSym = true;
    }
    return sawSym;
  }

  // An element's own (non-whitespace) text, via a Range per direct text node.
  // Each text node is a RUN with its own rect: an element with inline siblings
  // (由 <span>name</span> rest) has its text split AROUND the span, so the runs
  // must stay separate — merging them into one box would span the whole line and
  // overlap the inline span. Returns { runs:[{text,rect}], text, rect } or null
  // (text/rect = joined/union, for naming + the single-run fast path).
  function measureDirectText(el, ox, oy) {
    const runs = [];
    for (const ch of el.childNodes) {
      if (ch.nodeType === 3 && ch.textContent && ch.textContent.trim()) {
        const r = document.createRange();
        r.selectNodeContents(ch);
        const rc = r.getBoundingClientRect();
        if (rc.width >= 1 && rc.height >= 1)
          // keep a leading/trailing space (NOT trimmed) — the run's rect includes
          // it, so the space renders as the gap to an adjacent inline sibling.
          runs.push({ text: ch.textContent.replace(/\s+/g, ' '),
                      rect: { x: rc.left - ox, y: rc.top - oy, w: rc.width, h: rc.height } });
      }
    }
    if (!runs.length) return null;
    const x0 = Math.min(...runs.map(r => r.rect.x)), y0 = Math.min(...runs.map(r => r.rect.y));
    const x1 = Math.max(...runs.map(r => r.rect.x + r.rect.w)), y1 = Math.max(...runs.map(r => r.rect.y + r.rect.h));
    return { runs, text: runs.map(r => r.text).join('').replace(/\s+/g, ' ').trim(), rect: { x: x0, y: y0, w: x1 - x0, h: y1 - y0 } };
  }

  // An <input>/<textarea> paints its value/placeholder itself (not as child text
  // nodes), so measureDirectText misses it and it gets baked into the box sprite.
  // Synthesize a text run from the value (else placeholder) at the content box,
  // left-aligned and vertically centred, with the placeholder's lighter color.
  function inputText(el, cs, r, ox, oy) {
    const text = ((el.value || '').trim()) || ((el.getAttribute('placeholder') || '').trim());
    if (!text) return null;
    const isPlaceholder = !(el.value || '').trim();
    const fs = parseFloat(cs.fontSize) || 13;
    ncx.font = `${cs.fontStyle || 'normal'} ${cs.fontWeight || 400} ${fs}px ${cs.fontFamily || 'sans-serif'}`;
    const padL = parseFloat(cs.paddingLeft) || 0, bL = parseFloat(cs.borderLeftWidth) || 0;
    const lh = /px/.test(cs.lineHeight) ? parseFloat(cs.lineHeight) : fs * 1.3;
    const w = Math.min(ncx.measureText(text).width, r.width - padL - bL - 2);
    const rect = { x: r.left + bL + padL - ox, y: r.top + (r.height - lh) / 2 - oy, w, h: lh };
    let color = cs.color;
    if (isPlaceholder) { try { const pc = getComputedStyle(el, '::placeholder').color; if (pc) color = pc; } catch (e) {} }
    return { runs: [{ text, rect }], text, rect, color };
  }

  function node(el, ox, oy, depth) {
    const cs = getComputedStyle(el);
    if (!visible(el, cs)) return null;
    const r = el.getBoundingClientRect();
    const tag = el.tagName.toLowerCase();
    // Drop a full-frame photo/video background — that's the game-world scene/map
    // art (e.g. <img src="scene-tomb.png">), not UI. A CSS gradient/solid
    // background is a <div> (kept); small media like the minimap is far under the
    // threshold (kept). svg/canvas (icons/charts) are never dropped here.
    if ((tag === 'img' || tag === 'video') && r.width * r.height >= rootArea * 0.85) return null;
    let ti = measureDirectText(el, ox, oy);
    let inputColor = null;
    if (!ti && (tag === 'input' || tag === 'textarea')) {
      const it = inputText(el, cs, r, ox, oy);
      if (it) { ti = { runs: it.runs, text: it.text, rect: it.rect }; inputColor = it.color; }
    }

    // A leaf span that is purely an unrenderable symbol glyph: bake it to a
    // sprite (below) and drop its text so no mis-fonted Label is emitted.
    const symbolGlyph = !!ti && el.children.length === 0 && isSymbolGlyphRun(ti.text);
    if (symbolGlyph) { ti = null; inputColor = null; }

    const wholeRaster = (tag === 'img' || tag === 'svg' || tag === 'canvas' || tag === 'video') || isTransformed(cs) || symbolGlyph;
    const clipped = cs.clipPath && cs.clipPath !== 'none';
    const bgRaster = hasRealBg(cs) || clipped || fancyBorder(cs);
    let raster = null, rasterWhole = false, rasterHideContent = false;
    if (wholeRaster) { raster = ++rid; rasterWhole = true; }
    else if (bgRaster) { raster = ++rid; rasterHideContent = true; }
    if (raster) el.setAttribute('data-w2c', String(raster));

    // Component identity straight from React's tree (the source components), not
    // a guess from DOM structure: an element is a COMPONENT ROOT when the nearest
    // function component rendering it differs from its parent's — i.e. it is the
    // top DOM node a component (PlayerCard, GSlider, RoomThumb…) outputs. Roots
    // carry the component name; inner elements use their own class (else tag_N),
    // so the tree reads as <Component> wrapping anonymous structure, not the
    // ancestor component name repeated down every child.
    const comp = reactName(el);
    const compRoot = !!(comp && comp !== (el.parentElement ? reactName(el.parentElement) : null));
    const ownClass = (typeof el.className === 'string' && el.className.trim()) ? el.className.trim().split(/\s+/)[0] : null;
    const out = {
      tag,
      cname: compRoot ? comp : (ownClass || null),
      comp, compRoot,            // source-component type + instance-root flag (prefab grouping spine)
      rect: { x: r.left - ox, y: r.top - oy, w: r.width, h: r.height },
      text: ti ? ti.text : null,
      textRect: ti ? ti.rect : null,
      textRuns: ti ? ti.runs : null,
      bg: norm(cs.backgroundColor),
      // Resolve percentage corner radii to px (Edge returns "50%" unresolved):
      // a 50% radius is a circle (half the box), not 50px — otherwise a baked
      // ring/avatar comes out as a squircle.
      radius: [cs.borderTopLeftRadius, cs.borderTopRightRadius,
               cs.borderBottomRightRadius, cs.borderBottomLeftRadius]
              .map(v => { const s = String(v).trim();
                          return s.endsWith('%') ? (parseFloat(s) || 0) / 100 * r.width : (parseFloat(s) || 0); }),
      borderW: parseFloat(cs.borderTopWidth) || 0,
      borderColor: norm(cs.borderTopColor),
      borderStyle: cs.borderTopStyle,
      effect: shadowFromBox(cs.boxShadow) || shadowFromFilter(cs.filter),
      opacity: parseFloat(cs.opacity),
      transform: cs.transform,
      overflow: cs.overflow,
      overflowX: cs.overflowX,
      overflowY: cs.overflowY,
      color: norm(inputColor || cs.color),
      fontFamily: cs.fontFamily,
      fontSize: parseFloat(cs.fontSize) || 0,
      fontWeight: cs.fontWeight,
      textAlign: cs.textAlign,
      lineHeight: cs.lineHeight,
      raster, rasterWhole, rasterHideContent,
      // Effective stacking z: a positioned element honors its z-index; everything
      // else stacks at 0. Used to reorder siblings into CSS paint order below.
      z: (cs.position !== 'static' && cs.zIndex !== 'auto') ? (parseInt(cs.zIndex, 10) || 0) : 0,
      // naming hints (only meaningful for nodes carrying direct text)
      clickable: ti ? !!el.closest('button,[role="button"]') : false,
      nearAvatar: ti ? nearAvatar(el) : false,
      kids: [],
    };
    const an = animOf(cs, r);
    if (an) out.anim = an;
    if (!rasterWhole) {
      const pairs = [];
      for (const child of el.children) {
        const n = node(child, ox, oy, depth + 1);
        if (n) pairs.push({ n, el: child });
      }
      // Emit children in CSS paint order: a higher z-index paints on top (later),
      // even when it comes earlier in the DOM (e.g. a zIndex-raised corner accent
      // before the panel it overlays). Array.sort is stable, so equal-z siblings
      // keep their DOM order. Then merge overlapping decoration leaves into one
      // sprite (the L-accent's two arms).
      pairs.sort((a, b) => a.n.z - b.n.z);
      out.kids = mergeDecoGroups(pairs, ox, oy);
      // Drop siblings hidden behind a full-root opaque overlay (e.g. the game
      // HUD under a full-screen death overlay) — keep only the overlay onward.
      for (let k = out.kids.length - 1; k >= 1; k--) {
        if (occludesRoot(out.kids[k])) { out.kids = out.kids.slice(k); break; }
      }
    }
    // Substance: total descendant count (used to gate AI-naming candidates).
    out.desc = (out.kids || []).reduce((s, k) => s + 1 + (k.desc || 0), 0);

    // Tag AI-naming candidates: a composed CONTAINER worth recognizing — either a
    // top-level screen region (a direct child of the root) or a substantive
    // component (>=3 descendants). Bounded in size (skip the full-screen frame /
    // near-full overlays and sub-text fragments) so the montage holds real UI
    // pieces. Each tagged element gets a data-w2c-cand attr so the main loop can
    // screenshot it live (in its own screen) for the vision pass.
    if (aiName && candId < 400 && !rasterWhole && out.kids.length >= 1) {
      const area = r.width * r.height;
      const sizeOK = r.width >= 24 && r.height >= 16 && area <= rootArea * 0.92 &&
                     r.width <= rootRect.width + 1 && r.height <= rootRect.height + 1;
      const topRegion = depth === 1;          // a major section of the screen
      const component = out.desc >= 3;          // a card/widget/button group
      if (sizeOK && (topRegion || component)) {
        out.cand = ++candId;
        el.setAttribute('data-w2c-cand', String(candId));
      }
    }
    // A rasterized element with an outer glow/shadow paints BEYOND its box, but
    // a screenshot clipped to the box (and a clip_contents parent) slices the glow
    // off. Grow the node's box to the glow's reach and capture that expanded clip;
    // children stay put (their frame-relative rects auto-compensate against the
    // grown parent), and clipping to the grown box no longer cuts the glow.
    if (raster && out.effect) {
      const e = out.effect;
      const gm = Math.ceil(Math.max(Math.abs(e.ox || 0), Math.abs(e.oy || 0)) + (e.blur || 0) + (e.spread || 0));
      if (gm > 0) {
        const x0 = Math.max(0, out.rect.x + ox - gm), y0 = Math.max(0, out.rect.y + oy - gm);
        const x1 = Math.min(window.innerWidth, out.rect.x + ox + out.rect.w + gm);
        const y1 = Math.min(window.innerHeight, out.rect.y + oy + out.rect.h + gm);
        out.glowClip = { x: x0, y: y0, width: x1 - x0, height: y1 - y0 };
        out.rect = { x: x0 - ox, y: y0 - oy, w: x1 - x0, h: y1 - y0 };
        // KEEP out.effect: it's baked into the expanded sprite (figo2godot never
        // re-draws shadows), but the flag must stay so the node is NOT 9-sliced —
        // a glow panel'd middle would stretch into the glow margin and balloon
        // the panel (button_47 grew once the effect was cleared and it sliced).
      }
    }
    return out;
  }

  // A scrolled container (e.g. the chat log auto-scrolls to its latest message)
  // reports its children at scroll-shifted — often negative — positions. Reset
  // every scroll offset to 0 so children are measured at their natural top-left
  // layout; the exported ScrollContainer then starts at the top with correct
  // child offsets and the right content extent.
  root.scrollTop = 0; root.scrollLeft = 0;
  root.querySelectorAll('*').forEach(e => { e.scrollTop = 0; e.scrollLeft = 0; });
  const tree = node(root, rootRect.left, rootRect.top, 0);
  return { tree, rootW: rootRect.width, rootH: rootRect.height };
}

// Isolate a tagged element for its screenshot. element.screenshot captures the
// page clipped to the element's box, so overlapping FOREGROUND content (siblings
// drawn on top, e.g. text over a background image) would be baked into the
// sprite and re-emitted as its own nodes (duplicate). Hide everything, then
// reveal only the target:
//  - hideKids (background raster): keep the target's children + own text hidden
//    so only its own background/border is captured (children emit separately).
//  - else (whole-element raster, <img>/<svg>): reveal the whole subtree.
// html/body carry an opaque page background (e.g. #07080b). visibility:hidden
// hides every descendant's paint but NOT html/body's own background, so omit-
// Background can't make it transparent — a semi-transparent fill (a 0.18 toggle
// track) would bake over black into an opaque dark blob. Both isolation helpers
// below neutralize the page background while shooting so translucent sprites
// keep real alpha.
function setBgOnlyFn({ id, on, hideKids }) {
  const all = document.querySelectorAll('body *');
  if (on) {
    document.documentElement.style.background = 'transparent';
    document.body.style.background = 'transparent';
    for (const e of all) e.style.visibility = 'hidden';
    const el = document.querySelector(`[data-w2c="${id}"]`);
    if (!el) return;
    el.style.visibility = 'visible';
    if (hideKids) {
      el.setAttribute('data-w2c-c', el.style.color || '~'); el.style.color = 'transparent';
      // <input>/<textarea> paint their value/placeholder themselves; color:
      // transparent hides the value but not the ::placeholder, so clear it too —
      // else "输入消息…" bakes into the box sprite (it's emitted as a TEXT node).
      if ('placeholder' in el && el.placeholder) { el.setAttribute('data-w2c-ph', el.placeholder); el.placeholder = ''; }
    }
    else el.querySelectorAll('*').forEach(d => { d.style.visibility = 'visible'; });
  } else {
    document.documentElement.style.background = '';
    document.body.style.background = '';
    for (const e of all) e.style.visibility = '';
    const el = document.querySelector(`[data-w2c="${id}"]`);
    if (el) {
      const s = el.getAttribute('data-w2c-c'); if (s != null) { el.style.color = (s === '~' ? '' : s); el.removeAttribute('data-w2c-c'); }
      const ph = el.getAttribute('data-w2c-ph'); if (ph != null) { el.placeholder = ph; el.removeAttribute('data-w2c-ph'); }
    }
  }
}

// Reveal only a merged decoration group (all members tagged data-w2c-grp=gid) so
// a single clip screenshot of their union bakes the whole cluster into one PNG.
function setGroupOnlyFn({ gid, on }) {
  const all = document.querySelectorAll('body *');
  if (on) {
    document.documentElement.style.background = 'transparent';
    document.body.style.background = 'transparent';
    for (const e of all) e.style.visibility = 'hidden';
    document.querySelectorAll(`[data-w2c-grp="${gid}"]`).forEach(el => {
      el.style.visibility = 'visible';
      el.querySelectorAll('*').forEach(d => { d.style.visibility = 'visible'; });
    });
  } else {
    document.documentElement.style.background = '';
    document.body.style.background = '';
    for (const e of all) e.style.visibility = '';
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

// Name a TEXT node by its semantic ROLE inferred from content + context, NOT
// its literal text (which is unstable and meaningless as a node id). Order
// matters: specific content patterns win before the context fallbacks.
function textRole(text, n) {
  const t = (text || '').trim();
  if (!t) return 'text';
  if (/^[^\w一-鿿\s]{1,2}$/.test(t)) return 'icon';            // ◆ ◂ ✕ ⏻ ⤢ ▸ →
  if (/^(\/\/|◆|◇|■|●|▸|►|◂|——|==)/.test(t)) return 'heading';
  if (/^[¥$€£]\s*[\d.,]/.test(t) || /[\d.,]\s*(金币|coins?|元)$/i.test(t)) return 'amount';
  if (/^[\d.,]+\s*%$/.test(t)) return 'percent';
  if (/编号|uid|\bid\b/i.test(t) || /\d{3,}[\s-]\d{3,}/.test(t)) return 'playerId';
  if (/号位|座位/.test(t)) return 'seatLabel';
  if (/(\d+\s*级|lv\.?\s*\d+)/i.test(t)) return 'levelText';
  if (/^(千|万|亿)$/.test(t)) return 'unit';
  if (/^[\d.,]+\s*(千|万|亿|k|m|b)?$/i.test(t)) return 'count';
  if (/^(在线|离线|忙碌|空闲|组队中|匹配中|观战中|准备就绪|已准备|未准备|准备|房主|游客|存活|阵亡|出局|死亡|安全)/.test(t)) return 'status';
  // Long / sentence-like text is descriptive copy, never a name or button.
  if (t.length > 12 || /[，。、；：？！,.;:?!]/.test(t)) return 'hintText';
  if (n && n.clickable) return 'buttonLabel';
  // A short, punctuation-free string next to a portrait is a person's name.
  if (n && n.nearAvatar) return 'username';
  return 'labelText';
}

// One TEXT node per text RUN (a contiguous text node). `txt`/`tr` are the run's
// content/rect; font + color come from the owning element `n`.
function makeTextNode(n, base, txt, tr) {
  if (txt === undefined) { txt = n.text; tr = n.textRect; }
  const node = {
    name: textRole(txt.trim(), n),
    type: 'TEXT',
    transform: { x: +(tr.x - base.x).toFixed(2), y: +(tr.y - base.y).toFixed(2) },
    size: { x: +tr.w.toFixed(2), y: +tr.h.toFixed(2) },
    textData: { characters: txt },
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

// All TEXT nodes for an element's direct text — one per run (see measureDirectText).
function textNodes(n, base) {
  const runs = (n.textRuns && n.textRuns.length) ? n.textRuns : [{ text: n.text, rect: n.textRect }];
  return runs.map(r => makeTextNode(n, base, r.text, r.rect));
}

function mapNode(n, parent) {
  const base = parent ? parent.rect : n.rect;
  const hasText = !!(n.text && n.textRect);
  const singleRun = !n.textRuns || n.textRuns.length <= 1;
  const kids = n.kids || [];
  const solid = solidPaint(n.bg);
  const hasBorder = n.borderW > 0 && n.borderStyle !== 'none';
  const hasRadius = (n.radius || []).some(v => v > 0);
  const boxVisual = !!solid || !!n.raster || hasBorder || hasRadius || !!n.effect;

  // Pure text (no box, no children, one run) -> a single TEXT node.
  if (parent && hasText && !boxVisual && kids.length === 0 && singleRun) {
    const t = makeTextNode(n, base);
    if (n.opacity < 0.999) t.opacity = n.opacity;
    if (n.anim) t.anim = n.anim;
    return t;
  }

  // Otherwise a FRAME box; text (if any) and children go inside it.
  const node = {
    name: n.cname || (n.tag + '_' + (nameCounter++)),
    type: 'FRAME',
    transform: { x: parent ? +(n.rect.x - base.x).toFixed(2) : 0, y: parent ? +(n.rect.y - base.y).toFixed(2) : 0 },
    size: { x: +n.rect.w.toFixed(2), y: +n.rect.h.toFixed(2) },
  };
  // Carry the AI-naming candidate id (stripped before writing) so the live
  // element can be screenshot and the node renamed from the vision pass.
  if (n.cand != null) node._cand = n.cand;
  // Emit source-component identity (kept in the canvas.json) so figo2godot can
  // group every instance of a component type into one prefab.
  if (n.compRoot) { node.comp = n.comp; node.compRoot = true; }
  if (!n.raster) { const rot = rotationDeg(n.transform); if (Math.abs(rot) > 0.1) node.transform.rotation = +rot.toFixed(2); }
  const fills = [];
  // A rasterized node's screenshot already INCLUDES its background (correctly
  // clipped by clip-path). Adding the solid fill underneath would refill the
  // clipped-away corners — opaque bg → cut lost, translucent bg → tinted corner.
  if (solid && !n.raster) fills.push(solid);
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
  // overflow:auto/scroll on an axis -> a scrolling frame (figo ScrollDirection).
  // The browser only paints a scrollbar when content actually overflows; Godot's
  // ScrollContainer AUTO mode replicates that, so mark the axis regardless and
  // let overflow decide at runtime. overflow:hidden stays a plain clip (above).
  const canScroll = v => v === 'auto' || v === 'scroll';
  const sx = canScroll(n.overflowX), sy = canScroll(n.overflowY);
  if (sx || sy) node.scrollDirection = sx && sy ? 'BOTH' : sx ? 'HORIZONTAL' : 'VERTICAL';
  if (n.opacity < 0.999) node.opacity = n.opacity;
  if (n.anim) node.anim = n.anim;

  const children = [];
  if (hasText) for (const t of textNodes(n, n.rect)) children.push(t);  // one per run
  for (const k of kids) children.push(mapNode(k, n));
  if (children.length) node.children = children;
  return node;
}

function rasterMarks(n, acc) {
  if (n.raster && n.rasterHideContent) acc.push({ id: n.raster, clip: n.glowClip });
  for (const k of (n.kids || [])) rasterMarks(k, acc);
  return acc;
}

// ---- captures (states + click-driven flows) -------------------------------

// A "capture" is one screen to grab: an optional nav target (window[navFn](nav))
// plus a list of interaction steps run before the screenshot. Each capture maps
// to one top-level frame -> one .tscn. Build them from --flows (rich) or
// --states (simple nav-only), falling back to a single current-screen capture.
// ---- interactive pick mode -------------------------------------------------
// In-page overlay: a devtools-style hover highlight. Plain clicks/hovers pass
// THROUGH to the page (so the user can open modals/dropdowns, navigate, type to
// stage the UI). Alt+Click picks the element under the cursor (tags it
// data-w2c-pick and signals Node). The freeze key tags the hovered ancestor
// chain (data-w2c-hover) and asks Node to pin :hover via CDP so pure-CSS hover
// UI stays open while the cursor moves away to pick it.
function pickerOverlayFn(pickKey) {
  if (window.__w2cPicker) return;
  window.__w2cPicker = true;
  const mk = css => { const d = document.createElement('div'); d.style.cssText = css; document.documentElement.appendChild(d); return d; };
  const Z = '2147483647';
  const box = mk(`position:fixed;z-index:${Z};pointer-events:none;border:2px solid #4af;background:rgba(68,170,255,.12);box-sizing:border-box;display:none`);
  const tag = mk(`position:fixed;z-index:${Z};pointer-events:none;background:#4af;color:#fff;font:11px/1.4 monospace;padding:1px 5px;border-radius:3px;display:none;white-space:nowrap`);
  const hint = mk(`position:fixed;left:50%;top:10px;transform:translateX(-50%);z-index:${Z};pointer-events:none;background:rgba(0,0,0,.82);color:#fff;font:12px/1.5 system-ui,sans-serif;padding:6px 12px;border-radius:6px`);
  const baseHint = `Alt+点击 = 拾取组件   ${pickKey.toUpperCase()} = 冻结(钉住 :hover/停动画)   普通点击/操作 = 正常交互`;
  hint.textContent = baseHint;
  const mine = el => el === box || el === tag || el === hint;
  let lastEl = null, lastX = 0, lastY = 0, frozen = false;
  function show(el) {
    if (!el) { box.style.display = tag.style.display = 'none'; return; }
    const r = el.getBoundingClientRect();
    box.style.left = r.left + 'px'; box.style.top = r.top + 'px';
    box.style.width = r.width + 'px'; box.style.height = r.height + 'px'; box.style.display = 'block';
    const cls = (typeof el.className === 'string' && el.className.trim()) ? '.' + el.className.trim().split(/\s+/)[0] : '';
    tag.textContent = `${el.tagName.toLowerCase()}${cls}  ${Math.round(r.width)}×${Math.round(r.height)}`;
    tag.style.left = r.left + 'px'; tag.style.top = Math.max(0, r.top - 18) + 'px'; tag.style.display = 'block';
  }
  document.addEventListener('mousemove', e => {
    lastX = e.clientX; lastY = e.clientY;
    const el = document.elementFromPoint(e.clientX, e.clientY);
    if (el && !mine(el)) { lastEl = el; show(el); }
  }, true);
  document.addEventListener('click', e => {
    if (!e.altKey) return;                 // plain click → let the page handle it
    e.preventDefault(); e.stopPropagation(); e.stopImmediatePropagation();
    const el = document.elementFromPoint(e.clientX, e.clientY) || lastEl;
    if (!el) return;
    el.setAttribute('data-w2c-pick', '');
    box.style.display = tag.style.display = hint.style.display = 'none';
    window.__w2cPick();
  }, true);
  document.addEventListener('keydown', e => {
    if (!frozen && e.key.toLowerCase() === pickKey) {
      let el = document.elementFromPoint(lastX, lastY);
      while (el) { if (!mine(el)) el.setAttribute('data-w2c-hover', ''); el = el.parentElement; }
      frozen = true;
      hint.textContent = `❄ 已冻结 — Alt+点击 拾取已显形的组件   Esc 解冻`;
      window.__w2cFreeze();
    } else if (frozen && e.key === 'Escape') {
      document.querySelectorAll('[data-w2c-hover]').forEach(n => n.removeAttribute('data-w2c-hover'));
      frozen = false;
      hint.textContent = baseHint;
      window.__w2cUnfreeze();
    }
  }, true);
}

// Node side: open the picker, wire CDP-based freeze, resolve when the user picks.
async function interactivePick(page, pickKey, at, freezeAt) {
  const cdp = await page.context().newCDPSession(page);
  await cdp.send('DOM.enable').catch(() => {});
  await cdp.send('CSS.enable').catch(() => {});
  await cdp.send('Animation.enable').catch(() => {});
  const forceHover = async (on) => {
    const { root } = await cdp.send('DOM.getDocument', { depth: 0 });
    const { nodeIds } = await cdp.send('DOM.querySelectorAll', { nodeId: root.nodeId, selector: '[data-w2c-hover]' });
    for (const nodeId of nodeIds) {
      await cdp.send('CSS.forcePseudoState', { nodeId, forcedPseudoClasses: on ? ['hover'] : [] }).catch(() => {});
    }
  };
  let resolve;
  const picked = new Promise(r => (resolve = r));
  await page.exposeFunction('__w2cPick', () => resolve());
  await page.exposeFunction('__w2cFreeze', async () => {
    try { await forceHover(true); await cdp.send('Animation.setPlaybackRate', { playbackRate: 0 }); }
    catch (e) { console.error('  freeze:', e.message); }
  });
  await page.exposeFunction('__w2cUnfreeze', async () => {
    try { await forceHover(false); await cdp.send('Animation.setPlaybackRate', { playbackRate: 1 }); }
    catch (e) {}
  });
  await page.evaluate(pickerOverlayFn, pickKey);
  if (at) {   // non-interactive: pick at coordinates (scripted / AI / self-test)
    if (freezeAt) {   // hover the trigger then fire the freeze key (real overlay path)
      await page.mouse.move(freezeAt.x, freezeAt.y);
      await page.waitForTimeout(120);
      await page.keyboard.press(pickKey);
      await page.waitForTimeout(150);
    }
    await page.mouse.move(at.x, at.y);
    await page.waitForTimeout(120);
    await page.keyboard.down('Alt');     // mouse.click ignores modifiers; hold Alt via keyboard
    await page.mouse.click(at.x, at.y);
    await page.keyboard.up('Alt');
  } else {
    console.log('\n  >>> PICK MODE — 在浏览器窗口里把组件准备到位(点开 modal/下拉、切 tab、滚动…)');
    console.log(`      然后 Alt+点击 拾取它。纯 :hover 的 UI:先 hover 出来,按 ${pickKey.toUpperCase()} 冻结,再 Alt+点击。\n`);
  }
  await picked;   // forced :hover stays on through collection so the picked UI keeps rendering
}

function buildCaptures(a) {
  if (a.flows) {
    const raw = JSON.parse(fs.readFileSync(a.flows, 'utf8'));
    if (!Array.isArray(raw)) throw new Error('--flows must be a JSON array of captures');
    return raw.map((c, i) => ({
      name: c.name || ('cap' + i),
      nav: (c.nav !== undefined ? c.nav : null),
      side: c.side || null,            // 'good' | 'bad' — set before the screen mounts
      steps: c.steps || c.do || [],
      // Per-capture post-step settle (ms). Override the global --wait when a step
      // opens a self-dismissing overlay (e.g. the meeting-call演出 auto-advances at
      // 2400ms): a shorter settle collects the overlay before it tears itself down.
      settle: (typeof c.settle === 'number' ? c.settle : null),
    }));
  }
  if (a.states) {
    return a.states.split(',').map(s => s.trim()).filter(Boolean)
      .map(s => ({ name: s, nav: s, steps: [] }));
  }
  return [{ name: null, nav: null, steps: [] }];
}

// Run one interaction step. Format "verb:arg"; bare strings default to click.
// click/hover take a Playwright selector (text=…, css, [attr=…]); nav calls the
// app's nav hook; wait pauses. Each step settles for `defaultWait` ms after
// (except wait, which uses its own duration) so React can render the result.
async function runStep(page, raw, navFn, defaultWait) {
  const s = String(raw).trim();
  const m = /^([a-zA-Z]+):([\s\S]*)$/.exec(s);
  const verb = m ? m[1].toLowerCase() : 'click';
  const arg = (m ? m[2] : s).trim();
  if (verb === 'wait') { await page.waitForTimeout(parseInt(arg, 10) || 0); return; }
  if (verb === 'nav') {
    await page.evaluate(({ st, fn }) => { if (typeof window[fn] === 'function') window[fn](st); }, { st: arg, fn: navFn });
  } else if (verb === 'hover') {
    await page.locator(arg).first().hover({ timeout: 5000 });
  } else { // click (default)
    await page.locator(arg).first().click({ timeout: 5000 });
  }
  await page.waitForTimeout(defaultWait);
}

// ---- AI naming (vision) ---------------------------------------------------

// Replace the rule-based component names (React fiber / CSS class) with names a
// vision model infers from how each component actually LOOKS. Candidates were
// tagged in the collector (data-w2c-cand) and are screenshot live, per screen,
// inside the capture loop. Here we dedup structurally identical components, lay
// the representatives out in a numbered montage, and ask the `claude` CLI to
// name each — then write the name onto every member of its dedup group.

// Cheap structural key: group repeated components (e.g. 11 identical player
// cards) so each unique shape is named once. EXCLUDES text content so cards that
// differ only in their labels still share a name (figo2godot's prefab dedup is
// structural too).
function candKey(node) {
  const w = Math.round((node.size && node.size.x || 0) / 6);
  const h = Math.round((node.size && node.size.y || 0) / 6);
  const kids = node.children || [];
  const types = kids.map(c => {
    const img = (c.fillPaints || []).some(p => p.type === 'IMAGE') ? 'i' : '';
    return (c.type || '?')[0] + img;
  }).sort().join('');
  return `${w}x${h}|${kids.length}|${types}`;
}

// PascalCase, ASCII-only identifier from a model's free-text name.
function sanitizeIdent(s) {
  if (!s || typeof s !== 'string') return null;
  let t = s.replace(/[^A-Za-z0-9]+/g, ' ').trim().split(/\s+/)
    .map(w => w ? w[0].toUpperCase() + w.slice(1) : '').join('');
  if (!t) return null;
  if (/^[0-9]/.test(t)) t = 'C' + t;
  return t.slice(0, 40);
}

// Screenshot every tagged candidate in the CURRENT screen (the elements only
// exist in the DOM while their screen is mounted, so this runs inside the loop).
async function shotCandidates(page, frame, si, dir, records) {
  const found = [];
  (function walk(n) { if (n._cand != null) found.push(n); for (const c of (n.children || [])) walk(c); })(frame);
  for (const n of found) {
    const file = path.join(dir, `cand_${si}_${n._cand}.png`);
    try {
      await page.locator(`[data-w2c-cand="${n._cand}"]`).first().screenshot({ path: file, timeout: 4000 });
      records.push({ node: n, file, key: candKey(n) });
    } catch (e) { /* not screenshot-able (clipped / off-screen) — keep rule name */ }
  }
}

// Render the representatives as a numbered grid and screenshot it into one image
// the model can read in a single call.
async function buildMontage(page, chunk, dir, off) {
  const cells = chunk.map((r, i) => {
    const b64 = fs.readFileSync(r.file).toString('base64');
    return `<div class="cell"><span class="num">${i + 1}</span>` +
           `<img src="data:image/png;base64,${b64}"></div>`;
  }).join('');
  const html = `<!doctype html><meta charset="utf-8"><style>
    body{margin:0;background:#2b2b2b;font-family:Arial,sans-serif}
    .grid{display:flex;flex-wrap:wrap;gap:10px;padding:10px;width:1320px;box-sizing:border-box}
    .cell{position:relative;width:200px;height:200px;background:#444;border:1px solid #666;
          display:flex;align-items:center;justify-content:center;overflow:hidden}
    .cell img{max-width:194px;max-height:194px;object-fit:contain;display:block}
    .num{position:absolute;top:0;left:0;background:#ff3b30;color:#fff;font-weight:700;
         font-size:18px;line-height:1;padding:3px 8px;z-index:2}
  </style><div class="grid">${cells}</div>`;
  // Reuse the main page (all captures are done; we close the browser right
  // after the naming pass, so overwriting its content is safe).
  await page.setViewportSize({ width: 1340, height: 800 });
  await page.setContent(html, { waitUntil: 'load' });
  await page.waitForTimeout(120);
  const mfile = path.join(dir, `montage_${off}.png`);
  await page.locator('.grid').screenshot({ path: mfile });
  return mfile;
}

// Ask the `claude` CLI to name a montage. Headless (`-p`), prompt on stdin to
// dodge Windows arg-quoting, JSON output. Returns a {number: name} map; on any
// failure returns {} so the caller falls back to rule-based names.
function claudeNameMontage(montagePath, count) {
  const prompt =
    `Read the image at ${montagePath.replace(/\\/g, '/')}\n\n` +
    `It is a grid of ${count} game/app UI components, each marked with a red number ` +
    `badge in its top-left corner. For EACH numbered component, infer a short, ` +
    `meaningful PascalCase name from what it visually is and its likely UI role ` +
    `(e.g. PlayerCard, HealthBar, PrimaryButton, AvatarBadge, ScoreCounter, NavBar, ` +
    `SettingsPanel, MinimapFrame, RoleEmblem). Reply with ONLY a JSON object mapping ` +
    `each number (as a string) to its name — no prose, no code fence. ` +
    `Example: {"1":"PlayerCard","2":"HealthBar"}`;
  try {
    const res = spawnSync('claude', ['-p', '--allowedTools', 'Read', '--output-format', 'json'],
      { input: prompt, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024, timeout: 180000, shell: true });
    if (res.status !== 0) {
      console.error(`  ai-name: claude exited ${res.status}: ${(res.stderr || '').slice(0, 200)}`);
      return {};
    }
    let text = res.stdout || '';
    try { const env = JSON.parse(text); if (env && typeof env.result === 'string') text = env.result; } catch (e) {}
    const m = text.match(/\{[\s\S]*\}/);
    if (!m) { console.error('  ai-name: no JSON map in claude reply'); return {}; }
    return JSON.parse(m[0]);
  } catch (e) { console.error('  ai-name: claude call failed:', e.message); return {}; }
}

async function aiNamePass(page, records, dir) {
  // Dedup structurally-identical components — name each unique shape once.
  const groups = new Map();
  for (const r of records) {
    if (!groups.has(r.key)) groups.set(r.key, []);
    groups.get(r.key).push(r);
  }
  const reps = [...groups.values()].map(g => g[0]);
  console.log(`  ai-name: ${records.length} candidates -> ${reps.length} unique components`);
  const CHUNK = 24;
  let renamed = 0;
  for (let off = 0; off < reps.length; off += CHUNK) {
    const chunk = reps.slice(off, off + CHUNK);
    const montage = await buildMontage(page, chunk, dir, off);
    const names = claudeNameMontage(montage, chunk.length);
    chunk.forEach((rep, i) => {
      const nm = sanitizeIdent(names[i + 1] != null ? names[i + 1] : names[String(i + 1)]);
      if (nm) { for (const r of groups.get(rep.key)) r.node.name = nm; renamed++; }
    });
  }
  console.log(`  ai-name: renamed ${renamed}/${reps.length} unique components`);
}

// ---- figoedit preview -----------------------------------------------------

// Launch figoedit on the freshly written canvas.json so the user can eyeball /
// edit the result without a second command. Detached + unref'd so web2canvas
// exits cleanly while the editor stays open. Returns false (with a hint) when
// the binary isn't built yet — never fatal.
function openInFigoedit(file) {
  const exe = path.join(__dirname, '..', '..', 'build', process.platform === 'win32' ? 'figoedit.exe' : 'figoedit');
  if (!fs.existsSync(exe)) {
    console.error(`figoedit not found at ${exe} (build it, then: figoedit ${path.basename(file)})`);
    return false;
  }
  try {
    const child = spawn(exe, [path.resolve(file)], { detached: true, stdio: 'ignore' });
    child.unref();
    console.log(`opening in figoedit -> ${exe}`);
    return true;
  } catch (e) {
    console.error('figoedit launch failed:', e.message);
    return false;
  }
}

// ---- main -----------------------------------------------------------------

(async () => {
  const a = parseArgs(process.argv);
  if (!a.input) { console.error('usage: web2canvas <url|file.html> [-o out] [--root SEL] [--pick] [--pick-key KEY] [--open|--no-open] [--viewport WxH] [--states "a,b,c"] [--flows FILE] [--fonts DIR] [--ai-name] [--browser msedge|chrome] [--scale N]'); process.exit(2); }
  const out = a.out || (a.pick ? 'picked.canvas.json' : a.input.replace(/\.[^.]+$/, '') + '.canvas.json');
  const outDir = path.dirname(path.resolve(out));
  const imagesDir = path.join(outDir, 'images');

  console.log(`launching ${a.browser} ...`);
  const browser = await chromium.launch({ channel: a.browser, headless: a.pickAt ? true : !a.pick });
  const page = await browser.newPage({ viewport: { width: a.vw, height: a.vh }, deviceScaleFactor: a.scale });
  // Many UIs gate animations behind @media (prefers-reduced-motion: no-preference);
  // headless can default to 'reduce', which strips animation-* off the elements so
  // none would be captured. Force motion on.
  await page.emulateMedia({ reducedMotion: 'no-preference' });
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
  // Pick mode: let the user stage + click-select one component in the live page.
  // The picked element is tagged data-w2c-pick; collect just that subtree.
  if (a.pick) {
    await interactivePick(page, a.pickKey, a.pickAt, a.pickFreezeAt);
    a.root = '[data-w2c-pick]';
    a.states = null; a.flows = null;   // a single current-state capture of the pick
  }
  // Captures: each is one screen (nav target + interaction steps) and becomes
  // one top-level frame. --flows gives click-driven popups/overlays; --states is
  // the simple nav-only form; neither → a single current-screen capture.
  const captures = buildCaptures(a);
  const multi = captures.length > 1;
  const frames = [];
  let totShot = 0, totMarks = 0, offsetX = 0;
  // AI naming: per-candidate live screenshots collected screen-by-screen.
  const candDir = path.join(outDir, '.ai-name');
  const candRecords = [];
  if (a.aiName) fs.mkdirSync(candDir, { recursive: true });

  for (let si = 0; si < captures.length; si++) {
    const cap = captures[si];
    if (cap.nav != null) {
      // A previous capture may have opened a SHELL-level overlay (avatar/locker
      // picker) that nav doesn't unmount — it would cover the screen and swallow
      // the next capture's clicks. Escape closes any PopupShell-based overlay.
      await page.keyboard.press('Escape').catch(() => {});
      await page.waitForTimeout(60);
      // Faction must be set BEFORE the screen mounts (the game seeds its tasks
      // from the side at mount and won't rebuild on a later change). Reset every
      // capture so a prior `bad` doesn't leak into a default-good screen.
      await page.evaluate((s) => { if (typeof window.__setSide === 'function') window.__setSide(s); }, cap.side || 'good');
      // Unmount via a sentinel state first so the target screen remounts fresh —
      // clears any popup/local state left open by a previous capture that shared
      // this nav target (e.g. game, game+death, game+settings).
      await page.evaluate(({ st, fn }) => { if (typeof window[fn] === 'function') window[fn](st); }, { st: a.navReset, fn: a.navFn });
      await page.waitForTimeout(80);
      await page.evaluate(({ st, fn }) => { if (typeof window[fn] === 'function') window[fn](st); }, { st: cap.nav, fn: a.navFn });
      await page.waitForTimeout(a.wait);
    }
    // A click-triggered second-level page should contain ONLY what the click
    // opened, not the parent screen behind it. Tag every existing element before
    // the steps; afterwards find the root of the largest NEW subtree (a new
    // element whose parent already existed) and collect from it. This catches
    // both a positioned overlay (chat/settings modal) and an inline panel that
    // REPLACES a sibling (the gift panel swapping the chat bar). A step that just
    // mutates the current screen (a toggle) adds no such subtree → keep the screen.
    let captureRoot = a.root;
    const hasSteps = cap.steps && cap.steps.length;
    if (hasSteps) await page.evaluate(() => document.querySelectorAll('*').forEach(e => e.setAttribute('data-w2c-pre', '1')));
    try {
      const settle = cap.settle != null ? cap.settle : a.wait;
      for (const step of (cap.steps || [])) await runStep(page, step, a.navFn, settle);
    } catch (e) { console.error(`  WARN: step failed for ${cap.name}: ${e.message.split('\n')[0]}`); }
    if (hasSteps) {
      const ovl = await page.evaluate((rootSel) => {
        const root = document.querySelector(rootSel) || document.body;
        const rr = root.getBoundingClientRect(), rootArea = rr.width * rr.height;
        let best = null, bestArea = 0;
        // All boundary roots (a new element whose parent already existed). A modal
        // is ONE such subtree (its dim/backdrop wrapper). A full-screen STATE change
        // (e.g. the meeting intro→discuss stage, which mounts the header, the stage,
        // the skills bar and the chat panel as separate siblings) yields SEVERAL —
        // grouped under one pre-existing parent. Track them so a multi-subtree state
        // change captures the shared parent (whole screen) instead of just the
        // largest sibling, which would drop the rest.
        const boundary = [];
        for (const e of document.querySelectorAll(':not([data-w2c-pre])')) {
          const p = e.parentElement;                                 // boundary = parent already existed
          if (p && !p.hasAttribute('data-w2c-pre') && p !== document.body && p !== document.documentElement) continue;
          const cs = getComputedStyle(e);
          if (cs.display === 'none' || cs.visibility === 'hidden') continue;
          const r = e.getBoundingClientRect(), area = r.width * r.height;
          if (area >= 64) boundary.push({ e, p, area });
          const positioned = cs.position === 'fixed' || cs.position === 'absolute';
          // a full-screen positioned overlay, OR an inline panel that's a real
          // chunk but not a whole-screen re-render
          const ok = (positioned && area >= rootArea * 0.25) ||
                     (area >= rootArea * 0.02 && area <= rootArea * 0.90);
          if (ok && area > bestArea) { best = e; bestArea = area; }
        }
        // Several new sibling subtrees under one parent (inside root) → a screen-wide
        // state change: capture the common parent so all the new panels come along.
        if (best) {
          const groups = new Map();
          for (const b of boundary) {
            if (!b.p) continue;
            const g = groups.get(b.p) || { n: 0, max: 0 };
            g.n++; g.max = Math.max(g.max, b.area); groups.set(b.p, g);
          }
          for (const [parent, g] of groups) {
            if (g.n >= 2 && g.max >= rootArea * 0.25 && parent !== root && root.contains(parent)) {
              parent.setAttribute('data-w2c-ovl', '1');
              return true;
            }
          }
        }
        if (best) { best.setAttribute('data-w2c-ovl', '1'); return true; }
        return false;
      }, a.root);
      if (ovl) captureRoot = '[data-w2c-ovl]';
    }
    // Animated elements are mid-flight at capture: their live transform/opacity
    // would be frozen into BOTH the geometry and the baked sprite, and then the
    // emitted animation track would apply them a SECOND time — double scale,
    // double-dim, and (worst) a transparent ring with a live transform bakes as a
    // whole raster that captures the dark backdrop through its hole as a square.
    // Neutralize transform+opacity to the resting state so the box/sprite are
    // clean and the track animates from a correct base. animation-driven LAYOUT
    // (the voiceprint bars' height) uses neither, so it's left running.
    await page.evaluate(() => {
      for (const el of document.querySelectorAll('*')) {
        const an = getComputedStyle(el).animationName;
        if (an && an !== 'none') {
          el.style.setProperty('transform', 'none', 'important');
          el.style.setProperty('opacity', '1', 'important');
        }
      }
    });

    if (si === 0) await page.screenshot({ path: out.replace(/\.canvas\.json$|\.json$/, '') + '.web.png' }).catch(() => {});

    const res = await page.evaluate(collectorFn, { rootSelector: captureRoot, aiName: a.aiName });
    if (!res.tree) { console.error('WARN: nothing collected for ' + (cap.name || 'page')); continue; }
    const tree = res.tree;
    statePrefix = multi ? (si + '_') : '';

    const marks = rasterMarks(tree, []);
    (function whole(n) {
      if (n.rasterGroup) marks.push({ id: n.raster, group: true, clip: n.groupClip });
      else if (n.raster && !n.rasterHideContent) marks.push({ id: n.raster, whole: true, clip: n.glowClip });
      for (const k of (n.kids || [])) whole(k);
    })(tree);
    if (marks.length) fs.mkdirSync(imagesDir, { recursive: true });
    for (const m of marks) {
      const outPath = path.join(imagesDir, `w2c_${statePrefix}${m.id}.png`);
      try {
        if (m.group) {  // one clip screenshot of the whole decoration cluster
          await page.evaluate(setGroupOnlyFn, { gid: m.id, on: true });
          await page.screenshot({ path: outPath, clip: m.clip, omitBackground: true });
        } else if (m.clip) {  // raster with an outer glow — capture the expanded box
          await page.evaluate(setBgOnlyFn, { id: m.id, on: true, hideKids: !m.whole });
          await page.screenshot({ path: outPath, clip: m.clip, omitBackground: true });
        } else {
          await page.evaluate(setBgOnlyFn, { id: m.id, on: true, hideKids: !m.whole });
          await page.locator(`[data-w2c="${m.id}"]`).screenshot({ path: outPath, omitBackground: true });
        }
        totShot++;
      } catch (e) { /* not screenshot-able */ }
      finally {
        if (m.group) await page.evaluate(setGroupOnlyFn, { gid: m.id, on: false });
        else await page.evaluate(setBgOnlyFn, { id: m.id, on: false, hideKids: !m.whole });
      }
    }
    totMarks += marks.length;

    nameCounter = 0;
    const frame = mapNode(tree, null);
    frame.name = cap.name || 'Page';
    frame.scrollDirection = 'VERTICAL';
    frame.transform = { x: offsetX, y: 0 };
    offsetX += (frame.size.x || res.rootW) + 60;
    frames.push(frame);
    // Screenshot this screen's naming candidates while it's still mounted.
    if (a.aiName) await shotCandidates(page, frame, si, candDir, candRecords);
    console.log(`  captured ${frame.name} (${marks.length} rasters)`);
  }
  // Vision-name the components (needs the browser for montage rendering).
  if (a.aiName && candRecords.length) {
    try { await aiNamePass(page, candRecords, candDir); }
    catch (e) { console.error('ai-name pass failed:', e.message); }
  }
  if (a.aiName) frames.forEach(f => (function strip(n) { delete n._cand; for (const c of (n.children || [])) strip(c); })(f));
  await browser.close();
  if (server) server.close();
  if (!frames.length) { console.error('FAIL: no frames captured'); process.exit(1); }

  const doc = {
    document: { type: 'DOCUMENT', children: [{ type: 'CANVAS', name: 'Page 1', children: frames }] },
    styles: {},
  };
  fs.writeFileSync(out, JSON.stringify(doc, null, 2));
  console.log(`RESULT: OK  ${frames.length} frame(s)  ${totShot}/${totMarks} rasters -> ${out}`);
  // Interactive pick (a human just Alt-clicked in a headed browser) previews in
  // figoedit by default — override with --no-open. The headless/script paths
  // (--pick-at, plain captures) leave the glue to the AI/pipeline layer
  // (Bash + MCP open_document) and open only on explicit --open.
  if (a.open !== null ? a.open : (a.pick && !a.pickAt)) openInFigoedit(out);
})().catch(e => { console.error('FAIL:', e.message); process.exit(1); });
