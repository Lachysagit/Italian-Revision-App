// Sign-in gate, sign-up step, and the signed-in-as corner of the nav.
//
// The gate is an overlay over the page rather than a redirect, so whatever the
// page wanted to fetch at load still runs behind it. By the time somebody has
// clicked through Google and come back, the pickers are already populated and
// the exam can start on the first click.
//
// Loaded with a plain <script src> like translate.js, and defines one entry
// point. Load order is the only dependency.

let currentUser = null;

function initAccount(options) {
    const settings = options || {};
    const onReady = settings.onReady || function () {};
    // called once the page is unblocked, so a page can defer work that only
    // makes sense for a signed-in visitor

    buildGate();
    buildAccountBox();

    return fetch("/api/me", { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : null))
        .then((user) => {
            currentUser = user;
            paintAccountBox(user);

            if (!user) {
                showGate("signin");
                return null;
            }
            if (!user.onboarded) {
                showGate("signup");
                return null;
            }
            hideGate();
            onReady(user);
            return user;
        })
        .catch(() => {
            // the server being unreachable and nobody being signed in look the
            // same from here, and both mean the page must stay blocked
            paintAccountBox(null);
            showGate("signin");
            return null;
        });
}

// ---------------------------------------------------------------------------
// the gate
// ---------------------------------------------------------------------------

function buildGate() {
    if (document.getElementById("authGate")) return;

    const gate = document.createElement("div");
    gate.id = "authGate";
    gate.hidden = true;
    gate.innerHTML = `
        <div id="authCard">
            <h2 id="authTitle">Sign in</h2>
            <p id="authBlurb"></p>

            <a id="authGoogle" href="/auth/login">Sign in with Google</a>

            <form id="authSignup" hidden>
                <label for="authYear">Year level</label>
                <select id="authYear" required>
                    <option value="">Choose...</option>
                    <option value="7">Year 7</option>
                    <option value="8">Year 8</option>
                    <option value="9">Year 9</option>
                    <option value="10">Year 10</option>
                    <option value="11">Year 11</option>
                    <option value="12">Year 12</option>
                </select>

                <label for="authLevel">Subject level</label>
                <select id="authLevel" required>
                    <option value="">Choose...</option>
                    <option value="beginners">Beginners</option>
                    <option value="continuers">Continuers</option>
                    <option value="advanced">Advanced</option>
                    <option value="extension">Extension</option>
                </select>

                <label for="authLanguage">Language</label>
                <select id="authLanguage" required>
                    <option value="">Choose...</option>
                </select>

                <button type="submit" id="authSubmit">Finish setting up</button>
                <p id="authError" hidden></p>
            </form>
        </div>`;
    document.body.appendChild(gate);

    document.getElementById("authSignup").onsubmit = submitSignup;
}

function showGate(mode) {
    const gate = document.getElementById("authGate");
    const form = document.getElementById("authSignup");
    const google = document.getElementById("authGoogle");
    const title = document.getElementById("authTitle");
    const blurb = document.getElementById("authBlurb");

    if (mode === "signup") {
        title.textContent = "Almost there";
        blurb.textContent = currentUser && currentUser.name
            ? `Welcome, ${currentUser.name}. Tell us what you are studying.`
            : "Tell us what you are studying.";
        google.hidden = true;
        form.hidden = false;
        fillLanguageChoices();
    } else {
        title.textContent = "Sign in";
        blurb.textContent =
            "Sign in with Google to practise and to keep a record of your exams.";
        google.hidden = false;
        form.hidden = true;
    }

    gate.hidden = false;
    document.body.classList.add("gated");
}

function hideGate() {
    const gate = document.getElementById("authGate");
    if (gate) gate.hidden = true;
    document.body.classList.remove("gated");
}

function fillLanguageChoices() {
    const select = document.getElementById("authLanguage");
    if (select.options.length > 1) return;
    // already filled: the list does not change while the page is open

    fetch("/api/languages", { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : []))
        .then((languages) => {
            languages.forEach((language) => {
                const option = document.createElement("option");
                option.value = language.id;
                option.textContent = language.label;
                select.appendChild(option);
            });
        })
        .catch(() => {
            // leaving only "Choose..." is honest: the form will not submit
            // without a language, and the server would reject it anyway
        });
}

function submitSignup(event) {
    event.preventDefault();

    const error = document.getElementById("authError");
    const button = document.getElementById("authSubmit");
    error.hidden = true;
    button.disabled = true;

    const payload = {
        year_level: document.getElementById("authYear").value,
        subject_level: document.getElementById("authLevel").value,
        preferred_language: document.getElementById("authLanguage").value,
    };

    fetch("/api/me/profile", {
        method: "POST",
        credentials: "same-origin",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
    })
        .then((response) =>
            response.json().then((body) => ({ ok: response.ok, body })))
        .then((result) => {
            if (!result.ok) {
                throw new Error(result.body.error || "could not save that");
            }
            window.location.reload();
            // a reload rather than carrying on in place: the page's own setup
            // ran while the gate was up and has no idea a profile now exists,
            // and this happens once per account
        })
        .catch((failure) => {
            error.textContent = failure.message;
            error.hidden = false;
            button.disabled = false;
        });
}

// ---------------------------------------------------------------------------
// the nav corner
// ---------------------------------------------------------------------------

function buildAccountBox() {
    const nav = document.getElementById("siteNav");
    if (!nav || document.getElementById("accountBox")) return;

    const box = document.createElement("span");
    box.id = "accountBox";
    nav.appendChild(box);
}

function paintAccountBox(user) {
    const box = document.getElementById("accountBox");
    if (!box) return;
    box.textContent = "";

    if (!user) {
        const link = document.createElement("a");
        link.href = "/auth/login";
        link.textContent = "Sign in";
        box.appendChild(link);
        return;
    }

    const name = document.createElement("span");
    name.id = "accountName";
    name.textContent = user.name || user.email;
    box.appendChild(name);

    const form = document.createElement("form");
    form.method = "POST";
    form.action = "/auth/logout";
    // a form rather than a link: /auth/logout is POST only, so a prefetch or a
    // crawler cannot sign somebody out by following it

    const button = document.createElement("button");
    button.type = "submit";
    button.id = "signOutButton";
    button.textContent = "Sign out";
    form.appendChild(button);
    box.appendChild(form);
}
