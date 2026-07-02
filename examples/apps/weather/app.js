// weather — benchmark app: bindList + setText + unit toggle + optional fetch.
// Sample data renders first (deterministic offline path); a live open-meteo
// fetch may overwrite the UI afterwards, but bench assertions only cover the
// sample path. Condition "icon" is an accent-tint badge with a text label.

ui.selectFrame(APP.entryFrame || "Weather");

const SAMPLE = {
    city: "Shanghai",
    temp: 26,             // Celsius, integers only (no fake precision)
    desc: "Partly cloudy",
    hi: 29, lo: 22,
    days: [
        { day: "Mon", cond: "Cloudy", hi: 29, lo: 22 },
        { day: "Tue", cond: "Sunny",  hi: 31, lo: 24 },
        { day: "Wed", cond: "Rain",   hi: 27, lo: 21 },
        { day: "Thu", cond: "Rain",   hi: 25, lo: 20 },
        { day: "Fri", cond: "Sunny",  hi: 28, lo: 21 },
    ],
    updated: "Updated: sample data",
};

// state (module-level: hot reload re-runs the script, resetting is fine)
let data = SAMPLE;
let useF = false;

function toUnit(c) { return useF ? Math.round(c * 9 / 5 + 32) : c; }

// WMO weather_code -> three text buckets (Sunny / Cloudy / Rain)
function condOf(code) {
    if (code <= 1) return "Sunny";
    if (code <= 48) return "Cloudy";
    return "Rain";
}

function descOf(code) {
    if (code === 0) return "Clear sky";
    if (code === 1) return "Mostly clear";
    if (code === 2) return "Partly cloudy";
    if (code === 3) return "Overcast";
    if (code <= 48) return "Fog";
    if (code <= 57) return "Drizzle";
    if (code <= 67) return "Rain";
    if (code <= 77) return "Snow";
    if (code <= 82) return "Rain showers";
    return "Thunderstorm";
}

function render() {
    ui.setText("City Name", data.city);
    ui.setText("Current Temp", toUnit(data.temp) + "°");
    ui.setText("Current Desc", data.desc);
    ui.setText("Today High", "H " + toUnit(data.hi) + "°");
    ui.setText("Today Low", "L " + toUnit(data.lo) + "°");
    ui.setText("Unit Label", useF ? "°C" : "°F");
    ui.setText("Updated Label", data.updated);
    ui.bindList("Forecast List", data.days.length, (item, i) => {
        const d = data.days[i];
        item.find("Row Day").text = d.day;
        item.find("Cond Label").text = d.cond;
        item.find("Row High").text = toUnit(d.hi) + "°";
        item.find("Row Low").text = toUnit(d.lo) + "°";
    });
}

// bindList must not run inside a click dispatch: hop out via setTimeout
ui.onClick("Unit Toggle", () => {
    useF = !useF;
    setTimeout(render, 0);
});
ui.onHover("Unit Toggle",
    (node, entered) => ui.setOpacity(node, entered ? 0.82 : -1.0));

render();
console.log("weather ready (sample data)");

// ---- live data: overwrite silently on success, keep sample on any failure.
// In SELFDRIVE the fetch is deferred until after the bench verdict so the
// asserted frames always show the deterministic sample rendering.
const API = "https://api.open-meteo.com/v1/forecast?latitude=31.23&longitude=121.47" +
    "&daily=temperature_2m_max,temperature_2m_min,weather_code" +
    "&current=temperature_2m,weather_code&timezone=auto";
const DAY_NAMES = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

function fetchLive() {
    fetch(API).then(r => r.json()).then(j => {
        const days = [];
        for (let i = 0; i < 5 && i < j.daily.time.length; i++) {
            const wd = new Date(j.daily.time[i] + "T12:00:00").getDay();
            days.push({
                day: DAY_NAMES[wd],
                cond: condOf(j.daily.weather_code[i]),
                hi: Math.round(j.daily.temperature_2m_max[i]),
                lo: Math.round(j.daily.temperature_2m_min[i]),
            });
        }
        data = {
            city: "Shanghai",
            temp: Math.round(j.current.temperature_2m),
            desc: descOf(j.current.weather_code),
            hi: days[0].hi, lo: days[0].lo,
            days,
            updated: "Updated: live",
        };
        console.log("weather live data:", data.temp + "°C,", data.desc);
        setTimeout(render, 0);
    }).catch(e => {
        console.log("weather fetch failed, keeping sample:", e);
    });
}

// ---- unattended tour (figoplay --selfdrive) ----
// frame 30  -> <prefix>_home.png  (sample data, Celsius)
// frame 40  -> tap Unit Toggle, assert Fahrenheit at 50
// frame 60  -> tap back to Celsius, assert at 70
// frame 90  -> tap to Fahrenheit again so the nav shot differs from home
// frame 110 -> <prefix>_nav.png
// frame 125 -> verdict; frame 126 -> kick off the live fetch (log only);
// figoplay exits at 140
if (globalThis.SELFDRIVE) {
    let frames = 0;  // count frames, not dt: first dt includes file load
    const checks = [];
    const check = (what, got, want) => checks.push([what, String(got), String(want)]);
    ui.onUpdate(() => {
        frames++;
        if (frames === 20) {
            check("bindList rows", ui.findAll("Forecast Row").length, "5");
            const rows = ui.findAll("Forecast Row");
            check("row 1 day", rows[0].find("Row Day").text, "Mon");
            check("row 2 cond", rows[1].find("Cond Label").text, "Sunny");
            check("row 5 high", rows[4].find("Row High").text, "28°");
            check("current temp", ui.find("Current Temp").text, "26°");
            check("today low", ui.find("Today Low").text, "L 22°");
            check("updated", ui.find("Updated Label").text, "Updated: sample data");
        }
        if (frames === 40 || frames === 60 || frames === 90)
            console.log("selfdrive tap Unit Toggle ->", ui.tap("Unit Toggle"));
        if (frames === 50) {
            check("temp in F", ui.find("Current Temp").text, "79°");
            check("row 1 high F", ui.findAll("Forecast Row")[0]
                .find("Row High").text, "84°");
            check("unit label F", ui.find("Unit Label").text, "°C");
        }
        if (frames === 70)
            check("back to C", ui.find("Current Temp").text, "26°");
        if (frames === 125) {
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
        if (frames === 126) fetchLive();
    });
} else {
    fetchLive();
}
