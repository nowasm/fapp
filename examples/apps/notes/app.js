// notes — benchmark app for the G1 text-editing surface in a real app shape:
// a note list (bindList + localStorage) and an editor page whose title and
// body are REAL editable TEXT nodes (setEditable/focusText/typeText/editKey).
//
// Engine notes:
// - Structural changes (save/delete rebind the list) must run outside event
//   dispatch: handlers mutate state and defer render() via setTimeout(0).
// - The editor reads back node.text on save — the engine's edit buffer is the
//   source of truth while editing, state only syncs on back/delete.
// - Char count refreshes every frame from the body node (covers typing,
//   paste, cut — anything the edit path does).

ui.selectFrame(APP.entryFrame || "Notes");

const STORE_KEY = "notes.v1";

const INITIAL = [
    { title: "周末采购清单",
      body: "牛奶两盒、鸡蛋一打\n青菜、番茄、豆腐\n洗衣液快用完了，记得买大瓶装",
      date: "6月28日" },
    { title: "周三产品例会",
      body: "Q3 目标：导出功能月底前上线\n设计稿周五评审，叫上小林\n下次例会改到上午十点",
      date: "7月1日" },
    { title: "《置身事内》摘录",
      body: "地方政府的行为逻辑，是理解中国经济的一把钥匙。\n第三章讲土地财政，值得重读。",
      date: "6月25日" },
];

// Bench runs must be idempotent: wipe persisted state before loading it.
if (globalThis.SELFDRIVE) localStorage.removeItem(STORE_KEY);

let state = { notes: INITIAL.map(n => ({ ...n })) };
try {
    const saved = JSON.parse(localStorage.getItem(STORE_KEY) || "null");
    if (saved && Array.isArray(saved.notes)) state = saved;
} catch (e) { /* corrupt store -> start fresh */ }

function save() {
    localStorage.setItem(STORE_KEY, JSON.stringify(state));
}

function today() {
    const d = new Date();
    return `${d.getMonth() + 1}月${d.getDate()}日`;
}

function preview(body) {
    const line = (body || "").split("\n")[0];
    return line.length > 20 ? line.slice(0, 20) + "…" : line;
}

function render() {
    ui.bindList("NoteList", state.notes.length, (row, i) => {
        const n = state.notes[i];
        row.find("NoteTitle").text = n.title;
        row.find("NotePreview").text = preview(n.body);
        row.find("NoteDate").text = n.date;
    });
    ui.setText("NoteCount", `${state.notes.length} 条笔记`);
}
render();

// ---- editor ----
// editing: index into state.notes, or -1 for a brand-new note.
let editing = -1;

ui.setEditable("EditorTitle");
ui.setEditable("EditorBody");

function openEditor(i) {
    editing = i;
    ui.setText("EditorTitle", i >= 0 ? state.notes[i].title : "");
    ui.setText("EditorBody", i >= 0 ? state.notes[i].body : "");
    ui.navigateTo("Editor", "slideLeft", 0.25);
}

// Auto-save on back: read the live edit buffers, sync state, rebind the list
// (deferred — bindList is illegal inside event dispatch).
function closeEditor(discard) {
    const title = (ui.find("EditorTitle").text || "").replace(/\n/g, " ").trim();
    const body = ui.find("EditorBody").text || "";
    const i = editing;
    ui.blur();
    setTimeout(() => {
        if (discard) {
            if (i >= 0) state.notes.splice(i, 1);
        } else if (i >= 0) {
            const n = state.notes[i];
            if (n.title !== title || n.body !== body) {
                n.title = title || "未命名";
                n.body = body;
                n.date = today();
            }
        } else if (title || body) {  // new note, only keep non-empty
            state.notes.unshift({ title: title || "未命名", body,
                                  date: today() });
        }
        save();
        render();
    }, 0);
    ui.navigateBack(0.25);
}

ui.onClick("NoteRow", (node) => {
    const i = node.index;
    if (i >= 0 && i < state.notes.length) openEditor(i);
});
ui.onClick("NewBtn", () => openEditor(-1));
ui.onClick("BackBtn", () => closeEditor(false));
ui.onClick("DeleteBtn", () => closeEditor(true));

// press feedback (opacity only; -1 restores the authored value)
for (const n of ["NoteRow", "NewBtn", "BackBtn", "DeleteBtn"])
    ui.onHover(n, (node, entered) => ui.setOpacity(node, entered ? 0.72 : -1.0));

// char count follows the live edit buffer every frame
ui.onUpdate(() => {
    const body = ui.find("EditorBody");
    if (body) ui.setText("CharCount", `${(body.text || "").length} 字`);
});

console.log("notes ready");

// ---- unattended tour (figoplay --selfdrive) ----
// frame 30  -> <prefix>_home.png (note list, 3 rows)
// frame 32  -> tap 新建, slideLeft 0.25s
// frame 52  -> settled: focus title, typeText
// frame 56+ -> focus body, type two lines (editKey enter), then
//              selectAll/copy/end/paste doubles the body
// frame 70  -> back = auto-save; frame 90 asserts the new list row
// frame 92  -> reopen the note; frame 108 asserts content restored
// frame 110 -> <prefix>_nav.png (editor with real typed content)
// frame 124 -> bench check lines + verdict; figoplay exits at 140
if (globalThis.SELFDRIVE) {
    const checks = [];
    const check = (what, got, want) => checks.push([what, String(got), String(want)]);
    const rows = () => ui.findAll("NoteRow");
    const LINE1 = "上午去爬山看红叶";
    const LINE2 = "下午整理相机里的照片";
    const TWO = `${LINE1}\n${LINE2}`;          // 20 chars
    const DOUBLED = TWO + TWO;                 // 40 chars after paste
    let frames = 0;  // count frames, not dt: first dt includes file load

    ui.onUpdate(() => {
        frames++;

        if (frames === 20) {
            check("list has 3 rows", rows().length, 3);
            check("row0 title", rows()[0].find("NoteTitle").text, INITIAL[0].title);
            check("row0 preview", rows()[0].find("NotePreview").text,
                  preview(INITIAL[0].body));
            check("count label", ui.find("NoteCount").text, "3 条笔记");
        }

        if (frames === 32) console.log("selfdrive tap new ->", ui.tap("NewBtn"));
        if (frames === 52) {
            check("nav settled", ui.transitionProgress() >= 1, true);
            check("on editor", ui.currentFrame().name, "Editor");
            check("editor starts empty", ui.find("EditorBody").text, "");
            ui.focusText("EditorTitle");
            ui.typeText("周末计划");
        }
        if (frames === 54) {
            check("typed title", ui.find("EditorTitle").text, "周末计划");
            ui.focusText("EditorBody");
            ui.typeText(LINE1);
            ui.editKey("enter");
            ui.typeText(LINE2);
        }
        if (frames === 58) {
            check("typed two lines", JSON.stringify(ui.find("EditorBody").text),
                  JSON.stringify(TWO));
            check("char count", ui.find("CharCount").text, `${TWO.length} 字`);
            ui.editKey("selectAll");
            check("copy returns body", ui.editKey("copy"), TWO);
            ui.editKey("end");    // collapse the selection to the end
            ui.editKey("paste");  // append a full second copy
        }
        if (frames === 62) {
            check("paste doubled body", JSON.stringify(ui.find("EditorBody").text),
                  JSON.stringify(DOUBLED));
        }
        if (frames === 64)  // char count needs one more frame after the paste
            check("char count doubled", ui.find("CharCount").text,
                  `${DOUBLED.length} 字`);

        if (frames === 70) console.log("selfdrive tap back ->", ui.tap("BackBtn"));
        if (frames === 90) {
            check("back settled", ui.transitionProgress() >= 1, true);
            check("back on list", ui.currentFrame().name, "Notes");
            check("list has 4 rows", rows().length, 4);
            check("new row title", rows()[0].find("NoteTitle").text, "周末计划");
            check("new row preview", rows()[0].find("NotePreview").text, LINE1);
            check("count label updated", ui.find("NoteCount").text, "4 条笔记");
            check("persisted", localStorage.getItem(STORE_KEY),
                  JSON.stringify(state));
        }

        if (frames === 91) {  // scribble over the editor so the restore
            ui.setText("EditorTitle", "×");  // checks can't pass vacuously
            ui.setText("EditorBody", "×");
        }
        if (frames === 92) console.log("selfdrive reopen ->", ui.tap(rows()[0]));
        if (frames === 108) {
            check("reopen settled", ui.transitionProgress() >= 1, true);
            check("reopen on editor", ui.currentFrame().name, "Editor");
            check("title restored", ui.find("EditorTitle").text, "周末计划");
            check("body restored", JSON.stringify(ui.find("EditorBody").text),
                  JSON.stringify(DOUBLED));
        }

        if (frames === 124) {
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
