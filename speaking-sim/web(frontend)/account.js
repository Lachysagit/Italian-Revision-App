// Sign-in gate, sign-up flow, and the signed-in-as corner of the nav.
//
// The gate is an overlay over the page rather than a redirect, so whatever the
// page wanted to fetch at load still runs behind it. By the time somebody has
// clicked through Google and come back, the pickers are already populated and
// the exam can start on the first click.
//
// The sign-up path asks for language, year and subject level BEFORE Google,
// because a new student has nothing to come back to otherwise: the answers are
// held in sessionStorage across the redirect and posted once the account
// exists. Signing in is the other button and goes straight to Google.
//
// Loaded with a plain <script src> like translate.js, and defines one entry
// point. Load order is the only dependency.

const SIGNUP_DRAFT = "signupDraft";

let currentUser = null;
let gateLanguages = [];

function initAccount(options) {
    const settings = options || {};
    const onReady = settings.onReady || function () {};

    buildGate();
    buildAccountBox();
    loadGateLanguages();
    // fetched up front rather than when the sign-up step opens: it is a small
    // request, it shares the connection with everything else the page is
    // already asking for, and the picker is then never waiting on it

    return fetch("/api/me", { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : null))
        .then((user) => {
            currentUser = user;
            paintAccountBox(user);

            if (!user) {
                showStep("choice");
                return null;
            }
            if (!user.onboarded) {
                return finishPendingSignup(onReady);
            }
            hideGate();
            onReady(user);
            return user;
        })
        .catch(() => {
            // the server being unreachable and nobody being signed in look the
            // same from here, and both mean the page must stay blocked
            paintAccountBox(null);
            showStep("choice");
            return null;
        });
}

// A signed-in account that is not onboarded: either they just came back from
// Google having filled the form, or they signed in without ever finishing it.
function finishPendingSignup(onReady) {
    let draft = null;
    try {
        draft = JSON.parse(sessionStorage.getItem(SIGNUP_DRAFT) || "null");
    } catch (ignored) {
        draft = null;
    }

    if (!draft) {
        showStep("details");
        return null;
        // signed in but nothing saved, so ask now. This is also the path for
        // somebody who pressed "sign in" when they meant "create an account"
    }

    return postProfile(draft)
        .then(() => {
            sessionStorage.removeItem(SIGNUP_DRAFT);
            window.location.reload();
            // the page set itself up while the gate was raised and has no idea
            // a profile now exists. Once per account, so a reload is honest
            return null;
        })
        .catch(() => {
            sessionStorage.removeItem(SIGNUP_DRAFT);
            showStep("details");
            showGateError("Those details could not be saved. Please try again.");
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
            <h2 id="authTitle">Welcome</h2>
            <p id="authBlurb">Practise speaking, and keep a record of your exams.</p>

            <div id="authChoice">
                <a class="authOption" href="/auth/login">
                    <span class="authOptionLabel">Sign in with Google</span>
                    <span class="authOptionNote">For students who already have an account</span>
                </a>

                <button type="button" class="authOption" id="authCreate">
                    <span class="authOptionLabel">Create an account</span>
                    <span class="authOptionNote">First time here</span>
                </button>
            </div>

            <form id="authDetails" hidden>
                <label for="authLanguage">Language</label>
                <select id="authLanguage" required>
                    <option value="">Choose...</option>
                </select>

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

                <button type="submit" id="authNext">Next</button>
                <button type="button" id="authBack">Back</button>
            </form>

            <p id="authError" hidden></p>
        </div>`;
    document.body.appendChild(gate);

    document.getElementById("authCreate").onclick = () => showStep("details");
    document.getElementById("authBack").onclick = () => showStep("choice");
    document.getElementById("authDetails").onsubmit = submitDetails;
}

function showStep(step) {
    const gate = document.getElementById("authGate");
    const choice = document.getElementById("authChoice");
    const details = document.getElementById("authDetails");
    const title = document.getElementById("authTitle");
    const blurb = document.getElementById("authBlurb");
    const next = document.getElementById("authNext");

    hideGateError();

    if (step === "details") {
        const signedIn = Boolean(currentUser);
        title.textContent = signedIn ? "Almost there" : "Create an account";
        blurb.textContent = signedIn
            ? "Tell us what you are studying."
            : "Tell us what you are studying, then sign in with Google.";
        next.textContent = signedIn ? "Finish" : "Next";
        // the button says what actually happens next: a signed-in student is
        // saving, one who is not is about to be sent to Google
        document.getElementById("authBack").hidden = signedIn;
        choice.hidden = true;
        details.hidden = false;
        restoreDraft();
    } else {
        title.textContent = "Welcome";
        blurb.textContent = "Practise speaking, and keep a record of your exams.";
        choice.hidden = false;
        details.hidden = true;
    }

    gate.hidden = false;
    document.body.classList.add("gated");
}

function hideGate() {
    const gate = document.getElementById("authGate");
    if (gate) gate.hidden = true;
    document.body.classList.remove("gated");
}

function showGateError(message) {
    const error = document.getElementById("authError");
    if (!error) return;
    error.textContent = message;
    error.hidden = false;
}

function hideGateError() {
    const error = document.getElementById("authError");
    if (error) error.hidden = true;
}

function loadGateLanguages() {
    return fetch("/api/languages", { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : []))
        .then((list) => {
            gateLanguages = Array.isArray(list) ? list : [];
            const select = document.getElementById("authLanguage");
            if (!select) return;

            gateLanguages.forEach((language) => {
                const option = document.createElement("option");
                option.value = language.id;
                option.textContent = language.label;
                select.appendChild(option);
            });
            restoreDraft();
            // the draft may have been restored before this landed, so re-apply
            // it now that the options it names actually exist
        })
        .catch(() => {
            showGateError(
                "Could not load the language list. Please refresh the page.");
            // said out loud rather than left as an empty picker: a form that
            // cannot be completed must explain itself
        });
}

// The answers survive the trip to Google in sessionStorage: it is per-tab and
// cleared when the tab closes, which is the right lifetime for a half-finished
// form. Nothing secret goes in it.
function saveDraft(draft) {
    try {
        sessionStorage.setItem(SIGNUP_DRAFT, JSON.stringify(draft));
    } catch (ignored) {
        // private browsing can refuse storage. The sign-up still works: the
        // student is simply asked again after signing in
    }
}

function restoreDraft() {
    let draft = null;
    try {
        draft = JSON.parse(sessionStorage.getItem(SIGNUP_DRAFT) || "null");
    } catch (ignored) {
        return;
    }
    if (!draft) return;

    setSelectValue("authLanguage", draft.preferred_language);
    setSelectValue("authYear", draft.year_level);
    setSelectValue("authLevel", draft.subject_level);
}

function setSelectValue(id, value) {
    const select = document.getElementById(id);
    if (!select || !value) return;
    if (Array.prototype.some.call(select.options, (o) => o.value === value)) {
        select.value = value;
    }
}

function submitDetails(event) {
    event.preventDefault();
    hideGateError();

    const draft = {
        preferred_language: document.getElementById("authLanguage").value,
        year_level: document.getElementById("authYear").value,
        subject_level: document.getElementById("authLevel").value,
    };

    if (!draft.preferred_language || !draft.year_level || !draft.subject_level) {
        showGateError("Please answer all three.");
        return;
    }

    const next = document.getElementById("authNext");
    next.disabled = true;

    if (!currentUser) {
        saveDraft(draft);
        window.location.href = "/auth/login";
        return;
        // off to Google. The answers are posted on the way back, once there is
        // an account to attach them to
    }

    postProfile(draft)
        .then(() => {
            sessionStorage.removeItem(SIGNUP_DRAFT);
            window.location.reload();
        })
        .catch((failure) => {
            showGateError(failure.message);
            next.disabled = false;
        });
}

function postProfile(draft) {
    return fetch("/api/me/profile", {
        method: "POST",
        credentials: "same-origin",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(draft),
    }).then((response) =>
        response
            .json()
            .catch(() => ({}))
            .then((body) => {
                if (!response.ok) {
                    throw new Error(body.error || "could not save that");
                }
                return body;
            }));
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
