// news — benchmark app: bindList feed + reading page navigation.
// Data is a built-in sample wire of short tech briefs; an optional fetch of
// the HN front page replaces the list titles when it succeeds (not part of
// the bench contract, silent on failure).

ui.selectFrame(APP.entryFrame || "Feed");

const ARTICLES = [
    {
        tag: "AI", source: "Circuit Weekly", when: "26m ago",
        title: "Helios 2 adds on-device transcription in nine languages",
        body: [
            "Arc Systems released Helios 2 today, moving speech transcription fully on device for nine languages, including Japanese and Portuguese.",
            "The company says the new model runs at 1.9x real time on last year's phones, and that no audio leaves the handset, a point it repeated often in the briefing.",
            "Early testers report solid accuracy on meetings and lectures, with names and jargon still the weak spot. A custom vocabulary list is planned for the fall.",
        ],
    },
    {
        tag: "DEV TOOLS", source: "The Build Log", when: "1h ago",
        title: "Torch build cache cuts cold CI runs to under four minutes",
        body: [
            "Torch 1.4 ships a remote build cache that deduplicates compile steps across branches. In the team's own monorepo, cold CI runs dropped from 19 minutes to under 4.",
            "The cache keys on compiler flags and file hashes, so a toolchain bump invalidates everything at once. Torch says partial invalidation is the next milestone.",
            "Pricing stays per seat. Self hosting the cache requires the enterprise tier, which drew some grumbling in the launch thread.",
            "Support covers Linux and macOS runners today. Windows runners are in a private beta, with a waitlist open now.",
        ],
    },
    {
        tag: "HARDWARE", source: "Teardown Report", when: "2h ago",
        title: "Arden shows a repairable earbud with a swappable battery",
        body: [
            "Arden's new Slate buds open with a twist: the battery pops out like a watch cell, and replacements cost six dollars a pair.",
            "The trade is size. Each bud weighs 6.8 grams, about a gram more than rivals, and the charging case is noticeably chunkier.",
            "Battery swaps take under a minute with no tools. Arden rates the buds for five years of service and sells every internal part on its site.",
        ],
    },
    {
        tag: "SECURITY", source: "Wire Notes", when: "4h ago",
        title: "Patched Relay flaw let guests read private channel titles",
        body: [
            "Relay fixed a permissions bug that let guest accounts list the titles of private channels they were never invited to. Message contents were not exposed.",
            "The flaw sat in a sidebar search endpoint that skipped a membership check. It was reported through Relay's bounty program three weeks ago.",
            "Relay says fewer than 2 percent of workspaces had active guests during the window, and admins can pull an audit export to check for lookups.",
        ],
    },
    {
        tag: "STARTUPS", source: "Ledger Daily", when: "7h ago",
        title: "Fieldpay raises a Series A to keep card readers working offline",
        body: [
            "Fieldpay closed an 18 million dollar Series A led by Meridian Ventures to build payment terminals that queue transactions without a network connection.",
            "The pitch is markets and street vendors, where connectivity drops mid sale. Terminals sync and settle once they find a signal again.",
            "The company charges a flat monthly fee instead of taking a cut per transaction, which it credits for early traction in Southeast Asia.",
        ],
    },
    {
        tag: "APPS", source: "Circuit Weekly", when: "Yesterday",
        title: "Paper 4 review: a reading app grows into a research tool",
        body: [
            "Paper started as a read it later app. Version 4 adds highlights that sync into a queryable notebook, and it changes what the app is for.",
            "Highlights now carry their source paragraph with them, so quotes keep their context. Search spans everything you have ever saved.",
            "The redesign is quieter too: one typeface, fewer buttons, no badge counts. At eight dollars a month it earns a spot for heavy readers.",
        ],
    },
];

// masthead date (today, in the design's small-caps meta style)
const DAYS = ["SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY",
              "FRIDAY", "SATURDAY"];
const MONTHS = ["JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
                "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER",
                "DECEMBER"];
const now = new Date();
ui.setText("Feed Date",
           `${DAYS[now.getDay()]}, ${MONTHS[now.getMonth()]} ${now.getDate()}`);

function openArticle(i) {
    const a = ARTICLES[i];
    // cross-frame writes by name are supported (current frame first,
    // falling back to the whole document)
    ui.setText("Article Tag", a.tag);
    ui.setText("Article Title", a.title);
    ui.setText("Article Byline", `${a.source} · ${a.when}`);
    for (let p = 0; p < 4; p++) {
        const has = p < a.body.length;
        ui.setText(`Article Para ${p + 1}`, has ? a.body[p] : "");
        ui.setVisible(`Article Para ${p + 1}`, has);
    }
    ui.navigateTo("Article", "slideLeft", 0.3);
}

ui.bindList("Story List", ARTICLES.length, (item, i) => {
    const a = ARTICLES[i];
    item.find("Story Tag").text = a.tag;
    item.find("Story Title").text = a.title;
    item.find("Story Meta").text = `${a.source} · ${a.when}`;
});

ui.onClick("Story Row", (node) => openArticle(node.index));
ui.onClick("Back Button", () => ui.navigateBack(0.3));

// press feedback (opacity only; -1 restores the authored value)
for (const n of ["Story Row", "Back Button"])
    ui.onHover(n, (node, entered) => ui.setOpacity(node, entered ? 0.72 : -1.0));

console.log("news ready");

// optional live refresh: swap in real HN front-page titles when the network
// cooperates. Silent on failure and skipped in SELFDRIVE (not part of the
// bench PASS conditions).
if (!globalThis.SELFDRIVE) {
    fetch("https://hnrss.org/frontpage.jsonfeed")
        .then((r) => r.json())
        .then((feed) => {
            const items = (feed && feed.items) || [];
            if (!items.length) return;
            const rows = ui.findAll("Story Row");
            const count = Math.min(rows.length, items.length);
            for (let i = 0; i < count; i++) {
                ARTICLES[i].title = items[i].title;
                ARTICLES[i].source = "Hacker News";
                rows[i].find("Story Title").text = items[i].title;
                rows[i].find("Story Meta").text = "Hacker News · front page";
            }
            console.log(`news: replaced ${count} titles from hnrss.org`);
        })
        .catch(() => {});
}

// ---- unattended tour (figoplay --selfdrive) ----
// frame 30  -> <prefix>_home.png  (feed, untouched)
// frame 32  -> assert list row count + first row title
// frame 35  -> tap 2nd row (Torch), slideLeft 0.3s (~9 frames at capped dt)
// frame 60  -> transition settled (transitionProgress >= 1): assert Article
//              frame + tag/title/byline/body
// frame 68  -> back; assert Feed at 84
// frame 85  -> tap 4th row (Relay); assert swapped content at 105
// frame 110 -> <prefix>_nav.png  (article page, second story)
// frame 120 -> print bench check lines + verdict; figoplay exits at 140
if (globalThis.SELFDRIVE) {
    let frames = 0;  // count frames, not dt: first dt includes file load
    const checks = [];
    const check = (what, got, want) => checks.push([what, String(got), String(want)]);
    ui.onUpdate(() => {
        frames++;
        if (frames === 32) {
            const rows = ui.findAll("Story Row");
            check("feed row count", rows.length, ARTICLES.length);
            check("first row title", rows[0].find("Story Title").text,
                  ARTICLES[0].title);
        }
        if (frames === 35) {
            const rows = ui.findAll("Story Row");
            console.log("selfdrive tap row 2 ->", ui.tap(rows[1]));
        }
        if (frames === 60) {
            check("transition settled", ui.transitionProgress() >= 1, true);
            check("nav to article", ui.currentFrame().name, "Article");
            check("article tag", ui.find("Article Tag").text, ARTICLES[1].tag);
            check("article title", ui.find("Article Title").text,
                  ARTICLES[1].title);
            check("article para 1", ui.find("Article Para 1").text,
                  ARTICLES[1].body[0]);
        }
        if (frames === 68) ui.tap("Back Button");
        if (frames === 84)
            check("back to feed", ui.currentFrame().name, "Feed");
        if (frames === 85) {
            // 0.3s slide at vsync dt (1/60) needs ~19 frames: tap here so the
            // transition is settled well before the frame-110 _nav screenshot
            const rows = ui.findAll("Story Row");
            console.log("selfdrive tap row 4 ->", ui.tap(rows[3]));
        }
        if (frames === 105) {
            check("second visit settled", ui.transitionProgress() >= 1, true);
            check("second article", ui.find("Article Title").text,
                  ARTICLES[3].title);
            check("second tag", ui.find("Article Tag").text, ARTICLES[3].tag);
        }
        if (frames === 120) {
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
