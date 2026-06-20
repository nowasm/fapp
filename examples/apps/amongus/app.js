// Among Us — a space-werewolf game UI demo (figo test case).
// An app = design + logic. 5 frames wired into a game flow:
//   MainMenu -> Lobby -> RoleReveal -> GameHUD <-> Meeting (voting)
// The script is idempotent so figoplay hot-reload re-runs cleanly.

ui.selectFrame(APP.entryFrame || "MainMenu");

// ---- shared game state (re-created on every (re)run; keeps reloads clean) ----
// The 7 vote rows + their colors live in the design (the JS API can set text /
// visibility / opacity, but not fills — so per-player color is baked in).
const PLAYERS = ["Blue", "Green", "Pink", "Orange", "Cyan", "Lime", "Purple"];
let votedFor = -1;   // index into PLAYERS, -1 = none

// ---------- MainMenu ----------
ui.onClick("btn-online",   () => ui.navigateTo("Lobby", "slideLeft", 0.3));
ui.onClick("btn-local",    () => ui.navigateTo("Lobby", "slideLeft", 0.3));
ui.onClick("btn-freeplay", () => startMatch());
for (const b of ["btn-online", "btn-local", "btn-freeplay", "btn-settings", "btn-howto"]) {
    ui.onHover(b, (n, e) => ui.setOpacity(n, e ? 0.85 : -1.0));
}

// ---------- Lobby ----------
ui.onClick("btn-lobby-back", () => ui.navigateBack(0.3));
ui.onClick("btn-start", () => startMatch());

// startMatch: dramatic role reveal, then drop into the round.
function startMatch() {
    ui.selectFrame("RoleReveal");
    setTimeout(() => ui.navigateTo("GameHUD", "fade", 0.4), 2600);
}

// ---------- RoleReveal ---------- (tap anywhere to skip the wait)
ui.onClick("role-mate", () => ui.navigateTo("GameHUD", "fade", 0.4));
ui.onClick("role-name", () => ui.navigateTo("GameHUD", "fade", 0.4));

// ---------- GameHUD ----------
let tasksLeft = 3;
ui.onClick("btn-report", () => openMeeting());
ui.onClick("btn-use", () => {
    // "complete a task": tick the counter (bar fill is decorative/static).
    if (tasksLeft > 0) tasksLeft--;
    ui.setText("task-pct", tasksLeft === 0 ? "DONE" : tasksLeft + " left");
    ui.setText("use-lbl", tasksLeft === 0 ? "✓" : "USE");
});
ui.onClick("btn-kill", () => openMeeting());
for (const b of ["btn-use", "btn-report", "btn-kill", "btn-map", "btn-menu", "btn-sabotage"]) {
    ui.onHover(b, (n, e) => ui.setOpacity(n, e ? 0.85 : -1.0));
}

// ---------- Meeting / voting ----------
function openMeeting() {
    votedFor = -1;
    ui.navigateTo("Meeting", "slideUp", 0.35);
    renderVotes();
}

// Reflect vote state onto the 7 baked rows: highlight the chosen row, flip its
// ✕ to the green ✓, update the status line.
function renderVotes() {
    for (let i = 0; i < PLAYERS.length; i++) {
        const voted = votedFor === i;
        ui.setVisible("vote-x-" + i, !voted);
        ui.setVisible("vote-ok-" + i, voted);
        ui.setText("row-status-" + i, voted ? "YOU VOTED" : "tap to vote");
        ui.setOpacity("vote-row-" + i, votedFor === -1 || voted ? -1.0 : 0.55);
    }
}

function castVote(i) {
    votedFor = (votedFor === i) ? -1 : i;
    renderVotes();
}

// Wire each player row once (handlers survive re-runs; idempotent).
for (let i = 0; i < PLAYERS.length; i++) {
    const idx = i;
    ui.onClick("vote-row-" + i, () => castVote(idx));
    ui.onHover("vote-row-" + i, (n, e) => { if (votedFor === -1) ui.setOpacity(n, e ? 0.8 : -1.0); });
}

ui.onClick("btn-skip", () => {
    // skip -> resolve meeting, back to the round
    ui.navigateTo("GameHUD", "slideDown", 0.35);
});
ui.onHover("btn-skip", (n, e) => ui.setOpacity(n, e ? 0.85 : -1.0));

// Initialize the vote rows (idempotent; also keeps hot-reload shots consistent).
renderVotes();

// For static --shot verification of the Meeting screen, pre-cast a vote so the
// screenshot exercises the ✓ / dim states (no effect during normal play).
if (globalThis.SHOT && (APP.entryFrame === "Meeting")) castVote(2);

console.log("Among Us ready —", ui.frameNames().join(", "));

// ---------- SELFDRIVE: scripted tour for screenshot verification ----------
if (globalThis.SELFDRIVE) {
    ui.onUpdate(() => {});                        // keep the clock ticking
    setTimeout(() => ui.tap("btn-online"), 500);  // menu -> lobby
    setTimeout(() => ui.tap("btn-start"),  1400); // lobby -> role reveal
    setTimeout(() => ui.tap("role-mate"),  4300); // -> game HUD
    setTimeout(() => ui.tap("btn-use"),    4900); // complete a task
    setTimeout(() => ui.tap("btn-report"), 5600); // -> meeting
    setTimeout(() => ui.tap("vote-row-2"), 6400); // cast a vote (Pink)
}
