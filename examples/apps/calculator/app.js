// calculator — benchmark app #1. Pure click grid + setText + expression eval.
// Keys are addressed by unique layer names (Key0..Key9, KeyAdd, ...).

ui.selectFrame(APP.entryFrame || "Calculator");

// ---- state (module-level; script is re-run on hot reload, so reset is fine) ----
let acc = null;        // accumulated value (left operand)
let op = null;         // pending operator: "+" | "-" | "*" | "/"
let entry = "0";       // digits being typed
let fresh = true;      // next digit starts a new entry
let lastEq = null;     // {op, rhs} for repeated "=" presses

const OPS = {
    "+": (a, b) => a + b,
    "-": (a, b) => a - b,
    "*": (a, b) => a * b,
    "/": (a, b) => b === 0 ? NaN : a / b,
};
const GLYPH = { "+": "+", "-": "-", "*": "×", "/": "÷" };

function fmt(v) {
    if (typeof v !== "number" || !isFinite(v)) return "Error";
    // trim float noise, cap display length
    let s = String(Math.round(v * 1e10) / 1e10);
    if (s.replace("-", "").replace(".", "").length > 12) s = v.toExponential(6);
    return s;
}

function render(exprText) {
    ui.setText("Result", entry);
    ui.setText("Expression", exprText !== undefined ? exprText
        : (op !== null ? `${fmt(acc)} ${GLYPH[op]}` : ""));
}

function inputDigit(d) {
    if (fresh) { entry = d === "." ? "0." : d; fresh = false; }
    else if (d === ".") { if (!entry.includes(".")) entry += "."; }
    else if (entry === "0") entry = d;
    else if (entry.replace("-", "").replace(".", "").length < 12) entry += d;
    lastEq = null;
    render();
}

function applyPending() {
    if (op !== null && !fresh) {
        acc = OPS[op](acc, parseFloat(entry));
        entry = fmt(acc);
    } else if (op === null) {
        acc = parseFloat(entry);
    }
}

function inputOp(o) {
    applyPending();
    op = o;
    fresh = true;
    lastEq = null;
    render();
}

function inputEq() {
    if (op !== null) {
        const rhs = fresh ? acc : parseFloat(entry);
        const expr = `${fmt(acc)} ${GLYPH[op]} ${fmt(rhs)} =`;
        lastEq = { op, rhs };
        entry = fmt(OPS[op](acc, rhs));
        acc = parseFloat(entry);
        op = null;
        fresh = true;
        render(expr);
    } else if (lastEq) {  // repeated "=" repeats the last operation
        const expr = `${entry} ${GLYPH[lastEq.op]} ${fmt(lastEq.rhs)} =`;
        entry = fmt(OPS[lastEq.op](parseFloat(entry), lastEq.rhs));
        acc = parseFloat(entry);
        render(expr);
    }
}

function clearAll() {
    acc = null; op = null; entry = "0"; fresh = true; lastEq = null;
    render("");
}

function toggleSign() {
    if (entry !== "0" && entry !== "Error") {
        entry = entry.startsWith("-") ? entry.slice(1) : "-" + entry;
        render();
    }
}

function percent() {
    entry = fmt(parseFloat(entry) / 100);
    fresh = false;
    render();
}

// ---- wire keys ----
for (let d = 0; d <= 9; d++) ui.onClick(`Key${d}`, () => inputDigit(String(d)));
ui.onClick("KeyDot", () => inputDigit("."));
ui.onClick("KeyAdd", () => inputOp("+"));
ui.onClick("KeySub", () => inputOp("-"));
ui.onClick("KeyMul", () => inputOp("*"));
ui.onClick("KeyDiv", () => inputOp("/"));
ui.onClick("KeyEq", inputEq);
ui.onClick("KeyClear", clearAll);
ui.onClick("KeySign", toggleSign);
ui.onClick("KeyPct", percent);

// press feedback
for (const n of ["Clear", "Sign", "Pct", "Div", "Mul", "Sub", "Add", "Eq", "Dot",
                 "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]) {
    ui.onHover(`Key${n}`, (node, entered) => ui.setOpacity(node, entered ? 0.85 : -1.0));
}

render("");
console.log("calculator ready");

// ---- unattended tour (figoplay --selfdrive): 12 + 7.5 = 19.5, then C ----
// shots: frame 30 -> <prefix>_home.png (mid-typing), frame 110 -> <prefix>_nav.png (result)
if (globalThis.SELFDRIVE) {
    const seq = [
        [10, "Key1"], [14, "Key2"], [18, "KeyAdd"], [22, "Key7"],
        [26, "KeyDot"], [28, "Key5"],
        // frame 30: home shot shows "12 +" and entry 7.5
        [60, "KeyEq"],          // 19.5
        // frame 110: nav shot shows the result
        [120, "KeyClear"],
    ];
    let frames = 0;  // count frames, not dt: first dt includes file load
    const checks = [];
    ui.onUpdate(() => {
        frames++;
        for (const [at, key] of seq) {
            if (frames === at) console.log(`selfdrive tap ${key} ->`, ui.tap(key));
        }
        if (frames === 61) checks.push(["12+7.5=", ui.find("Result").text, "19.5"]);
        if (frames === 125) {
            checks.push(["clear", ui.find("Result").text, "0"]);
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
