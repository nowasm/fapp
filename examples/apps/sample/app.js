// Starfall Menu — the reference "standard app" (an app = design + logic).
// Run it as a project directory:  figoplay examples/apps/sample
// figoplay reads app.json (design/script/viewport/entryFrame) and exposes the
// manifest as globalThis.APP. The script stays idempotent so hot-reload re-runs
// cleanly.

// app.json already selected MainMenu as the entry frame; this is just explicit.
ui.selectFrame(APP.entryFrame || "MainMenu");

ui.onClick("btn-start", () => {
    ui.setText("subtitle", "Loading…");
    console.log("start game");
});

ui.onClick("btn-options", () => ui.navigateTo("Settings", "slideLeft", 0.28));
ui.onClick("btn-back", () => ui.navigateBack(0.28));
ui.onClick("btn-quit", () => console.log("quit"));

// Hover feedback on the menu buttons.
for (const name of ["btn-start", "btn-options", "btn-quit", "btn-back"]) {
    ui.onHover(name, (node, entered) => ui.setOpacity(node, entered ? 0.85 : -1.0));
}

console.log("Starfall ready —", ui.frameNames().length, "frames, app:", APP.name);

// SELFDRIVE: a scripted tour for screenshot verification (figoplay --selfdrive).
if (globalThis.SELFDRIVE) {
    ui.onUpdate(() => {});  // keep the clock ticking
    setTimeout(() => ui.tap("btn-options"), 600);  // home -> settings for the _nav shot
}
