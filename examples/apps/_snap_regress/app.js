// _snap_regress — benchmark app for the scroll-snapping foundation
// (node.snapToChildren / node.snapIndex, ui.onScrollEnd, ui.snapTo,
// ui.scrollBy quantization). Layout (420x900 viewport, scale 1):
//   Picker : x 60..360, y 120..456, vertical scroll, autoLayout vertical,
//            item 300x100, spacing 12 -> pitch 112. bindList(8): content 884,
//            maxScrollY 548, boundaries 0,112,224,336,448,548(,548,548)
//   Plain  : x 60..360, y 520..820, vertical scroll, NOT snapping
// The fling->snap path needs real release velocity, which a one-tick
// synthetic gesture can't produce — that path is covered by render_test.
// Synthetic gestures follow the repo rule: complete within ONE tick.

ui.selectFrame(APP.entryFrame || "Main");

let ends = [];  // [name, y, index] per onScrollEnd

function fillPicker(count) {
    ui.bindList("Picker", count, (item, i) => {
        item.find("ItemLabel").text = "Item " + i;
    });
    ui.find("Picker").snapToChildren = true;  // runtime flag, re-set after rebuilds
}
fillPicker(8);

ui.onScrollEnd("Picker", (n, x, y, idx) => ends.push(["Picker", y, idx]));
ui.onScrollEnd("Plain", (n, x, y, idx) => ends.push(["Plain", y, idx]));

console.log("snap_regress ready");

// ---- unattended tour (figoplay --selfdrive) ----
if (globalThis.SELFDRIVE) {
    const checks = [];
    const near = (v, want, tol) => Math.abs(v - want) <= tol;
    const check = (what, got, want) => checks.push([what, String(got), String(want)]);
    const endsFor = (name) => ends.filter(([n]) => n === name);

    let frames = 0;  // count frames, not dt: first dt includes file load
    ui.onUpdate(() => {
        frames++;
        const picker = ui.find("Picker");

        if (frames === 5) {
            check("maxScrollY", near(picker.maxScrollY, 548, 1), "true");
            check("snapToChildren set", picker.snapToChildren, "true");
            check("snapIndex initial", picker.snapIndex, "0");
            check("plain snapIndex", ui.find("Plain").snapIndex, "-1");
        }

        if (frames === 8) {  // slow drag (one tick = zero release velocity):
            ui.pointerDown(210, 420);  // travel 90 -> scroll 90 -> snaps to 112
            ui.pointerMove(210, 390);
            ui.pointerMove(210, 360);
            ui.pointerMove(210, 330);
            ui.pointerUp(210, 330);
        }
        if (frames === 40) {  // easing had ~0.5s to settle
            check("drag snapped", near(picker.scrollY, 112, 0.5), "true");
            check("drag snapIndex", picker.snapIndex, "1");
            check("drag end once", endsFor("Picker").length, "1");
            const e = endsFor("Picker")[0] || [];
            check("drag end payload", `${Math.round(e[1])},${e[2]}`, "112,1");
        }

        if (frames === 42) {  // wheel notch (40px < pitch 112) -> one item
            check("scrollBy ok", ui.scrollBy(210, 300, 0, 40), "true");
        }
        if (frames === 72) {
            check("wheel quantized", near(picker.scrollY, 224, 0.5), "true");
            check("wheel end once", endsFor("Picker").length, "2");
            check("wheel end index", (endsFor("Picker")[1] || [])[2], "2");
        }

        if (frames === 74) check("snapTo ok", ui.snapTo("Picker", 5, 0.2), "true");
        if (frames === 100) {
            check("snapTo eased", near(picker.scrollY, 548, 0.5), "true");
            check("snapTo end index", (endsFor("Picker")[2] || [])[2], "5");
        }

        if (frames === 102) {  // bindList rebuild: snap keeps working
            fillPicker(6);     // content 660, maxScrollY 324
            check("snapTo instant", ui.snapTo("Picker", 2, 0), "true");
        }
        if (frames === 105) {
            check("rebuilt maxScrollY", near(picker.maxScrollY, 324, 1), "true");
            check("instant snapTo applied", near(picker.scrollY, 224, 0.5), "true");
            check("instant end index", (endsFor("Picker")[3] || [])[2], "2");
            // wheel after the rebuild: 224 + notch -> boundary 3 (= 324 clamped)
            ui.scrollBy(210, 300, 0, 30);
        }
        if (frames === 132) {
            check("rebuilt wheel snap", near(picker.scrollY, 324, 0.5), "true");
            check("rebuilt end index", (endsFor("Picker")[4] || [])[2], "3");
            check("picker ends total", endsFor("Picker").length, "5");
        }

        if (frames === 108) ui.setScroll("Plain", 0, 55);  // instant set = an end
        if (frames === 112) {
            const e = endsFor("Plain")[0] || [];
            check("plain end once", endsFor("Plain").length, "1");
            check("plain end payload", `${e[1]},${e[2]}`, "55,-1");
        }

        if (frames === 135) {
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
