// _events_regress — benchmark app for the G5 event surface:
// onLongPress / onSwipe / onScroll, node.scrollX/scrollY(/max*) and event
// coordinates on onClick. Layout (420x900 viewport, scale 1):
//   List  : x 24..396, y 120..720, vertical scroll, maxScrollY = 352
//   Row2  : on-screen center (210, 316) at scroll 0
//   Card  : on-screen center (210, 820), not scrollable
// Synthetic gestures follow the repo rule: a multi-move pointer gesture
// (pointerDown/Move/Up) completes within ONE tick.

ui.selectFrame(APP.entryFrame || "Main");

// ---- observed events (re-entrant on hot reload) ----
let scrollEvents = [];   // [name, x, y]
let swipes = [];         // [name, direction]
let longPresses = [];    // [name, x, y]
let clicks = [];         // [name, x, y]

ui.onScroll("List", (node, x, y) => scrollEvents.push([node.name, x, y]));
ui.onSwipe("Row2", (node, dir) => swipes.push([node.name, dir]));
ui.onSwipe("Card", (node, dir) => swipes.push([node.name, dir]));
ui.onLongPress("Card", (node, x, y) => longPresses.push([node.name, x, y]));
ui.onClick("Card", (node, x, y) => clicks.push([node.name, x, y]));
ui.onClick("Row2", (node, x, y) => clicks.push([node.name, x, y]));

console.log("events_regress ready");

// ---- unattended tour (figoplay --selfdrive) ----
if (globalThis.SELFDRIVE) {
    const checks = [];
    const near = (v, want, tol) => Math.abs(v - want) <= tol;
    const check = (what, got, want) => checks.push([what, String(got), String(want)]);
    const count = (arr, name) => arr.filter(([n]) => n === name).length;

    let frames = 0;  // count frames, not dt: first dt includes file load
    ui.onUpdate(() => {
        frames++;
        const list = ui.find("List");

        if (frames === 10) {
            check("maxScrollY", near(list.maxScrollY, 352, 2), "true");
            check("maxScrollX", list.maxScrollX, "0");
            check("scrollX", list.scrollX, "0");
        }
        if (frames === 15) check("setScroll ok", ui.setScroll("List", 0, 40), "true");
        if (frames === 18) {
            check("setScroll applied", list.scrollY, "40");
            const last = scrollEvents[scrollEvents.length - 1] || [];
            check("onScroll(setScroll)", `${last[0]},${last[2]}`, "List,40");
        }
        if (frames === 20) list.scrollY = 9999;  // property write clamps
        if (frames === 23) {
            check("scrollY write clamped", near(list.scrollY, 352, 2), "true");
            const last = scrollEvents[scrollEvents.length - 1] || [];
            check("onScroll(write)", near(last[2] || -1, 352, 2), "true");
        }
        if (frames === 26) list.scrollY = 0;

        if (frames === 35) {  // touch drag: one tick, down/moves/up
            ui.pointerDown(210, 500);
            ui.pointerMove(210, 470);
            ui.pointerMove(210, 430);
            ui.pointerMove(210, 400);
            ui.pointerUp(210, 400);
        }
        if (frames === 38) {
            check("drag scrolled", near(list.scrollY, 100, 40), "true");
            const last = scrollEvents[scrollEvents.length - 1] || [];
            check("onScroll(drag)", near(last[2] || -1, list.scrollY, 1), "true");
        }
        if (frames === 40) list.scrollY = 0;

        if (frames === 45) {  // horizontal flick on the vertical list => swipe
            ui.pointerDown(300, 316);
            ui.pointerMove(260, 318);
            ui.pointerMove(220, 319);
            ui.pointerMove(180, 320);
            ui.pointerUp(180, 320);
        }
        if (frames === 47) {
            const last = swipes[swipes.length - 1] || [];
            check("swipe left", `${last[0]},${last[1]}`, "Row2,left");
            check("swipe no row click", count(clicks, "Row2"), "0");
            check("swipe no vscroll", list.scrollY, "0");
        }
        if (frames === 50) {  // swipe right on a non-scrollable card
            ui.pointerDown(150, 820);
            ui.pointerMove(190, 821);
            ui.pointerMove(230, 822);
            ui.pointerMove(270, 822);
            ui.pointerUp(270, 822);
        }
        if (frames === 52) {
            const last = swipes[swipes.length - 1] || [];
            check("swipe right", `${last[0]},${last[1]}`, "Card,right");
            check("swipe no card click", count(clicks, "Card"), "0");
        }

        if (frames === 60) check("longPress ok", ui.longPress("Card"), "true");
        if (frames === 62) {
            check("longPress fired once", count(longPresses, "Card"), "1");
            check("longPress ate click", count(clicks, "Card"), "0");
            const lp = longPresses[0] || [];
            check("longPress coords",
                  near(lp[1] || 0, 210, 3) && near(lp[2] || 0, 820, 3), "true");
        }
        if (frames === 66) ui.tap("Card");  // a short press after a long press
        if (frames === 68) {
            check("click after longPress", count(clicks, "Card"), "1");
            const c = clicks.find(([n]) => n === "Card") || [];
            check("click coords",
                  near(c[1] || 0, 210, 3) && near(c[2] || 0, 820, 3), "true");
        }
        if (frames === 72) ui.tap("Row2");
        if (frames === 74) check("row click after swipe", count(clicks, "Row2"), "1");

        if (frames === 100) {
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "REPRO: FAIL" : "REPRO: PASS");
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
