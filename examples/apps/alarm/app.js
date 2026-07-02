// alarm — benchmark app: first real-world exercise of the scroll-snap wheel
// picker (node.snapToChildren / node.snapIndex, ui.onScrollEnd, ui.snapTo).
//
// Two frames:
//   Alarms : big title + bindList of alarm rows (time / label / pre-baked
//            switch pair flipped with setVisible) + "New Alarm" button.
//            Long-press a row -> demo countdown overlay -> playSound.
//   Picker : two wheel columns (hours 0-23, minutes 00-55 step 5), each a
//            FIXED-height scrolling frame with snapToChildren; the wheels
//            open on the current time (ui.snapTo instant), onScrollEnd
//            drives the live "HH:MM" preview. Save appends an alarm.
//
// Wheel geometry (must match gen_design.py): row pitch 56, viewport 280,
// paddingTop 112 so the snapped child sits on the CENTER row (snapTo aligns
// child i with the first child's start, and the first child starts on the
// middle row). paddingBottom does NOT extend the scroll extent (it ends at
// the last child's bottom), so each wheel appends TWO PHANTOM blank rows as
// bottom lead-out instead. Boundary i = i*56; hour maxScrollY = 23*56 = 1288,
// minute maxScrollY = 11*56 = 616 — every real value can reach the center.

ui.selectFrame(APP.entryFrame || "Alarms");

// Deterministic bench start: the storage file survives between runs.
if (globalThis.SELFDRIVE) localStorage.removeItem("alarms");

// ---- state (re-read on hot reload -> re-entrant) ----
const DEFAULTS = [
    { h: 6, m: 30, label: "Wake up", on: true },
    { h: 22, m: 0, label: "Wind down", on: false },
];
let alarms = null;
try { alarms = JSON.parse(localStorage.getItem("alarms") || "null"); }
catch (e) { /* corrupt storage -> defaults */ }
if (!Array.isArray(alarms) || alarms.length === 0)
    alarms = DEFAULTS.map(a => Object.assign({}, a));

const pad2 = v => String(v).padStart(2, "0");
const hhmm = (h, m) => pad2(h) + ":" + pad2(m);
function save() { localStorage.setItem("alarms", JSON.stringify(alarms)); }

// ---- alarm list ----
function renderList() {
    ui.bindList("AlarmList", alarms.length, (item, i) => {
        const a = alarms[i];
        item.find("RowTime").text = hhmm(a.h, a.m);
        item.find("RowLabel").text = a.label;
        item.find("RowSwOn").visible = a.on;
        item.find("RowSwOff").visible = !a.on;
        item.find("RowLine").visible = i < alarms.length - 1;  // no last hairline
    });
}
renderList();

// tap a row = toggle its switch (rows share the name; the callback gets the
// matched clone, node.index = row index in AlarmList)
ui.onClick("AlarmRow", (node) => {
    const a = alarms[node.index];
    if (!a) return;
    a.on = !a.on;
    save();
    node.find("RowSwOn").visible = a.on;
    node.find("RowSwOff").visible = !a.on;
});

// ---- wheels (bind once; snapToChildren is a runtime flag, re-set after
// any rebuild — these two are never rebuilt) ----
const WHEEL_ROW = 56;
const HOURS = 24, MINUTES = 12;  // real rows; +2 phantom lead-out rows each
ui.bindList("HourWheel", HOURS + 2,
    (item, i) => { item.find("HourText").text = i < HOURS ? pad2(i) : ""; });
ui.bindList("MinuteWheel", MINUTES + 2,
    (item, i) => { item.find("MinuteText").text = i < MINUTES ? pad2(i * 5) : ""; });
ui.find("HourWheel").snapToChildren = true;
ui.find("MinuteWheel").snapToChildren = true;

let pending = { h: 7, m: 0 };
function preview() { ui.setText("PreviewTime", hhmm(pending.h, pending.m)); }

const scrollEnds = [];  // ["h"|"m", index] log, used by the SELFDRIVE tour
ui.onScrollEnd("HourWheel", (n, x, y, idx) => {
    if (idx >= 0) { pending.h = Math.min(idx, HOURS - 1); preview(); }
    scrollEnds.push(["h", idx]);
});
ui.onScrollEnd("MinuteWheel", (n, x, y, idx) => {
    if (idx >= 0) { pending.m = Math.min(idx, MINUTES - 1) * 5; preview(); }
    scrollEnds.push(["m", idx]);
});

// ---- navigation ----
function openPicker() {
    const now = new Date();
    // nearest 5-minute notch; :58+ rounds to :00 without bumping the hour
    const mi = Math.round(now.getMinutes() / 5) % 12;
    pending = { h: now.getHours(), m: mi * 5 };
    preview();
    ui.navigateTo("Picker", "slideUp", 0.3);
    ui.snapTo("HourWheel", pending.h, 0);   // instant; fires onScrollEnd
    ui.snapTo("MinuteWheel", mi, 0);
}
ui.onClick("BtnNew", openPicker);
ui.onClick("BtnCancel", () => ui.navigateBack(0.3));
ui.onClick("BtnSave", () => {
    alarms.push({ h: pending.h, m: pending.m, label: "Alarm", on: true });
    save();
    renderList();            // structural mutation: don't touch node args after
    ui.navigateBack(0.3);
});

// ---- ring demo (no real notifications yet: G2 device support pending) ----
// Long-press an alarm row -> countdown overlay -> playSound + "Time's up".
// Tapping the overlay only hides the notice; the pending ring still fires
// and re-shows the overlay (dismiss the note, not the alarm).
const DEMO_SECS = globalThis.SELFDRIVE ? 2 : 5;
let ringResult = null;   // last ui.playSound return (bench introspection)
let demoTimer = null;
ui.onLongPress("AlarmRow", (node) => {
    const a = alarms[node.index];
    ui.setText("DemoTitle", "Alarm preview" + (a ? " " + hhmm(a.h, a.m) : ""));
    ui.setText("DemoText", "Rings in " + DEMO_SECS + " seconds");
    ui.setVisible("DemoOverlay", true);
    if (demoTimer !== null) clearTimeout(demoTimer);
    demoTimer = setTimeout(() => {
        ringResult = ui.playSound("sounds/ring.wav");
        console.log("alarm ring played=" + ringResult);
        ui.setText("DemoText", "Time's up");
        ui.setText("DemoHint", "Tap anywhere to close");
        ui.setVisible("DemoOverlay", true);
        demoTimer = null;
    }, DEMO_SECS * 1000);
});
ui.onClick("DemoOverlay", () => ui.setVisible("DemoOverlay", false));

// press feedback (plain frames, not component instances -> no autoStates)
for (const n of ["BtnNew", "BtnCancel", "BtnSave", "AlarmRow"]) {
    ui.onHover(n, (node, entered) => ui.setOpacity(node, entered ? 0.85 : -1.0));
}

console.log("alarm ready");

// ---- unattended tour (figoplay --selfdrive) ----
// shots: frame 30 -> <prefix>_home.png (untouched Alarms),
//        frame 110 -> <prefix>_nav.png (Picker wheels at 07:35),
//        host exits at frame 140 (2.33s at the 60fps cap), so the 2s demo
//        ring MUST start in the first frames to land before the final checks.
if (globalThis.SELFDRIVE) {
    const checks = [];
    const near = (v, want, tol) => Math.abs(v - want) <= tol;
    const chk = (what, got, want) => checks.push([what, String(got), String(want)]);

    let frames = 0;  // count frames, not dt: first dt includes file load
    ui.onUpdate(() => {
        frames++;
        if (frames === 4) {
            chk("initial rows", ui.find("AlarmList").childCount, 2);
            chk("row0 time", ui.find("AlarmList").child(0).find("RowTime").text, "06:30");
            // extent = paddingTop + (real+2 phantom) rows - viewport:
            // exactly boundary of the LAST real value = it can center
            chk("hour maxScrollY", near(ui.find("HourWheel").maxScrollY, 23 * WHEEL_ROW, 1), "true");
            chk("minute maxScrollY", near(ui.find("MinuteWheel").maxScrollY, 11 * WHEEL_ROW, 1), "true");
            chk("snap flag set", ui.find("HourWheel").snapToChildren, "true");
            // start the 2s ring demo NOW so it lands before frame ~130
            chk("longPress row", ui.longPress("AlarmRow"), "true");
        }
        if (frames === 8) {
            chk("demo overlay shown", ui.find("DemoOverlay").visible, "true");
            chk("demo countdown text", ui.find("DemoText").text, "Rings in 2 seconds");
            chk("dismiss tap", ui.tap("DemoOverlay"), "true");
        }
        if (frames === 12) {
            chk("demo overlay dismissed", ui.find("DemoOverlay").visible, "false");
            ui.tap("AlarmRow");  // toggle row 0: on -> off
        }
        if (frames === 16) {
            chk("toggle off", alarms[0].on, "false");
            chk("toggle off shown", ui.find("AlarmList").child(0).find("RowSwOff").visible, "true");
            ui.tap("AlarmRow");  // back on (idempotence)
        }
        if (frames === 20) chk("toggle on again", alarms[0].on, "true");

        // frames 20..30: untouched Alarms for the _home shot
        if (frames === 32) chk("tap new", ui.tap("BtnNew"), "true");
        if (frames === 56) {  // slideUp 0.3s is long done
            chk("on picker", ui.currentFrame().name, "Picker");
            // wheels opened on the current time (instant snapTo at open)
            chk("hour opens at now", ui.find("HourWheel").snapIndex, pending.h);
            chk("minute opens at now", ui.find("MinuteWheel").snapIndex * 5, pending.m);
            chk("preview shows now", ui.find("PreviewTime").text, hhmm(pending.h, pending.m));
            chk("open scroll-ends", scrollEnds.length >= 2, "true");
        }
        if (frames === 58) {
            // instant snapTo (duration 0): applies + fires onScrollEnd now.
            // Eased snapTo settle time is covered by _snap_regress; here the
            // 140-frame budget goes to the drag-release snap path below.
            chk("snapTo hour 7", ui.snapTo("HourWheel", 7, 0), "true");
            chk("snapTo minute 30", ui.snapTo("MinuteWheel", 6, 0), "true");
        }
        if (frames === 60) {
            chk("hour at 7", near(ui.find("HourWheel").scrollY, 7 * WHEEL_ROW, 0.5), "true");
            chk("hour snapIndex 7", ui.find("HourWheel").snapIndex, 7);
            chk("minute snapIndex 6", ui.find("MinuteWheel").snapIndex, 6);
            chk("preview 07:30", ui.find("PreviewTime").text, "07:30");
        }
        if (frames === 62) {
            // slow one-tick drag on the minute wheel (zero release velocity):
            // travel 40 < row 56 -> rests at 376 -> snaps to boundary 392
            ui.pointerDown(290, 470);
            ui.pointerMove(290, 455);
            ui.pointerMove(290, 442);
            ui.pointerMove(290, 430);
            ui.pointerUp(290, 430);
        }
        if (frames === 104) {  // snap easing settled well before the 110 shot
            const y = ui.find("MinuteWheel").scrollY;
            chk("drag snapped to pitch", near(y % WHEEL_ROW, 0, 0.5) ? "true"
                : "y=" + y, "true");
            chk("drag snapIndex 7", ui.find("MinuteWheel").snapIndex, 7);
            chk("preview 07:35", ui.find("PreviewTime").text, "07:35");
        }
        // frame 110: _nav shot = Picker wheels at 07:35
        if (frames === 112) chk("tap save", ui.tap("BtnSave"), "true");
        if (frames === 134) {  // slideDown done; 2s ring long fired
            chk("back home", ui.currentFrame().name, "Alarms");
            chk("3 alarms", alarms.length, 3);
            chk("list rows 3", ui.find("AlarmList").childCount, 3);
            chk("new row time", ui.find("AlarmList").child(2).find("RowTime").text, "07:35");
            chk("persisted", (localStorage.getItem("alarms") || "").includes('"h":7'), "true");
            chk("ring text", ui.find("DemoText").text, "Time's up");
            chk("ring overlay reshown", ui.find("DemoOverlay").visible, "true");
            // playSound returned a real verdict (true here on figoplay/raylib;
            // the log line keeps the actual value visible)
            chk("ring playSound returned", ringResult !== null, "true");
            console.log("alarm ring result=" + ringResult);
        }
        if (frames === 136) {
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
