// expense — benchmark app: ledger with month total + per-category summary +
// bindList transactions, and an Add page (slideUp) with a numeric keypad,
// category pills and a rotating note pool. Data persists in localStorage.
//
// Engine notes:
// - bindList is illegal inside event handlers (frees nodes mid-dispatch), so
//   Save mutates data + rebinds via setTimeout(0) (todo pattern).
// - JS can't set colors: expense/income amounts are dual TEXT (danger/success)
//   toggled with .visible; category dots ship all three colors per row and
//   one is shown; pill selection is an On/Off frame pair per pill.
// - No date picker (G3): new records are always stamped "today".

ui.selectFrame(APP.entryFrame || "Ledger");

const STORE_KEY = "expense.state.v1";
const CATS = ["餐饮", "出行", "日用"];
const NOTES = ["楼下便利店", "打车", "午后咖啡", "地铁充值", "外卖晚餐", "纸巾补货"];

// Bench runs must be idempotent: wipe persisted state before loading it.
if (globalThis.SELFDRIVE) localStorage.removeItem(STORE_KEY);

// amounts are numbers: expense < 0, income > 0 (two decimals, no fake precision)
const INITIAL = [
    { cat: 1, note: "打车回家", date: "7月2日", amt: -28.60 },
    { cat: 2, note: "退货退款", date: "7月2日", amt: 25.00 },
    { cat: 0, note: "午餐轻食", date: "7月2日", amt: -38.00 },
    { cat: 2, note: "洗衣液补货", date: "7月1日", amt: -32.90 },
    { cat: 1, note: "地铁通勤", date: "7月1日", amt: -6.00 },
    { cat: 0, note: "早餐豆浆油条", date: "7月1日", amt: -12.50 },
];

let state = { txs: INITIAL.map(t => ({ ...t })) };
try {
    const saved = JSON.parse(localStorage.getItem(STORE_KEY) || "null");
    if (saved && Array.isArray(saved.txs)) state = saved;
} catch (e) { /* corrupt store -> start fresh */ }

function save() { localStorage.setItem(STORE_KEY, JSON.stringify(state)); }

// ---- Add page form state (runtime only) ----
let entry = "0";     // amount being typed
let selCat = 0;      // selected category pill
let noteIdx = 0;     // rotating note pool cursor

const fmt = (v) => v.toFixed(2);
const today = () => { const d = new Date(); return `${d.getMonth() + 1}月${d.getDate()}日`; };

// Cross-page writes go through ui.find (falls back to the whole document).
function setTextAnywhere(name, s) { const n = ui.find(name); if (n) n.text = s; }
function setVisibleAnywhere(name, b) { const n = ui.find(name); if (n) n.visible = b; }

// ---- Ledger rendering ----
function renderSummary() {
    let total = 0;
    const cats = [0, 0, 0];
    for (const t of state.txs) {
        if (t.amt < 0) { total += -t.amt; cats[t.cat] += -t.amt; }
    }
    setTextAnywhere("Total Amount", `¥${fmt(total)}`);
    setTextAnywhere("Count Label", `共 ${state.txs.length} 笔`);
    for (let i = 0; i < 3; i++)
        setTextAnywhere(`Cat Sum ${i + 1}`, `¥${fmt(cats[i])}`);
}

function renderList() {
    ui.bindList("Tx List", state.txs.length, (item, i) => {
        const t = state.txs[i];
        item.find("Row Note").text = t.note;
        item.find("Row Date").text = t.date;
        for (let j = 0; j < 3; j++)
            item.find(`Row Dot ${j + 1}`).visible = j === t.cat;
        item.find("Amt Exp").visible = t.amt < 0;
        item.find("Amt Inc").visible = t.amt >= 0;
        if (t.amt < 0) item.find("Amt Exp").text = `-${fmt(-t.amt)}`;
        else item.find("Amt Inc").text = `+${fmt(t.amt)}`;
    });
}

function render() { renderSummary(); renderList(); }
render();

// ---- Add page rendering ----
const PILLS = ["Pill Food", "Pill Transit", "Pill Daily"];

function renderForm() {
    setTextAnywhere("Amount Display", entry);
    setTextAnywhere("Note Value", NOTES[noteIdx]);
    for (let i = 0; i < 3; i++) {
        setVisibleAnywhere(`${PILLS[i]} On`, i === selCat);
        setVisibleAnywhere(`${PILLS[i]} Off`, i !== selCat);
    }
}
renderForm();

// ---- interactions: Ledger ----
ui.onClick("Fab", () => {
    entry = "0";
    selCat = 0;
    renderForm();
    ui.navigateTo("Add", "slideUp", 0.3);
});

// ---- interactions: Add ----
function inputDigit(d) {
    if (d === ".") { if (!entry.includes(".")) entry += "."; }
    else if (entry === "0") entry = d;
    else {
        const [int, dec] = entry.split(".");
        if (dec !== undefined) { if (dec.length < 2) entry += d; }
        else if (int.length < 6) entry += d;
    }
    setTextAnywhere("Amount Display", entry);
}

for (let d = 0; d <= 9; d++) ui.onClick(`Key${d}`, () => inputDigit(String(d)));
ui.onClick("KeyDot", () => inputDigit("."));
ui.onClick("KeyDel", () => {
    entry = entry.length > 1 ? entry.slice(0, -1) : "0";
    setTextAnywhere("Amount Display", entry);
});

for (let i = 0; i < 3; i++)
    ui.onClick(PILLS[i], () => { selCat = i; renderForm(); });

ui.onClick("Note Row", () => {
    noteIdx = (noteIdx + 1) % NOTES.length;
    setTextAnywhere("Note Value", NOTES[noteIdx]);
});

ui.onClick("Close Button", () => ui.navigateBack(0.3));

ui.onClick("Save Button", () => {
    const v = parseFloat(entry);
    if (!isFinite(v) || v <= 0) return;  // nothing typed -> ignore
    const tx = { cat: selCat, note: NOTES[noteIdx], date: today(),
                 amt: -Math.round(v * 100) / 100 };
    noteIdx = (noteIdx + 1) % NOTES.length;  // next entry gets a fresh note
    setTimeout(() => {  // bindList must run outside event dispatch
        state.txs.unshift(tx);
        save();
        render();
        ui.navigateBack(0.3);
    }, 0);
});

// press feedback (opacity only; -1 restores the authored value)
const HOVERABLE = ["Fab", "Save Button", "Close Button", "Note Row", "Tx Row",
                   "KeyDot", "KeyDel"];
for (let d = 0; d <= 9; d++) HOVERABLE.push(`Key${d}`);
for (const p of PILLS) HOVERABLE.push(p);
for (const n of HOVERABLE)
    ui.onHover(n, (node, entered) => ui.setOpacity(node, entered ? 0.82 : -1.0));

console.log("expense ready");

// ---- unattended tour (figoplay --selfdrive) ----
// figoplay shoots frame 30 -> <prefix>_home.png (fresh ledger) and frame 110
// -> <prefix>_nav.png (ledger after the new record), exits at 140.
// Tour: assert initial totals -> tap "+" -> gate on transitionProgress -> type
// 4 2 . 5 0 -> cycle note -> pick 出行 -> Save -> gate the way back -> assert
// the new row, month total and category sums all moved by 42.50.
if (globalThis.SELFDRIVE) {
    const checks = [];
    const check = (what, got, want) => checks.push([what, String(got), String(want)]);
    const rows = () => ui.findAll("Tx Row");
    let frames = 0;  // count frames, not dt: first dt includes file load
    let addAt = 0;   // frame when the Add page finished sliding up
    let backAt = 0;  // frame when the Ledger finished sliding back
    let printed = false;

    function verdict() {
        if (printed) return;
        printed = true;
        check("sequence completed", backAt > 0, true);
        const fails = checks.filter(([, got, want]) => got !== want);
        for (const [what, got, want] of checks)
            console.log(`bench check ${what}: got=${got} want=${want}`);
        console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
    }

    ui.onUpdate(() => {
        frames++;

        if (frames === 34) {
            check("initial rows", rows().length, 6);
            check("initial total", ui.find("Total Amount").text, "¥118.00");
            check("initial count", ui.find("Count Label").text, "共 6 笔");
            check("initial cat 餐饮", ui.find("Cat Sum 1").text, "¥50.50");
            check("initial cat 出行", ui.find("Cat Sum 2").text, "¥34.60");
            check("initial cat 日用", ui.find("Cat Sum 3").text, "¥32.90");
            check("income row green", rows()[1].find("Amt Inc").visible, true);
        }
        if (frames === 36) console.log("selfdrive tap fab ->", ui.tap("Fab"));

        // gate: the slideUp transition must fully settle before typing
        if (frames > 36 && !addAt && ui.transitionProgress() >= 1
            && ui.currentFrame().name === "Add") {
            addAt = frames;
            console.log("selfdrive add page ready at frame", frames);
        }
        if (addAt) {
            const k = frames - addAt;
            if (k === 2) ui.tap("Key4");
            if (k === 4) ui.tap("Key2");
            if (k === 6) ui.tap("KeyDot");
            if (k === 8) ui.tap("Key5");
            if (k === 10) ui.tap("Key0");
            if (k === 12)
                check("typed amount", ui.find("Amount Display").text, "42.50");
            if (k === 14) ui.tap("Note Row");  // 楼下便利店 -> 打车
            if (k === 16) ui.tap("Pill Transit");
            if (k === 18) {
                check("pill 出行 on", ui.find("Pill Transit On").visible, true);
                check("pill 餐饮 off", ui.find("Pill Food On").visible, false);
                check("note cycled", ui.find("Note Value").text, "打车");
            }
            if (k === 20) console.log("selfdrive tap save ->", ui.tap("Save Button"));

            // gate: back on the Ledger with the return transition settled
            if (k > 22 && !backAt && ui.transitionProgress() >= 1
                && ui.currentFrame().name === "Ledger") {
                backAt = frames;
                console.log("selfdrive back on ledger at frame", frames);
            }
        }
        if (backAt && frames === backAt + 2) {
            check("back on ledger", ui.currentFrame().name, "Ledger");
            check("rows after save", rows().length, 7);
            check("new row note", rows()[0].find("Row Note").text, "打车");
            check("new row amount", rows()[0].find("Amt Exp").text, "-42.50");
            check("new row is expense", rows()[0].find("Amt Exp").visible, true);
            check("new row date", rows()[0].find("Row Date").text, today());
            check("total moved", ui.find("Total Amount").text, "¥160.50");
            check("cat 出行 moved", ui.find("Cat Sum 2").text, "¥77.10");
            check("cat 餐饮 unchanged", ui.find("Cat Sum 1").text, "¥50.50");
            check("count moved", ui.find("Count Label").text, "共 7 笔");
            check("persisted", localStorage.getItem(STORE_KEY),
                  JSON.stringify(state));
        }

        // verdict prints once everything landed, and no later than frame 124
        if ((backAt && frames >= backAt + 4 && frames >= 118) || frames === 124)
            verdict();
    });
}
