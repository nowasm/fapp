// shop — benchmark app: bindList + navigation + stepper + cart badge.
// Product art is a per-product tint block toggled with setVisible (JS can't
// touch colors, so all five tints ship in the design and we show one).

ui.selectFrame(APP.entryFrame || "Shop");

const PRODUCTS = [
    { name: "Linen Throw Pillow", price: "$38",
      desc: "Washed linen cover with a hidden zip. Insert included." },
    { name: "Oak Side Table", price: "$149",
      desc: "Solid white oak with a soft matte finish. Wipes clean." },
    { name: "Ceramic Table Lamp", price: "$89",
      desc: "Hand glazed stoneware base with a cotton drum shade." },
    { name: "Wool Area Rug", price: "$219",
      desc: "Flat woven wool in a heathered weave. No pad needed." },
    { name: "Stoneware Vase Set", price: "$54",
      desc: "Three vases in staggered heights. Food safe glaze." },
];

// state (module-level: hot reload re-runs the script, resetting is fine)
let cartCount = 0;
let currentProduct = 0;
let qty = 1;

// NB: ui.setText/setVisible only resolve names inside the CURRENT frame,
// so cross-page writes (Shop badge while on Detail, Detail fields while on
// Shop) go through ui.find(...) node handles, which fall back to the document.
function setTextAnywhere(name, s) { const n = ui.find(name); if (n) n.text = s; }
function setVisibleAnywhere(name, b) { const n = ui.find(name); if (n) n.visible = b; }

function renderBadge() {
    setTextAnywhere("Cart Badge Count", String(cartCount));
    setVisibleAnywhere("Cart Badge", cartCount > 0);
}

function renderQty() {
    setTextAnywhere("Qty Value", String(qty));
}

function openProduct(i) {
    currentProduct = i;
    qty = 1;
    const p = PRODUCTS[i];
    setTextAnywhere("Detail Name", p.name);
    setTextAnywhere("Detail Price", p.price);
    setTextAnywhere("Detail Desc", p.desc);
    for (let j = 0; j < PRODUCTS.length; j++)
        setVisibleAnywhere(`Detail Tint ${j + 1}`, j === i);
    renderQty();
    ui.navigateTo("Detail", "slideLeft", 0.28);
}

ui.bindList("Product List", PRODUCTS.length, (item, i) => {
    const p = PRODUCTS[i];
    item.find("Item Name").text = p.name;
    item.find("Item Price").text = p.price;
    for (let j = 0; j < PRODUCTS.length; j++)
        item.find(`Item Tint ${j + 1}`).visible = j === i;
});

ui.onClick("Product Row", (node) => openProduct(node.index));
ui.onClick("Back Button", () => ui.navigateBack(0.28));
ui.onClick("Qty Minus", () => { if (qty > 1) { qty--; renderQty(); } });
ui.onClick("Qty Plus", () => { if (qty < 9) { qty++; renderQty(); } });
ui.onClick("Add To Cart", () => { cartCount += qty; renderBadge(); });

// press feedback (opacity only; -1 restores the authored value)
for (const n of ["Product Row", "Add To Cart", "Qty Minus", "Qty Plus",
                 "Back Button"]) {
    ui.onHover(n, (node, entered) => ui.setOpacity(node, entered ? 0.82 : -1.0));
}

renderBadge();
console.log("shop ready");

// ---- unattended tour (figoplay --selfdrive) ----
// frame 30  -> <prefix>_home.png  (product list, untouched)
// frame 35  -> tap 2nd row (Oak Side Table), slideLeft 0.28s
// frame 50  -> transition settled: assert Detail frame + product name
// frames 55/60 -> tap + twice (qty 3), assert at 65
// frame 70  -> Add to Cart, assert badge text/visibility at 75
// frame 110 -> <prefix>_nav.png  (detail page after add-to-cart)
// frame 112 -> back; assert Shop at 125, print verdict; figoplay exits at 140
if (globalThis.SELFDRIVE) {
    let frames = 0;  // count frames, not dt: first dt includes file load
    const checks = [];
    const check = (what, got, want) => checks.push([what, String(got), String(want)]);
    ui.onUpdate(() => {
        frames++;
        if (frames === 35) {
            const rows = ui.findAll("Product Row");
            console.log("selfdrive tap row 2 ->", ui.tap(rows[1]));
        }
        if (frames === 50) {
            check("nav to detail", ui.currentFrame().name, "Detail");
            check("detail name", ui.find("Detail Name").text, "Oak Side Table");
            check("detail price", ui.find("Detail Price").text, "$149");
        }
        if (frames === 55 || frames === 60) ui.tap("Qty Plus");
        if (frames === 65) check("qty stepper", ui.find("Qty Value").text, "3");
        if (frames === 70) ui.tap("Add To Cart");
        if (frames === 75) {
            // node.visible has no JS getter, so assert the badge via its text
            // (it is only shown when cartCount > 0, set by the same handler)
            check("badge count", ui.find("Cart Badge Count").text, "3");
            check("cart total", cartCount, "3");
        }
        if (frames === 112) ui.tap("Back Button");
        if (frames === 125) {
            check("back to shop", ui.currentFrame().name, "Shop");
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
