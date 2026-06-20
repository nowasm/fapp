// wallet.js — the demo_wallet app expressed as pure script.
// Run with: figoplay wallet.fig wallet.js   (see figo/script.h for the API)

const portfolio = [
    { symbol: "ETH", change: "+ 2.56", usd: "$4.240,50", amount: "25 ETH", rate: "$420,50" },
    { symbol: "BTC", change: "- 1.20", usd: "$2.890,00", amount: "0.10 BTC", rate: "$28.900,00" },
    { symbol: "BNB", change: "+ 0.88", usd: "$1.024,37", amount: "3.2 BNB", rate: "$324,37" },
    { symbol: "XRP", change: "- 0.45", usd: "$830,90", amount: "1.800 XRP", rate: "$0,46" },
    { symbol: "ADA", change: "+ 1.15", usd: "$640,12", amount: "1.700 ADA", rate: "$0,38" },
    { symbol: "DOGE", change: "+ 5.02", usd: "$420,69", amount: "6.000 DOGE", rate: "$0,07" },
    { symbol: "SOL", change: "- 2.31", usd: "$386,40", amount: "4.2 SOL", rate: "$92,00" },
];

function firstText(n) {
    if (!n) return null;
    if (n.type === "Text") return n;
    for (let i = 0; i < n.childCount; ++i) {
        const r = firstText(n.child(i));
        if (r) return r;
    }
    return null;
}

ui.setResizeMode("reflow");
ui.selectFrame("Home");

// Design fixups (wallet.fig quirks): pin every bottom nav bar (the Home
// screens forgot the fixed-when-scrolling flag) and let the Portfolio
// section grow downward with live data.
ui.findAll("Bottom Nav Bar").forEach((n) => (n.scrollFixed = true));
const pf = ui.find("Portfolio");
if (pf) {
    const list = pf.find("List");
    if (list) list.name = "portfolio-list";  // scope the binding (Trending has a List too)
    pf.primarySizing = "hug";
    pf.primaryAlign = "min";
}

// ---- data: the portfolio list ----
ui.bindList("portfolio-list", portfolio.length, (item, i) => {
    const c = portfolio[i];
    const heading = item.find("Heading");
    if (heading && heading.childCount >= 2) {
        heading.child(0).text = c.symbol;
        heading.child(1).text = c.change;
    }
    const balance = item.find("Balance");
    if (balance && balance.childCount >= 2) {
        balance.child(0).text = c.usd;
        balance.child(1).text = c.amount;
    }
});

// ---- behavior: tap a coin row -> Coin Info ----
function openCoin(c) {
    console.log("open coin", c.symbol);
    ui.navigateTo("Coin Info", "slideLeft", 0.28);
    const conv = ui.currentFrame().find("Conversion Value");
    if (conv) {
        const unit = firstText(conv);
        if (unit) unit.text = "1 " + c.symbol;
        let price = null;  // the price is the last direct text child
        for (let i = 0; i < conv.childCount; ++i) {
            if (conv.child(i).type === "Text") price = conv.child(i);
        }
        if (price) price.text = c.rate;
    }
}
ui.onClick("Card", (node) => {
    if (!node.parent || node.parent.name !== "portfolio-list") return;  // hero card etc.
    if (node.index >= 0 && node.index < portfolio.length) openCoin(portfolio[node.index]);
});

// ---- behavior: bottom navigation ----
ui.onClick("Discover", () => ui.navigateTo("Discover"));
ui.onClick("Trade", () => ui.navigateTo("Marketplace"));
ui.onClick("Account", () => ui.navigateTo("Profile"));
ui.onClick("Wallet", () => {  // center button = home
    while (ui.canGoBack()) ui.navigateBack(0);
    ui.navigateTo("Home", "dissolve", 0.2);
});

// ---- behavior: the greeting is a text field ----
const greeting = firstText(ui.find("Hero"));
if (greeting) {
    greeting.name = "greeting";
    ui.setEditable("greeting");
}

console.log("wallet.js ready —", ui.frameNames().length, "frames");

// ---- unattended tour (figoplay --selfdrive) ----
if (globalThis.SELFDRIVE) {
    let frames = 0;  // count frames, not dt: the first dt includes file loading
    ui.onUpdate(() => {
        if (++frames !== 60) return;
        const list = ui.find("portfolio-list");
        const row = list && list.childCount > 1 ? list.child(1) : null;
        console.log("selfdrive tap ->", ui.tap(row || "Card"));
    });

    // Timer + fetch smoke tests (results show up in the selfdrive log).
    let ticks = 0;
    const iv = setInterval(() => {
        if (++ticks === 3) {
            clearInterval(iv);
            console.log("interval ticked 3x, then cleared");
        }
    }, 100);
    setTimeout(() => console.log("timeout fired at ~250ms"), 250);
    fetch("https://example.com")
        .then((r) => console.log("fetch:", r.status, "ok =", r.ok,
                                 "body bytes =", r.text().length))
        .catch((e) => console.log("fetch error:", String(e)));
}
