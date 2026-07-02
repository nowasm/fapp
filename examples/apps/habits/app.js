// habits — benchmark app #2. bindList rows + setVisible check toggles +
// localStorage persistence.
//
// Engine notes:
// - The list is bound ONCE; toggles mutate row text/visibility in place.
//   (Calling bindList inside an onClick handler destroys the node mid-
//   dispatch and the handler walk reads freed nodes — see bench notes.)
// - node.visible has no JS getter (write-only), so the bench verifies via
//   label text and localStorage instead.

ui.selectFrame(APP.entryFrame || "Habits");

const HABITS = [
    { name: "喝水", base: 8 },   // base = streak before today
    { name: "阅读", base: 21 },
    { name: "运动", base: 3 },
    { name: "早睡", base: 12 },
    { name: "冥想", base: 5 },
];
const STORE_KEY = "habits.done.v1";
const DONE_SUFFIX = " · 今日已打卡";

// Bench runs must be idempotent: wipe persisted state before loading it.
if (globalThis.SELFDRIVE) localStorage.removeItem(STORE_KEY);

let done = HABITS.map(() => false);
try {
    const saved = JSON.parse(localStorage.getItem(STORE_KEY) || "[]");
    if (Array.isArray(saved)) done = HABITS.map((_, i) => !!saved[i]);
} catch (e) { /* corrupt store -> start fresh */ }

function save() {
    localStorage.setItem(STORE_KEY, JSON.stringify(done));
}

function streakText(i) {
    const streak = HABITS[i].base + (done[i] ? 1 : 0);
    return done[i] ? `连续 ${streak} 天${DONE_SUFFIX}` : `连续 ${streak} 天`;
}

// Row visuals from state. `row` is a live HabitRow node (not held across
// binds — the list is only ever bound once per script run).
function applyRow(row, i) {
    row.find("StreakLabel").text = streakText(i);
    row.find("CheckOn").visible = done[i];
    row.find("CheckOff").visible = !done[i];
    row.opacity = done[i] ? 0.72 : 1.0;
}

function applyHeader() {
    const n = done.filter(Boolean).length;
    ui.setText("ProgressLabel", `${n}/5`);
    for (let i = 0; i < 5; i++) ui.setVisible(`SegFill${i}`, i < n);
    ui.setVisible("BadgeOn", n === 5);
    ui.setVisible("BadgeOff", n !== 5);
}

// One-time list bind (re-entrant: hot reload re-runs this whole script and
// bindList reuses the stored template).
ui.bindList("HabitList", HABITS.length, (item, i) => {
    item.find("HabitName").text = HABITS[i].name;
    for (let k = 0; k < 5; k++) item.find(`Icon${k}`).visible = k === i;
    applyRow(item, i);
});
applyHeader();

// header date (app renders today's date at load)
{
    const d = new Date();
    const wd = "日一二三四五六"[d.getDay()];
    ui.setText("DateLabel", `${d.getMonth() + 1}月${d.getDate()}日 周${wd}`);
}

ui.onClick("HabitRow", (node) => {
    const i = node.index;  // clone position in the list container
    if (i >= 0 && i < HABITS.length) {
        done[i] = !done[i];
        save();
        applyRow(node, i);
        applyHeader();
    }
});
ui.onHover("HabitRow", (node, entered) => {
    if (!done[node.index]) ui.setOpacity(node, entered ? 0.88 : -1.0);
});

console.log("habits ready");

// ---- unattended tour (figoplay --selfdrive) ----
// figoplay shoots frame 30 -> <prefix>_home.png (initial state) and
// frame 110 -> <prefix>_nav.png (after check-ins), exits at 140.
// Tour: check rows 1 & 3 -> verify 2/5, uncheck row 1 -> verify 1/5,
// re-check row 1 so the nav shot shows 2/5.
if (globalThis.SELFDRIVE) {
    let frames = 0;  // count frames, not dt: first dt includes file load
    const checks = [];
    const row = (i) => ui.findAll("HabitRow")[i];
    const isDone = (i) => row(i).find("StreakLabel").text.includes(DONE_SUFFIX);
    const push = (what, got, want) => checks.push([what, String(got), String(want)]);
    ui.onUpdate(() => {
        frames++;
        if (frames === 40) console.log("selfdrive tap row0 ->", ui.tap(row(0)));
        if (frames === 46) console.log("selfdrive tap row2 ->", ui.tap(row(2)));
        if (frames === 55) {
            push("progress after 2 taps", ui.find("ProgressLabel").text, "2/5");
            push("row0 checked", isDone(0), true);
            push("row2 checked", isDone(2), true);
            push("row1 unchecked", isDone(1), false);
            push("row0 streak +1", row(0).find("StreakLabel").text.includes("9"), true);
        }
        if (frames === 65) console.log("selfdrive untap row0 ->", ui.tap(row(0)));
        if (frames === 75) {
            push("progress after uncheck", ui.find("ProgressLabel").text, "1/5");
            push("row0 unchecked again", isDone(0), false);
            push("persisted state", localStorage.getItem(STORE_KEY),
                 JSON.stringify([false, false, true, false, false]));
        }
        if (frames === 85) console.log("selfdrive retap row0 ->", ui.tap(row(0)));
        if (frames === 125) {
            push("final progress", ui.find("ProgressLabel").text, "2/5");
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
