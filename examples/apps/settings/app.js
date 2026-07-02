// settings — benchmark app #2. Two frames (Settings / About), pre-baked
// switch states flipped with setVisible, state persisted in localStorage.
// Rows and switches are addressed by unique layer names.

ui.selectFrame(APP.entryFrame || "Settings");

// Deterministic bench start: the storage file survives between runs.
if (globalThis.SELFDRIVE) localStorage.removeItem("settings");

// ---- state (re-read on hot reload -> re-entrant) ----
const DEFAULTS = { notif: false, dark: false, sound: true };
const SWITCH = { notif: "SwitchNotif", dark: "SwitchDark", sound: "SwitchSound" };

let state = Object.assign({}, DEFAULTS);
try {
    Object.assign(state, JSON.parse(localStorage.getItem("settings") || "{}"));
} catch (e) { /* corrupt storage -> defaults */ }

function save() { localStorage.setItem("settings", JSON.stringify(state)); }

function apply(key) {
    ui.setVisible(SWITCH[key] + "On", !!state[key]);
    ui.setVisible(SWITCH[key] + "Off", !state[key]);
}

function toggle(key) {
    state[key] = !state[key];
    save();
    apply(key);
}

// ---- wire rows ----
ui.onClick("Row Notifications", () => toggle("notif"));
ui.onClick("Row Dark Mode", () => toggle("dark"));
ui.onClick("Row Sounds", () => toggle("sound"));
ui.onClick("Row About", () => ui.navigateTo("About", "slideLeft", 0.28));
ui.onClick("BackBtn", () => ui.navigateBack(0.28));

// press feedback
for (const n of ["Row Notifications", "Row Dark Mode", "Row Sounds",
                 "Row About", "BackBtn"]) {
    ui.onHover(n, (node, entered) => ui.setOpacity(node, entered ? 0.82 : -1.0));
}

for (const k of Object.keys(SWITCH)) apply(k);
console.log("settings ready");

// ---- unattended tour (figoplay --selfdrive) ----
// shots: frame 30 -> <prefix>_home.png (initial home),
//        frame 110 -> <prefix>_nav.png (About page), host exits at frame 140.
// Note: ui.transitionProgress() is not exposed to JS, so transitions are
// gated by frame count (0.28s at 60fps vsync = ~17 frames).
if (globalThis.SELFDRIVE) {
    const seq = [
        // frames 1..30: untouched home for the _home shot
        [34, "Row Notifications"],  // off -> on
        [44, "Row About"],          // slideLeft to About (~done by frame 61)
        [62, "BackBtn"],            // back to Settings (~done by frame 79)
        [82, "Row Notifications"],  // on -> off (idempotence)
        [86, "Row About"],          // About again, settled before the 110 shot
    ];
    let frames = 0;  // count frames, not dt: first dt includes file load
    const checks = [];
    ui.onUpdate(() => {
        frames++;
        for (const [at, name] of seq) {
            if (frames === at) console.log(`selfdrive tap ${name} ->`, ui.tap(name));
        }
        if (frames === 40) checks.push(["notif on", String(state.notif), "true"]);
        if (frames === 60) checks.push(["nav about", ui.currentFrame().name, "About"]);
        if (frames === 80) checks.push(["back home", ui.currentFrame().name, "Settings"]);
        if (frames === 84) {
            checks.push(["notif off", String(state.notif), "false"]);
            const raw = localStorage.getItem("settings") || "";
            checks.push(["persisted", raw.includes('"notif":false') ? "yes" : "no", "yes"]);
        }
        if (frames === 122) {
            checks.push(["nav about again", ui.currentFrame().name, "About"]);
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
