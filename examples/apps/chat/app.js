// chat — benchmark app for the scroll + rebind + scroll-to-bottom combo:
// a message list (bindList over prebuilt left/right bubble pairs), an
// editable input bar (focusText/typeText), send -> deferred rebind ->
// re-scroll to bottom, a 1.2s auto reply, and long-press copy.
//
// Engine notes:
// - layout's in-flow test uses the AUTHORED visible flag; JS node.visible
//   only writes runtimeVisible, so a hidden auto-layout child still takes
//   flow space. Rows therefore stack both side cells HORIZONTALLY: the
//   hidden cell gets visible=false (pixels) AND empty texts (collapses to a
//   ~28px ghost = bubble padding), and row.primaryAlign min/max packs the
//   visible cell flush left (theirs) or right (mine).
// - Bubble TEXT is WIDTH_AND_HEIGHT (never wraps); wrap() pre-breaks long
//   messages so bubbles hug short texts and cap near 252px.
// - bindList invalidates node handles AND resets the scroll offset; every
//   render() re-pins scrollY = maxScrollY afterwards.

ui.selectFrame(APP.entryFrame || "Chat");

const MSGS = [
    { who: "them", text: "这周五有空吗，好久没聚了", time: "昨天 19:02" },
    { who: "me",   text: "有啊，正好那天不加班", time: "昨天 19:05" },
    { who: "them", text: "那约个饭？上次说的那家川菜一直没去成", time: "昨天 19:05" },
    { who: "me",   text: "可以啊，就是他家周五人多，得排队", time: "昨天 19:07" },
    { who: "them", text: "我提前订位就行，六点半怎么样", time: "昨天 19:08" },
    { who: "me",   text: "六点半赶不上，下午的需求评审老板改到四点了", time: "昨天 19:10" },
    { who: "them", text: "又改时间，你们那个项目真是一言难尽", time: "昨天 19:11" },
    { who: "me",   text: "别提了，那就七点吧，开完正好过去", time: "昨天 19:12" },
    { who: "them", text: "行，七点，位子我来订。老周叫不叫", time: "昨天 19:15" },
    { who: "me",   text: "叫啊，他上次还说欠我们一顿", time: "昨天 19:16" },
    { who: "them", text: "哈哈那正好让他把单结了", time: "昨天 19:17" },
    { who: "them", text: "订好了，周五晚上七点，川西坝子老店见", time: "今天 09:32" },
];

const REPLIES = [
    "好嘞，不见不散",
    "收到，我先到了就占座",
    "行，到时候路上说一声",
];
let replyAt = 0;

// simulated clipboard for long-press copy (bench asserts this variable)
let clipboard = "";

// Pre-wrap: bubble text is WIDTH_AND_HEIGHT (no engine wrap), so break at
// ~16 CJK units (ASCII counts half) => max line ~224px at 14px type.
function wrap(s, max = 16) {
    let out = "", u = 0;
    for (const ch of s) {
        const w = ch.codePointAt(0) < 256 ? 0.5 : 1;
        if (u + w > max) { out += "\n"; u = 0; }
        out += ch;
        u += w;
    }
    return out;
}

function nowTime() {
    const d = new Date();
    const mm = String(d.getMinutes()).padStart(2, "0");
    return `今天 ${d.getHours()}:${mm}`;
}

function scrollBottom() {
    const l = ui.find("MsgList");
    if (l) l.scrollY = l.maxScrollY;
}

function render() {
    ui.bindList("MsgList", MSGS.length, (row, i) => {
        const m = MSGS[i];
        const mine = m.who === "me";
        const side = mine ? "Mine" : "Their";
        const other = mine ? "Their" : "Mine";
        row.find(`${side}Cell`).visible = true;
        row.find(`${side}Text`).text = wrap(m.text);
        row.find(`${side}Time`).text = m.time;
        row.find(`${other}Cell`).visible = false;
        row.find(`${other}Text`).text = "";   // collapse the ghost's width
        row.find(`${other}Time`).text = "";
        row.primaryAlign = mine ? "max" : "min";
    });
    scrollBottom();   // rebind resets the offset; open-at-bottom is the rule
}
render();

// text layout settles over the first frames (font measurement); keep the
// list pinned to the bottom until it does.
let boot = 0;
ui.onUpdate(() => { if (boot < 3) { boot++; scrollBottom(); } });

// ---- input + send ----
ui.setEditable("InputText");
ui.onClick("InputBox", () => ui.focusText("InputText"));

ui.onUpdate(() => {
    const t = ui.find("InputText");
    ui.setVisible("InputHint", !(t && t.text));
});

function send() {
    const text = (ui.find("InputText").text || "").replace(/\n/g, " ").trim();
    if (!text) return;
    ui.blur();
    // structural rebind must run outside event dispatch
    setTimeout(() => {
        MSGS.push({ who: "me", text, time: nowTime() });
        ui.setText("InputText", "");
        render();
        // echo-style auto reply 1.2s later, then back to the bottom again
        const reply = REPLIES[replyAt++ % REPLIES.length];
        setTimeout(() => {
            MSGS.push({ who: "them", text: reply, time: nowTime() });
            render();
        }, 1200);
    }, 0);
}
ui.onClick("SendBtn", send);
ui.onHover("SendBtn", (n, entered) => ui.setOpacity(n, entered ? 0.8 : -1.0));

// ---- long-press any bubble -> simulated clipboard ----
// Rows have no fill (auto-layout wrappers), so listeners go on the bubbles,
// which do: bubble -> Their/MineCell -> MsgRow (.index = message index).
function copyBubble(node) {
    const i = node.parent.parent.index;
    if (i >= 0 && i < MSGS.length) {
        clipboard = MSGS[i].text;
        console.log(`chat: copied "${clipboard}"`);
    }
}
ui.onLongPress("TheirBubble", copyBubble);
ui.onLongPress("MineBubble", copyBubble);

console.log("chat ready");

// ---- unattended tour (figoplay --selfdrive) ----
// frame 20  -> assert 12 rows, opened AT the bottom (scrollY == maxScrollY)
// frame 30  -> <prefix>_home.png (list at the bottom, untouched)
// frame 32  -> one-tick drag downward: older messages scroll into view
// frame 36  -> assert scrollY shrank
// frame 38  -> focus input, typeText
// frame 40  -> assert typed, tap 发送 (deferred rebind next frame)
// frame 46  -> assert 13 rows, my bubble text, back AT the bottom, input clear
// frame 48  -> long-press row 13 bubble; frame 50 asserts the clipboard
// frame ~112-> the 1.2s reply lands (~72 vsync frames after the send)
// frame 110 -> <prefix>_nav.png (sent message, reply about to land)
// frame 122 -> assert 14 rows, reply text, bottom again
// frame 124 -> bench check lines + verdict; figoplay exits at 140
if (globalThis.SELFDRIVE) {
    const checks = [];
    const check = (what, got, want) => checks.push([what, String(got), String(want)]);
    const rows = () => ui.findAll("MsgRow");
    const list = () => ui.find("MsgList");
    const TYPED = "晚上七点见";
    let y0 = 0;
    let frames = 0;  // count frames, not dt: first dt includes file load

    ui.onUpdate(() => {
        frames++;

        if (frames === 20) {
            check("12 rows", rows().length, 12);
            check("scrollable", list().maxScrollY > 0, true);
            check("opened at bottom", list().scrollY, list().maxScrollY);
            check("first msg left", rows()[0].find("TheirCell").visible, true);
            check("last msg text", rows()[11].find("TheirText").text,
                  wrap(MSGS[11].text));
            y0 = list().scrollY;
        }

        if (frames === 32) {
            // synthesized drag must complete within one tick
            ui.pointerDown(210, 400);
            ui.pointerMove(210, 450);
            ui.pointerMove(210, 520);
            ui.pointerMove(210, 580);
            ui.pointerUp(210, 580);
        }
        if (frames === 36)
            check("drag revealed older msgs", list().scrollY < y0, true);

        if (frames === 38) {
            ui.focusText("InputText");
            ui.typeText(TYPED);
        }
        if (frames === 40) {
            check("typed into input", ui.find("InputText").text, TYPED);
            console.log("selfdrive tap send ->", ui.tap("SendBtn"));
        }
        if (frames === 46) {
            check("13 rows after send", rows().length, 13);
            check("new bubble is mine", rows()[12].find("MineCell").visible, true);
            check("new bubble text", rows()[12].find("MineText").text,
                  wrap(TYPED));
            check("auto-scrolled to bottom", list().scrollY, list().maxScrollY);
            check("input cleared", ui.find("InputText").text, "");
        }

        if (frames === 48)
            console.log("selfdrive long-press row 13 ->",
                        ui.longPress(rows()[12].find("MineBubble")));
        if (frames === 50)
            check("long-press copied", clipboard, TYPED);

        if (frames === 122) {
            check("14 rows after reply", rows().length, 14);
            check("reply is theirs", rows()[13].find("TheirCell").visible, true);
            check("reply text", rows()[13].find("TheirText").text,
                  wrap(REPLIES[0]));
            check("bottom after reply", list().scrollY, list().maxScrollY);
        }

        if (frames === 124) {
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
