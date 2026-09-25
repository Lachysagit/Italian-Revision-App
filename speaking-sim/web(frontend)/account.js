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

            discardDraft();
            // somebody who pressed "create an account", answered the three
            // pickers and then signed in with a Google account that already
            // exists: the account they landed in wins and its preferences are
            // left exactly as they were. The unused draft is dropped here
            // rather than left in the tab, where the next un-onboarded sign-in
            // would find it and quietly inherit this student's cohort

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

    if (currentUser.is_teacher) {
        showStep("teacherWelcome");
        return null;
        // year and subject level describe a student's cohort, so there is
        // nothing here to ask a teacher. Any draft they carry in is a student
        // answer set and must not be posted against a teacher account
    }

    if (!draft) {
        showStep("details");
        return null;
        // signed in but nothing saved, so ask now. This is also the path for
        // somebody who pressed "sign in" when they meant "create an account"
    }

    return postProfile(draft)
        .then(() => {
            discardDraft();
            window.location.reload();
            // the page set itself up while the gate was raised and has no idea
            // a profile now exists. Once per account, so a reload is honest
            return null;
        })
        .catch(() => {
            showStep("details");
            showGateError("Those details could not be saved. Please try again.");
            return null;
            // the draft is deliberately left in place: showStep restores it
            // into the form, so "try again" is one click rather than three
            // pickers. submitDetails clears it once the save lands
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

            <div id="authRole" hidden>
                <button type="button" class="authOption" id="authRoleStudent">
                    <span class="authOptionLabel">I'm a student</span>
                    <span class="authOptionNote">Sit practice exams and keep a record of them</span>
                </button>

                <button type="button" class="authOption" id="authRoleTeacher">
                    <span class="authOptionLabel">I'm a teacher</span>
                    <span class="authOptionNote">Set up classes and follow your students' progress</span>
                </button>

                <button type="button" id="authRoleBack">Back</button>
            </div>

            <div id="authTeacher" hidden>
                <p id="authTeacherBlurb">
                    Teacher accounts are set up by whoever runs this server, so
                    there is nothing to fill in here. Sign in with your school
                    Google account and your classes will be waiting.
                </p>
                <p id="authTeacherNote">
                    If you sign in and land on the student questions instead,
                    your address has not been added yet. Ask for it to be put on
                    the teacher list, then sign in again.
                </p>

                <a class="authOption" href="/auth/login">
                    <span class="authOptionLabel">Sign in with Google</span>
                    <span class="authOptionNote">Use your school address</span>
                </a>

                <button type="button" id="authTeacherBack">Back</button>
            </div>

            <div id="authTeacherWelcome" hidden>
                <p id="authTeacherWelcomeBlurb">
                    You are signed in as a teacher. Classes are not built yet,
                    so there is nothing to set up on this page for now.
                </p>
                <p id="authTeacherWelcomeNote">
                    To try the practice exam the way your students see it, use
                    a student account: this one has no year or subject level,
                    which is what an exam runs on.
                </p>

                <button type="button" id="authTeacherSignOut">Sign in as someone else</button>
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

    document.getElementById("authCreate").onclick = () => showStep("role");
    document.getElementById("authRoleStudent").onclick = () => showStep("details");
    document.getElementById("authRoleTeacher").onclick = () => showStep("teacher");
    document.getElementById("authRoleBack").onclick = () => showStep("choice");
    document.getElementById("authTeacherBack").onclick = () => showStep("role");
    document.getElementById("authTeacherSignOut").onclick = signOut;
    document.getElementById("authBack").onclick = leaveDetails;
    document.getElementById("authDetails").onsubmit = submitDetails;
}

// "Back" from the details step, which is a different move depending on how the
// student got there: one who has not signed in yet is just returning to the
// choice screen, while one who is already signed in has to be signed out
// before the choice screen means anything.
function leaveDetails() {
    if (!currentUser) {
        showStep("role");
        return;
        // back to the question they came through, not all the way out: they
        // picked "student" to get here and may have meant "teacher"
    }

    signOut();
}

// a real POST rather than a fetch: /auth/logout answers with a redirect to
// "/", and letting the browser follow it reloads the page signed out, which is
// exactly the state the choice screen expects
function signOut() {
    discardDraft();
    // whatever they typed belonged to the account they are leaving

    const form = document.createElement("form");
    form.method = "POST";
    form.action = "/auth/logout";
    form.hidden = true;
    document.body.appendChild(form);
    form.submit();
}

function showStep(step) {
    const gate = document.getElementById("authGate");
    const panels = {
        choice: document.getElementById("authChoice"),
        role: document.getElementById("authRole"),
        teacher: document.getElementById("authTeacher"),
        teacherWelcome: document.getElementById("authTeacherWelcome"),
        details: document.getElementById("authDetails"),
    };
    const title = document.getElementById("authTitle");
    const blurb = document.getElementById("authBlurb");

    hideGateError();

    Object.keys(panels).forEach((name) => {
        panels[name].hidden = name !== step;
    });
    // one pass over every panel rather than a hidden/shown pair per branch, so
    // adding a step cannot leave an older one on screen underneath

    if (step === "role") {
        title.textContent = "Create an account";
        blurb.textContent = "Which of these are you?";
    } else if (step === "teacher") {
        title.textContent = "Teacher accounts";
        blurb.textContent = "How to get signed in.";
    } else if (step === "teacherWelcome") {
        title.textContent = "Signed in as a teacher";
        blurb.textContent = currentUser && currentUser.name
            ? "Welcome, " + currentUser.name + "."
            : "Welcome.";
    } else if (step === "details") {
        prepareDetails();
    } else {
        title.textContent = "Welcome";
        blurb.textContent = "Practise speaking, and keep a record of your exams.";
    }

    gate.hidden = false;
    document.body.classList.add("gated");
}

// The student questions, which are reached two ways: forwards from the role
// step, and backwards by a signed-in account that has not onboarded yet. The
// wording and the way out differ between those, so they are settled here
// rather than at each call site.
function prepareDetails() {
    const signedIn = Boolean(currentUser);
    const next = document.getElementById("authNext");

    document.getElementById("authTitle").textContent =
        signedIn ? "Almost there" : "Create an account";
    document.getElementById("authBlurb").textContent = signedIn
        ? "Tell us what you are studying."
        : "Tell us what you are studying, then sign in with Google.";

    next.textContent = signedIn ? "Finish" : "Next";
    // the button says what actually happens next: a signed-in student is
    // saving, one who is not is about to be sent to Google
    next.disabled = false;
    // submitDetails disables it on the way to Google; coming back to this
    // step must not leave a dead button

    const back = document.getElementById("authBack");
    back.textContent = signedIn ? "Sign in as someone else" : "Back";
    // a signed-in student has no choice screen to go back to, but they do
    // need a way out of an account they did not mean to use: the gate
    // covers the nav, so the sign-out button behind it is unreachable

    restoreDraft();
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

function discardDraft() {
    try {
        sessionStorage.removeItem(SIGNUP_DRAFT);
    } catch (ignored) {
        // private browsing can refuse storage outright. Nothing was saved in
        // that case either, so there is nothing to drop
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
            discardDraft();
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
