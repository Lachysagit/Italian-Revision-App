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
    const needsOnboarding = settings.needsOnboarding !== false;
    // the gate's job is to stop an un-onboarded account starting an exam, so a
    // page that cannot start one has nothing to hold anybody at. The classes
    // page is that page, and passes false: a teacher account never onboards at
    // all - year and subject level describe a student - and a student who has
    // not finished signing up is still allowed to see which classes they are in

    buildGate();
    buildAccountBox();
    buildSettings();
    loadGateLanguages();
    // fetched up front rather than when the sign-up step opens: it is a small
    // request, it shares the connection with everything else the page is
    // already asking for, and the picker is then never waiting on it

    return fetch("/api/me", { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : null))
        .then((user) => {
            currentUser = user;
            paintAccountBox(user);
            if (typeof setTranslateLocked === "function") {
                setTranslateLocked(!(user && user.usage && user.usage.translation));
            }
            // here rather than in each page, so every page with the translate
            // box opens it or shuts it from the same answer
            paintTeacherLink(user);

            if (!user) {
                showStep("choice");
                return null;
            }
            if (!user.onboarded && needsOnboarding) {
                return finishPendingSignup(onReady);
                // teachers included: it is what offers them a first class.
                // The year and level questions are a student's, and that
                // function is where the two paths part
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
        showStep(currentUser.has_created_class ? "teacherWelcome" : "classOffer");
        return null;
        // year and subject level describe a student's cohort, so there is
        // nothing here to ask a teacher. Any draft they carry in is a student
        // answer set and must not be posted against a teacher account.
        //
        // A teacher account never becomes onboarded - there is no profile for
        // it to save - so this branch is every sign-in, not just the first. The
        // offer is what has to be first-time-only, and owning a class already
        // is what says it has been made
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
                    You are signed in as a teacher. Your classes are on the
                    classes page, and the practice exam itself is open to you
                    the same way it is to a student.
                </p>
                <p id="authTeacherWelcomeNote">
                    An exam runs on a year and subject level, which a teacher
                    account has none of, so start one from a class rather than
                    from this page.
                </p>

                <a class="authOption" href="/teacher">
                    <span class="authOptionLabel">View your classes</span>
                    <span class="authOptionNote">Everything you teach, in one place</span>
                </a>

                <button type="button" id="authTeacherSignOut">Sign in as someone else</button>
            </div>

            <div id="authClassOffer" hidden>
                <p id="authClassOfferBlurb">
                    Would you like to create a class? You can add students to
                    it later.
                </p>

                <button type="button" class="authOption" id="authClassYes">
                    <span class="authOptionLabel">Yes, create a class</span>
                    <span class="authOptionNote">Pick a year level, subject level and language</span>
                </button>

                <button type="button" class="authOption" id="authClassNo">
                    <span class="authOptionLabel">No, not now</span>
                    <span class="authOptionNote">Go straight to the site</span>
                </button>
            </div>

            <form id="authClassForm" hidden>
                <label for="authClassYear">Year level</label>
                <select id="authClassYear" required>
                    <option value="">Choose...</option>
                    <option value="7">Year 7</option>
                    <option value="8">Year 8</option>
                    <option value="9">Year 9</option>
                    <option value="10">Year 10</option>
                    <option value="11">Year 11</option>
                    <option value="12">Year 12</option>
                </select>

                <label for="authClassLevel">Subject level</label>
                <select id="authClassLevel" required>
                    <option value="">Choose...</option>
                    <option value="beginners">Beginners</option>
                    <option value="continuers">Continuers</option>
                    <option value="advanced">Advanced</option>
                    <option value="extension">Extension</option>
                </select>

                <label for="authClassLanguage">Language</label>
                <select id="authClassLanguage" required>
                    <option value="">Choose...</option>
                </select>

                <button type="submit" id="authClassCreate">Create class</button>
                <button type="button" id="authClassBack">Back</button>
            </form>

            <div id="authClassDone" hidden>
                <p id="authClassDoneBlurb"></p>

                <a class="authOption" href="/teacher">
                    <span class="authOptionLabel">View your classes</span>
                    <span class="authOptionNote">Go through to the class you just made</span>
                </a>

                <button type="button" id="authClassSkip">Skip for now</button>
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

    document.getElementById("authClassYes").onclick = () => showStep("classForm");
    document.getElementById("authClassNo").onclick = dismissClassOffer;
    document.getElementById("authClassBack").onclick = () => showStep("classOffer");
    document.getElementById("authClassSkip").onclick = dismissClassOffer;
    document.getElementById("authClassForm").onsubmit = submitClass;
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
        classOffer: document.getElementById("authClassOffer"),
        classForm: document.getElementById("authClassForm"),
        classDone: document.getElementById("authClassDone"),
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
    } else if (step === "classOffer") {
        title.textContent = currentUser && currentUser.name
            ? "Welcome, " + currentUser.name
            : "Welcome";
        blurb.textContent = "One thing before you start.";
    } else if (step === "classForm") {
        title.textContent = "Create a class";
        blurb.textContent = "What does this class study?";
    } else if (step === "classDone") {
        title.textContent = "Class created";
        blurb.textContent = "That is everything.";
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

            fillSettingsLanguages();
            fillClassLanguages();
            // and the settings and create-a-class pickers were both built from
            // the same empty list
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

// Persist a language change made from the exam page's own picker, so the
// choice follows the student to their next device rather than living in one
// browser's localStorage. The endpoint validates all three fields together, so
// year and subject level are resent unchanged from the account we already hold.
function savePreferredLanguage(id) {
    if (!currentUser || !currentUser.onboarded) return Promise.resolve(null);
    if (!id || id === currentUser.preferred_language) return Promise.resolve(null);

    return postProfile({
        preferred_language: id,
        year_level: currentUser.year_level,
        subject_level: currentUser.subject_level,
    })
        .then((body) => {
            currentUser.preferred_language = id;
            return body;
        })
        .catch(() => null);
    // swallowed: the picker has already changed and the exam runs on what is
    // on screen. A failed save costs the student nothing this session, and a
    // 403 is the expected answer for a student whose teacher owns their cohort
}

// ---------------------------------------------------------------------------
// the teacher's create-a-class offer
// ---------------------------------------------------------------------------
//
// Shown on a teacher's sign-in until they own a class, which is what makes it a
// first-time prompt without a column to record that it was answered: creating
// one is the only thing that stops it coming back. "No" therefore only settles
// the current page load, which is the honest scope for a question the teacher
// has not answered either way.

// "No, not now", and the same button on the confirmation. Either way the gate
// comes down and the page runs as it would for anybody signed in.
function dismissClassOffer() {
    hideGate();
    classOfferDone();
}

// The page was set up behind the gate and its onReady never fired, because a
// teacher signing in is not an onboarded account. Nothing on the exam page
// works for a teacher anyway - an exam runs on a year and subject level - so
// this only lowers the gate rather than calling back into the page.
function classOfferDone() {
    paintAccountBox(currentUser);
    // the corner was painted before the offer went up, and the classes button
    // it carries is the way back here
}

function submitClass(event) {
    event.preventDefault();
    hideGateError();

    const wanted = {
        year_level: document.getElementById("authClassYear").value,
        subject_level: document.getElementById("authClassLevel").value,
        language_id: document.getElementById("authClassLanguage").value,
    };

    if (!wanted.year_level || !wanted.subject_level || !wanted.language_id) {
        showGateError("Please answer all three.");
        return;
    }

    const create = document.getElementById("authClassCreate");
    create.disabled = true;

    fetch("/api/classes", {
        method: "POST",
        credentials: "same-origin",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(wanted),
    })
        .then((response) =>
            response
                .json()
                .catch(() => ({}))
                .then((body) => {
                    if (!response.ok) {
                        throw new Error(body.error || "could not create that class");
                    }
                    return body;
                }))
        .then((body) => {
            currentUser.has_created_class = true;
            // so a second pass through the gate this page load does not offer
            // again. The server is the real record; this keeps the two in step

            document.getElementById("authClassDoneBlurb").textContent =
                body.name
                    ? body.name + " is ready. Add students to it whenever you like."
                    : "Your class is ready.";
            showStep("classDone");
            // the class page is a link on this panel rather than a redirect: a
            // teacher who only wanted the class made can skip straight past it
        })
        .catch((failure) => {
            create.disabled = false;
            showGateError(failure.message);
        });
}

// The gate's language picker is filled from /api/languages, and so is this one.
// Called from loadGateLanguages for the same reason fillSettingsLanguages is:
// the list can land after the panels were built.
function fillClassLanguages() {
    const select = document.getElementById("authClassLanguage");
    if (!select) return;

    const chosen = select.value;
    select.textContent = "";

    const blank = document.createElement("option");
    blank.value = "";
    blank.textContent = "Choose...";
    select.appendChild(blank);

    gateLanguages.forEach((language) => {
        const option = document.createElement("option");
        option.value = language.id;
        option.textContent = language.label;
        select.appendChild(option);
    });

    setSelectValue("authClassLanguage", chosen);
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

// The dashboard link, for teachers only. Added to whichever page is open so the
// nav is the same everywhere, and marked current on the dashboard itself.
function paintTeacherLink(user) {
    const nav = document.getElementById("siteNav");
    if (!nav || !user || !user.is_teacher || document.getElementById("teacherLink")) {
        return;
    }
    const link = document.createElement("a");
    link.id = "teacherLink";
    link.href = "/teacher";
    link.textContent = "Teacher";
    if (window.location.pathname === "/teacher") {
        link.className = "current";
    }
    const box = document.getElementById("accountBox");
    nav.insertBefore(link, box);
    // before the account corner, which pushes itself to the right edge
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


    const settings = document.createElement("button");
    settings.type = "button";
    settings.id = "acctOpenButton";
    settings.textContent = "Account settings";
    settings.onclick = openSettings;
    box.appendChild(settings);
    // only for a signed-in account, and only beside the sign-out button: the
    // three questions it asks have no answer to edit until there is one

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

// ---------------------------------------------------------------------------
// account settings
// ---------------------------------------------------------------------------
//
// The same three questions sign-up asks, reopened from the nav. It posts to
// the same endpoint, so the server's rules - the closed lists, and the 403 for
// a student whose teacher owns their cohort - hold here without restating.
//
// Changing year or subject level moves the student between cohorts, so a save
// is confirmed before it is sent. The confirmation names the fields that
// actually changed rather than asking in the abstract.

const SETTINGS_FIELDS = [
    { key: "preferred_language", id: "acctLanguage", label: "Language" },
    { key: "year_level", id: "acctYear", label: "Year level" },
    { key: "subject_level", id: "acctLevel", label: "Subject level" },
];

function buildSettings() {
    if (document.getElementById("acctDialog")) return;

    const dialog = document.createElement("div");
    dialog.id = "acctDialog";
    dialog.hidden = true;
    dialog.innerHTML = `
        <div id="acctCard" role="dialog" aria-modal="true"
             aria-labelledby="acctTitle">
            <h2 id="acctTitle">Account settings</h2>

            <form id="acctForm">
                <label for="acctLanguage">Language</label>
                <select id="acctLanguage" required>
                    <option value="">Choose...</option>
                </select>

                <label for="acctYear">Year level</label>
                <select id="acctYear" required>
                    <option value="">Choose...</option>
                    <option value="7">Year 7</option>
                    <option value="8">Year 8</option>
                    <option value="9">Year 9</option>
                    <option value="10">Year 10</option>
                    <option value="11">Year 11</option>
                    <option value="12">Year 12</option>
                </select>

                <label for="acctLevel">Subject level</label>
                <select id="acctLevel" required>
                    <option value="">Choose...</option>
                    <option value="beginners">Beginners</option>
                    <option value="continuers">Continuers</option>
                    <option value="advanced">Advanced</option>
                    <option value="extension">Extension</option>
                </select>

                <p id="acctError" hidden></p>

                <div class="acctActions">
                    <button type="button" id="acctCancel">Cancel</button>
                    <button type="submit" id="acctSave">Save</button>
                </div>
            </form>

            <div id="acctConfirm" hidden>
                <h3 id="acctConfirmTitle">Are you sure?</h3>
                <ul id="acctChanges"></ul>
                <p id="acctWarning">
                    These set the cohort your exams are built for.
                </p>
                <div class="acctActions">
                    <button type="button" id="acctBack">Go back</button>
                    <button type="button" id="acctConfirmSave">Yes, change</button>
                </div>
            </div>
        </div>`;
    document.body.appendChild(dialog);

    fillSettingsLanguages();

    document.getElementById("acctCancel").onclick = closeSettings;
    document.getElementById("acctBack").onclick = () => showConfirm(false);
    document.getElementById("acctForm").onsubmit = reviewSettings;
    document.getElementById("acctConfirmSave").onclick = commitSettings;

    dialog.onclick = (event) => {
        if (event.target === dialog) closeSettings();
        // the backdrop and nothing else: a click that began inside the card
        // lands on the card, so dragging across a picker cannot close it
    };

    document.addEventListener("keydown", (event) => {
        if (event.key === "Escape" && !dialog.hidden) closeSettings();
    });
}

// The language list is the one the gate already fetched, which can land after
// this runs - loadGateLanguages calls back here for exactly that reason.
function fillSettingsLanguages() {
    const select = document.getElementById("acctLanguage");
    if (!select) return;

    const chosen = select.value;
    select.textContent = "";

    const blank = document.createElement("option");
    blank.value = "";
    blank.textContent = "Choose...";
    select.appendChild(blank);

    gateLanguages.forEach((language) => {
        const option = document.createElement("option");
        option.value = language.id;
        option.textContent = language.label;
        select.appendChild(option);
    });

    setSelectValue("acctLanguage", chosen);
    // a refill must not silently undo a choice made while the list was landing
}

function openSettings() {
    if (!currentUser) return;

    buildSettings();
    hideSettingsError();
    showConfirm(false);

    SETTINGS_FIELDS.forEach((field) => {
        setSelectValue(field.id, currentUser[field.key]);
    });

    document.getElementById("acctDialog").hidden = false;
    document.body.classList.add("gated");
    document.getElementById("acctLanguage").focus();
}

function closeSettings() {
    const dialog = document.getElementById("acctDialog");
    if (dialog) dialog.hidden = true;

    const gate = document.getElementById("authGate");
    if (!gate || gate.hidden) document.body.classList.remove("gated");
    // the gate holds the same class, and only one of the two can be up at a
    // time - but checking costs nothing and keeps the page scrollable
}

function showConfirm(on) {
    document.getElementById("acctForm").hidden = Boolean(on);
    document.getElementById("acctConfirm").hidden = !on;
}

// Save pressed: work out what actually changed and ask about that. Nothing is
// sent until the confirmation is answered.
function reviewSettings(event) {
    event.preventDefault();
    hideSettingsError();

    const chosen = {};
    SETTINGS_FIELDS.forEach((field) => {
        chosen[field.key] = document.getElementById(field.id).value;
    });

    if (!chosen.preferred_language || !chosen.year_level ||
        !chosen.subject_level) {
        showSettingsError("Please answer all three.");
        return;
    }

    const changes = SETTINGS_FIELDS.filter(
        (field) => chosen[field.key] !== currentUser[field.key]);

    if (changes.length === 0) {
        closeSettings();
        return;
        // nothing to confirm, and nothing worth a request
    }

    const list = document.getElementById("acctChanges");
    list.textContent = "";
    changes.forEach((field) => {
        const item = document.createElement("li");
        item.textContent = field.label + ": " +
            settingsLabelFor(field, currentUser[field.key]) + " → " +
            settingsLabelFor(field, chosen[field.key]);
        list.appendChild(item);
    });

    document.getElementById("acctConfirm").dataset.pending =
        JSON.stringify(chosen);
    showConfirm(true);
    document.getElementById("acctBack").focus();
}

// What the picker shows for a value, so the confirmation reads "Year 10" and
// "Continuers" rather than "10" and "continuers".
function settingsLabelFor(field, value) {
    const select = document.getElementById(field.id);
    if (!select || !value) return "not set";

    const option = Array.prototype.find.call(
        select.options, (o) => o.value === value);
    return option ? option.textContent : value;
}

function commitSettings() {
    const confirm = document.getElementById("acctConfirm");
    let chosen = null;
    try {
        chosen = JSON.parse(confirm.dataset.pending || "null");
    } catch (ignored) {
        chosen = null;
    }
    if (!chosen) {
        showConfirm(false);
        return;
    }

    const save = document.getElementById("acctConfirmSave");
    save.disabled = true;

    postProfile(chosen)
        .then(() => {
            window.location.reload();
            // the language, the topic list and the exam's whole shape are read
            // from the account at load. A reload is the one honest way to put
            // every one of them on the new cohort
        })
        .catch((failure) => {
            save.disabled = false;
            showConfirm(false);
            showSettingsError(failure.message);
            // back to the form with the reason above the buttons: the server
            // refuses a student whose teacher owns their cohort, and that
            // answer has to be readable beside the pickers it is about
        });
}

function showSettingsError(message) {
    const error = document.getElementById("acctError");
    if (!error) return;
    error.textContent = message;
    error.hidden = false;
}

function hideSettingsError() {
    const error = document.getElementById("acctError");
    if (error) error.hidden = true;
}
