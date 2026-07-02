// login — benchmark app: password mask + form validation + fetch POST.
// Flow: SignIn (email + masked password + inline errors) -> validate ->
// POST https://httpbin.org/post -> Welcome (dissolve) -> sign out clears
// the form. The fetch outcome is log-only so the flow stays deterministic
// offline: both success and failure land on Welcome.

ui.selectFrame(APP.entryFrame || "SignIn");

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

// idempotent on hot reload: setEditable/setPassword just re-apply
ui.setEditable("Email Input");
ui.setEditable("Password Input");
ui.setPassword("Password Input", true);

let masked = true;

function validate() {
    const email = ui.find("Email Input").text.trim();
    const pwd = ui.find("Password Input").text;
    const emailOk = EMAIL_RE.test(email);
    const pwdOk = pwd.length >= 8;
    ui.setVisible("Email Error", !emailOk);
    ui.setVisible("Password Error", !pwdOk);
    return emailOk && pwdOk ? email : null;
}

function signIn() {
    const email = validate();
    if (!email) return;
    // Privacy: the POST body carries only the email. The password never
    // leaves the process — it is not in the request body, and with
    // ui.setPassword the field's copy/cut return "" so it can never reach
    // a clipboard either.
    fetch("https://httpbin.org/post", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ event: "sign-in", email }),
    }).then(r => r.text().then(t => {
        // httpbin.org occasionally serves an HTML error page: parse
        // defensively, this is log-only either way.
        let echoed = "";
        try { echoed = (JSON.parse(t).json || {}).email || ""; } catch (e) {}
        console.log("sign-in POST status:", r.status, "echoed email:", echoed);
    })).catch(e => console.log("sign-in POST failed (offline is fine):", e));
    ui.setText("Welcome Title", "你好，" + email.split("@")[0]);
    ui.setText("Welcome Email", email);
    ui.blur();
    ui.navigateTo("Welcome", "dissolve", 0.3);
}

function resetForm() {
    ui.find("Email Input").text = "";
    ui.find("Password Input").text = "";
    ui.setVisible("Email Error", false);
    ui.setVisible("Password Error", false);
    masked = true;
    ui.setPassword("Password Input", true);
    ui.setText("Toggle Label", "显示");
}

ui.onClick("Sign In Button", () => signIn());
ui.onClick("Toggle Mask", () => {
    masked = !masked;
    ui.setPassword("Password Input", masked);
    ui.setText("Toggle Label", masked ? "显示" : "隐藏");
});
ui.onClick("Forgot Link", () =>
    console.log("forgot-password flow: out of scope for this benchmark"));
ui.onClick("Sign Out Button", () => {
    ui.blur();
    resetForm();
    ui.navigateBack(0.3);
});

ui.onHover("Sign In Button",
    (node, entered) => ui.setOpacity(node, entered ? 0.88 : -1.0));
ui.onHover("Sign Out Button",
    (node, entered) => ui.setOpacity(node, entered ? 0.88 : -1.0));

console.log("login ready");

// ---- --shot staging: SignIn with the password typed, mask on ----
if (globalThis.SHOT && !globalThis.SELFDRIVE) {
    let staged = false;
    ui.onUpdate(() => {
        if (staged) return;
        staged = true;
        ui.focusText("Email Input");
        ui.typeText("ada@example.com");
        ui.focusText("Password Input");
        ui.typeText("hunter2077");
    });
}

// ---- unattended tour (figoplay --selfdrive) ----
// frame 30 -> <prefix>_home.png (SignIn, inline validation errors visible)
// frame 110 -> <prefix>_nav.png (Welcome, post sign-in)
// frame 125 -> verdict; figoplay exits at 140
if (globalThis.SELFDRIVE) {
    let frames = 0;  // count frames, not dt: first dt includes file load
    const checks = [];
    const check = (what, got, want) => checks.push([what, String(got), String(want)]);
    ui.onUpdate(() => {
        frames++;

        // empty form: both errors must show
        if (frames === 10) ui.tap("Sign In Button");
        if (frames === 14) {
            check("empty email error", ui.find("Email Error").visible, "true");
            check("empty pwd error", ui.find("Password Error").visible, "true");
        }

        // bad email format keeps the email error up
        if (frames === 18) {
            ui.focusText("Email Input");
            ui.typeText("ada@invalid");
        }
        if (frames === 22) ui.tap("Sign In Button");
        if (frames === 26) {
            check("bad email error", ui.find("Email Error").visible, "true");
            check("bad email kept", ui.find("Email Input").text, "ada@invalid");
        }

        // fix the email, type the password; frame 30 home shot shows errors
        if (frames === 34) {
            ui.focusText("Email Input");
            ui.editKey("selectAll");
            ui.typeText("ada@example.com");
        }
        if (frames === 38) {
            ui.focusText("Password Input");
            ui.typeText("hunter2077");
        }
        if (frames === 42) {
            const pwd = ui.find("Password Input");
            check("pwd mask on", pwd.passwordMask, "true");
            check("pwd plaintext", pwd.text, "hunter2077");
        }

        // show/hide toggle
        if (frames === 46) ui.tap("Toggle Mask");
        if (frames === 50) {
            check("mask off", ui.find("Password Input").passwordMask, "false");
            check("toggle says hide", ui.find("Toggle Label").text, "隐藏");
        }
        if (frames === 54) ui.tap("Toggle Mask");
        if (frames === 58) {
            check("mask back on", ui.find("Password Input").passwordMask, "true");
            check("toggle says show", ui.find("Toggle Label").text, "显示");
        }

        // valid submit -> Welcome (dissolve); fetch result is log-only
        if (frames === 62) ui.tap("Sign In Button");
        if (frames === 80) {
            check("on Welcome", ui.currentFrame().name, "Welcome");
            check("welcome has name", ui.find("Welcome Title").text, "你好，ada");
            check("welcome email", ui.find("Welcome Email").text, "ada@example.com");
            check("errors cleared", ui.find("Email Error").visible, "false");
        }

        // frame 110: _nav.png shows Welcome; then sign out clears the form
        if (frames === 112) ui.tap("Sign Out Button");
        if (frames === 124) {
            check("back on SignIn", ui.currentFrame().name, "SignIn");
            check("email cleared", ui.find("Email Input").text, "");
            check("pwd cleared", ui.find("Password Input").text, "");
            check("email error hidden", ui.find("Email Error").visible, "false");
            check("pwd error hidden", ui.find("Password Error").visible, "false");
            check("mask restored", ui.find("Password Input").passwordMask, "true");
        }

        if (frames === 125) {
            const fails = checks.filter(([, got, want]) => got !== want);
            for (const [what, got, want] of checks)
                console.log(`bench check ${what}: got=${got} want=${want}`);
            console.log(fails.length ? "BENCH: FAIL" : "BENCH: PASS");
        }
    });
}
