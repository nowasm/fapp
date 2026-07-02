// recipes — benchmark app: chip-filtered bindList + favorite toggle inside a
// clickable row (event bubbling) + cross-frame detail page.
//
// Engine notes:
// - Click handlers bubble from the pressed node to its ancestors, so the
//   FavBtn handler fires BEFORE the RecipeRow handler on the same tap. A
//   one-shot flag lets the heart consume the tap without navigating.
// - bindList is illegal inside event dispatch: chip filtering and opening
//   the detail page (ingredient rebind) defer via setTimeout(0).
// - Fills aren't scriptable, so per-recipe tint = 5 pre-authored tint rects
//   per thumb, exactly one visible (same trick on the detail block).

ui.selectFrame(APP.entryFrame || "Recipes");

const CATS = ["全部", "快手", "主食", "汤"];
const TINT_COUNT = 5;

// {name, cat, mins, diff, serves, abbr, tint, ings: [[名, 量]], steps: []}
const RECIPES = [
    {
        name: "番茄炒蛋", cat: "快手", mins: 10, diff: "简单", serves: 2,
        abbr: "炒蛋", tint: 2,
        ings: [["番茄", "2 个"], ["鸡蛋", "3 个"], ["小葱", "1 根"],
               ["盐", "1 小勺"], ["白糖", "半勺"]],
        steps: [
            "1. 番茄切块，鸡蛋加少许盐打散。",
            "2. 热油把蛋液滑炒到刚凝固，盛出备用。",
            "3. 下番茄炒出汁，回锅鸡蛋，加糖调味，撒葱花出锅。",
        ],
    },
    {
        name: "宫保鸡丁", cat: "快手", mins: 25, diff: "中等", serves: 2,
        abbr: "宫保", tint: 1,
        ings: [["鸡腿肉", "300 克"], ["熟花生米", "50 克"], ["干辣椒", "8 个"],
               ["花椒", "1 小把"], ["葱白", "2 段"], ["料酒", "1 勺"]],
        steps: [
            "1. 鸡腿肉切丁，加料酒、生抽、淀粉抓匀腌 10 分钟。",
            "2. 调碗汁：糖、醋、生抽、淀粉加两勺水拌匀。",
            "3. 干辣椒和花椒小火炒香，下鸡丁滑炒到变色。",
            "4. 倒入碗汁收浓，加花生米和葱段翻匀出锅。",
        ],
    },
    {
        name: "番茄蛋汤", cat: "汤", mins: 15, diff: "简单", serves: 3,
        abbr: "蛋汤", tint: 0,
        ings: [["番茄", "1 个"], ["鸡蛋", "2 个"], ["小葱", "1 根"],
               ["香油", "几滴"], ["盐", "适量"]],
        steps: [
            "1. 番茄切薄片，入锅炒软出红汁。",
            "2. 加三碗水烧开，转小火。",
            "3. 转圈淋入蛋液冲成蛋花，加盐、撒葱花、点香油。",
        ],
    },
    {
        name: "扬州炒饭", cat: "主食", mins: 15, diff: "简单", serves: 2,
        abbr: "炒饭", tint: 3,
        ings: [["隔夜米饭", "2 碗"], ["鸡蛋", "2 个"], ["火腿", "50 克"],
               ["青豆", "30 克"], ["虾仁", "60 克"], ["葱花", "适量"]],
        steps: [
            "1. 鸡蛋炒散盛出，虾仁、火腿丁、青豆过油断生。",
            "2. 米饭下锅压散，中火炒到粒粒分开。",
            "3. 倒回配料翻匀，加盐和白胡椒，撒葱花出锅。",
        ],
    },
    {
        name: "葱油拌面", cat: "主食", mins: 12, diff: "简单", serves: 1,
        abbr: "拌面", tint: 4,
        ings: [["细面", "150 克"], ["小葱", "6 根"], ["生抽", "2 勺"],
               ["老抽", "半勺"], ["白糖", "1 勺"]],
        steps: [
            "1. 小葱切段，冷油下锅小火熬到焦黄捞出。",
            "2. 葱油里加生抽、老抽、白糖，小火烧到微沸。",
            "3. 面煮好沥干，浇上葱油酱汁拌匀，铺回葱段。",
        ],
    },
    {
        name: "蒜蓉油麦菜", cat: "快手", mins: 8, diff: "简单", serves: 2,
        abbr: "油麦", tint: 2,
        ings: [["油麦菜", "1 把"], ["大蒜", "4 瓣"], ["蚝油", "1 勺"],
               ["盐", "少许"]],
        steps: [
            "1. 油麦菜洗净切段，蒜切末分成两份。",
            "2. 一半蒜末炝锅，大火下菜快炒到断生。",
            "3. 加蚝油和盐，撒剩下的蒜末翻两下出锅。",
        ],
    },
    {
        name: "凉拌黄瓜", cat: "快手", mins: 5, diff: "简单", serves: 2,
        abbr: "黄瓜", tint: 3,
        ings: [["黄瓜", "2 根"], ["大蒜", "3 瓣"], ["香醋", "1 勺"],
               ["香油", "半勺"], ["盐", "少许"]],
        steps: [
            "1. 黄瓜拍裂切段，加盐腌 5 分钟挤掉水分。",
            "2. 蒜末、香醋、香油调成料汁。",
            "3. 料汁浇在黄瓜上拌匀，冷藏一会儿更脆。",
        ],
    },
    {
        name: "冬瓜排骨汤", cat: "汤", mins: 60, diff: "中等", serves: 4,
        abbr: "排骨", tint: 0,
        ings: [["排骨", "400 克"], ["冬瓜", "300 克"], ["姜", "3 片"],
               ["料酒", "1 勺"], ["盐", "适量"]],
        steps: [
            "1. 排骨冷水下锅，加料酒焯出浮沫后冲净。",
            "2. 排骨加姜片和热水，小火炖 40 分钟。",
            "3. 下冬瓜块再炖 15 分钟到透明。",
            "4. 加盐调味，撒一点白胡椒即可。",
        ],
    },
];

const STORE_KEY = "recipes.favs.v1";

// Bench runs must be idempotent: wipe persisted favorites before loading.
if (globalThis.SELFDRIVE) localStorage.removeItem(STORE_KEY);

let favs = new Set();
try {
    const saved = JSON.parse(localStorage.getItem(STORE_KEY) || "null");
    if (Array.isArray(saved))
        favs = new Set(saved.filter(i => i >= 0 && i < RECIPES.length));
} catch (e) { /* corrupt store -> start fresh */ }

function save() {
    localStorage.setItem(STORE_KEY, JSON.stringify([...favs].sort((a, b) => a - b)));
}

let cat = 0;          // selected chip index into CATS
let visible = [];     // recipe indices currently bound to the list
let detailIdx = -1;   // recipe shown on the Recipe page
let favTapped = false;  // FavBtn consumed this tap; RecipeRow must not navigate

function showTint(node, prefix, tint) {
    for (let t = 0; t < TINT_COUNT; t++)
        node.find(prefix + t).visible = (t === tint);
}

function applyRow(row, ri) {
    const r = RECIPES[ri];
    const thumb = row.find("RecipeThumb");
    showTint(thumb, "Tint", r.tint);
    thumb.find("ThumbAbbr").text = r.abbr;
    row.find("RecipeName").text = r.name;
    row.find("RecipeMeta").text = `${r.mins} 分钟 · ${r.diff}`;
    row.find("FavOn").visible = favs.has(ri);
    row.find("FavOff").visible = !favs.has(ri);
}

function applyChips() {
    ui.findAll("CatChip").forEach((chip, i) => {
        chip.find("ChipOn").visible = (i === cat);
        chip.find("ChipOff").visible = (i !== cat);
    });
}

function applyCount() {
    ui.setText("FavCount", `已收藏 ${favs.size}`);
}

// Full render: rebind the filtered list (re-entrant for hot reload).
function render() {
    visible = RECIPES.map((r, i) => i)
        .filter(i => cat === 0 || RECIPES[i].cat === CATS[cat]);
    ui.bindList("RecipeList", visible.length, (item, i) => applyRow(item, visible[i]));
    applyChips();
    applyCount();
}
render();

function applyDetailFav() {
    if (detailIdx < 0) return;
    ui.setVisible("DetailFavOn", favs.has(detailIdx));
    ui.setVisible("DetailFavOff", !favs.has(detailIdx));
}

function openRecipe(ri) {
    detailIdx = ri;
    const r = RECIPES[ri];
    const thumb = ui.find("DetailThumb");
    showTint(thumb, "DetailTint", r.tint);
    thumb.find("DetailAbbr").text = r.abbr;
    ui.setText("DetailName", r.name);
    ui.setText("DetailMeta", `${r.mins} 分钟 · ${r.diff} · ${r.serves} 人份`);
    ui.bindList("IngredientList", r.ings.length, (item, i) => {
        item.find("IngName").text = r.ings[i][0];
        item.find("IngAmt").text = r.ings[i][1];
    });
    for (let s = 0; s < 4; s++) {
        const has = s < r.steps.length;
        ui.setText(`Step ${s + 1}`, has ? r.steps[s] : "");
        ui.setVisible(`Step ${s + 1}`, has);
    }
    applyDetailFav();
    ui.navigateTo("Recipe", "slideLeft", 0.3);
}

// ---- interactions ----
ui.onClick("CatChip", (node) => {
    const i = node.index;
    if (i === cat) return;
    cat = i;
    setTimeout(render, 0);  // bindList must run outside event dispatch
});

// Heart inside the row: fires first (bubbling walks node -> ancestors), sets
// the one-shot flag so the RecipeRow handler below skips navigation.
ui.onClick("FavBtn", (node) => {
    favTapped = true;
    const row = node.parent;              // FavBtn is a direct child of the row
    const ri = visible[row.index];
    if (ri === undefined) return;
    if (favs.has(ri)) favs.delete(ri); else favs.add(ri);
    save();
    applyRow(row, ri);                    // in place, non-structural
    applyCount();
    if (detailIdx === ri) applyDetailFav();
});

ui.onClick("RecipeRow", (node) => {
    if (favTapped) { favTapped = false; return; }  // heart consumed this tap
    const ri = visible[node.index];
    if (ri === undefined) return;
    setTimeout(() => openRecipe(ri), 0);  // ingredient bindList -> defer
});

ui.onClick("DetailFav", () => {
    if (detailIdx < 0) return;
    const ri = detailIdx;
    if (favs.has(ri)) favs.delete(ri); else favs.add(ri);
    save();
    applyDetailFav();
    applyCount();
    const pos = visible.indexOf(ri);      // sync the list heart in place
    if (pos >= 0) applyRow(ui.findAll("RecipeRow")[pos], ri);
});

ui.onClick("Back Button", () => ui.navigateBack(0.2));

// press feedback (opacity only; -1 restores the authored value)
for (const n of ["RecipeRow", "CatChip", "Back Button", "DetailFav"])
    ui.onHover(n, (node, entered) => ui.setOpacity(node, entered ? 0.72 : -1.0));

console.log("recipes ready");

// ---- unattended tour (figoplay --selfdrive) ----
// figoplay shoots frame 30 -> <prefix>_home.png and 110 -> <prefix>_nav.png,
// exits at 140. Tour: assert 8 rows -> filter 汤 (2 rows) -> back to 全部 ->
// heart on row 2 (must NOT navigate) -> open row 3 -> assert detail ->
// back -> assert list. Verdict prints at 126.
if (globalThis.SELFDRIVE) {
    let frames = 0;  // count frames, not dt: first dt includes file load
    const checks = [];
    const check = (what, got, want) => checks.push([what, String(got), String(want)]);
    const rows = () => ui.findAll("RecipeRow");
    ui.onUpdate(() => {
        frames++;
        if (frames === 32) {
            check("list rows (全部)", rows().length, 8);
            check("first row name", rows()[0].find("RecipeName").text,
                  RECIPES[0].name);
            check("fav count start", ui.find("FavCount").text, "已收藏 0");
        }
        if (frames === 36)
            console.log("selfdrive tap chip 汤 ->", ui.tap(ui.findAll("CatChip")[3]));
        if (frames === 44) {
            check("filtered rows (汤)", rows().length, 2);
            check("filtered first name", rows()[0].find("RecipeName").text,
                  "番茄蛋汤");
        }
        if (frames === 48)
            console.log("selfdrive tap chip 全部 ->", ui.tap(ui.findAll("CatChip")[0]));
        if (frames === 56)
            check("rows after 全部", rows().length, 8);
        if (frames === 60)
            console.log("selfdrive tap row2 heart ->",
                        ui.tap(rows()[1].find("FavBtn")));
        if (frames === 66) {
            check("heart: fav count", ui.find("FavCount").text, "已收藏 1");
            check("heart: no navigation", ui.currentFrame().name, "Recipes");
            check("heart: row2 filled", rows()[1].find("FavOn").visible, true);
            check("heart: persisted", localStorage.getItem(STORE_KEY), "[1]");
        }
        if (frames === 70)
            console.log("selfdrive tap row3 ->", ui.tap(rows()[2]));
        if (frames === 95) {
            check("nav settled", ui.transitionProgress() >= 1, true);
            check("nav to detail", ui.currentFrame().name, "Recipe");
            check("detail name", ui.find("DetailName").text, RECIPES[2].name);
            check("ingredient rows", ui.findAll("IngredientRow").length,
                  RECIPES[2].ings.length);
            check("step 1 text", ui.find("Step 1").text, RECIPES[2].steps[0]);
            check("detail not faved", ui.find("DetailFavOff").visible, true);
        }
        if (frames === 112)
            console.log("selfdrive tap back ->", ui.tap("Back Button"));
        if (frames === 126) {
            check("back to list", ui.currentFrame().name, "Recipes");
            check("fav survives tour", ui.find("FavCount").text, "已收藏 1");
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
