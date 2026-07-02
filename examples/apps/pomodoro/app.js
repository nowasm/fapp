// pomodoro — benchmark app #2. Timer state machine + setText/setVisible/setOpacity.
// Focus 25:00 / Break 05:00, Start/Pause toggle, Reset, 4 round dots.

ui.selectFrame(APP.entryFrame || "Pomodoro");

// ---- state (module-level; reset on hot reload is fine) ----
const DUR = { focus: 25 * 60, break: 5 * 60 };
let mode = "focus";
let remaining = DUR[mode];
let running = false;
let rounds = 0;          // completed focus rounds, 0..4

function mmss(s) {
    const m = Math.floor(s / 60), sec = s % 60;
    return `${String(m).padStart(2, "0")}:${String(sec).padStart(2, "0")}`;
}

function render() {
    ui.setText("Timer", mmss(remaining));
    ui.setText("BtnStartLabel", running ? "Pause" : "Start");
    ui.setVisible("PillFocusOn", mode === "focus");
    ui.setVisible("PillFocusOff", mode !== "focus");
    ui.setVisible("PillBreakOn", mode === "break");
    ui.setVisible("PillBreakOff", mode !== "break");
    for (let i = 1; i <= 4; i++) ui.setOpacity(`Dot${i}`, i <= rounds ? 1.0 : 0.25);
}

function setMode(m) {
    mode = m;
    remaining = DUR[m];
    running = false;
    render();
}

function tick() {
    if (!running) return;
    remaining--;
    if (remaining <= 0) {
        if (mode === "focus") {
            rounds = Math.min(rounds + 1, 4);
            setMode("break");
            // Phase-change chime. Purely informational: playSound returns
            // false in silent environments (web / no audio device) and the
            // bench never depends on it.
            console.log("pomodoro ding played=" + ui.playSound("sounds/ding.wav"));
        } else {
            if (rounds >= 4) rounds = 0;   // new cycle after the 4th round
            setMode("focus");
        }
        return;
    }
    render();
}

// one interval for the app's lifetime; guard so hot reload doesn't stack them
if (globalThis.__pomoTimer !== undefined) clearInterval(globalThis.__pomoTimer);
globalThis.__pomoTimer = setInterval(tick, 1000);

// ---- wire controls ----
ui.onClick("BtnStart", () => { running = !running; render(); });
ui.onClick("BtnReset", () => { mode = "focus"; remaining = DUR.focus;
                               running = false; rounds = 0; render(); });
ui.onClick("PillFocus", () => setMode("focus"));
ui.onClick("PillBreak", () => setMode("break"));

for (const n of ["BtnStart", "BtnReset", "PillFocus", "PillBreak"]) {
    ui.onHover(n, (node, entered) => ui.setOpacity(node, entered ? 0.85 : -1.0));
}

render();
console.log("pomodoro ready");

// ---- unattended tour (figoplay --selfdrive) ----
// shots: frame 30 -> <prefix>_home.png (running), frame 110 -> <prefix>_nav.png (reset)
if (globalThis.SELFDRIVE) {
    let frames = 0;  // count frames, not dt: first dt includes file load
    const checks = [];
    const chk = (what, got, want) => checks.push([what, String(got), String(want)]);
    ui.onUpdate(() => {
        frames++;
        if (frames === 10) ui.tap("PillBreak");
        if (frames === 14) { chk("break mode", ui.find("Timer").text, "05:00");
                             ui.tap("PillFocus"); }
        if (frames === 18) { chk("focus mode", ui.find("Timer").text, "25:00");
                             ui.tap("BtnStart"); }
        if (frames === 22) chk("running label", ui.find("BtnStartLabel").text, "Pause");
        // frame 30: home shot shows the timer running
        if (frames === 100) {  // ~1.4s after Start at vsync: >=1 tick elapsed
            const t = ui.find("Timer").text;
            chk("countdown", t === "25:00" ? "stuck at 25:00" : "ticked", "ticked");
            ui.tap("BtnStart");  // pause
        }
        if (frames === 104) ui.tap("BtnReset");
        if (frames === 108) { chk("reset timer", ui.find("Timer").text, "25:00");
                              chk("reset label", ui.find("BtnStartLabel").text, "Start"); }
        // frame 110: nav shot shows the reset state
        if (frames === 120) {
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
