// The student's side of classes on the exam page: which class an exam is sat
// for, and the box that joins a class with the code a teacher hands out.
//
// An ES module. client.js imports this file and this file imports it back, so
// neither may touch the other's bindings while it is still being evaluated:
// everything that reads client.js's state (the language picker, the turn
// state) runs later, from initClasses and from the handlers.

import { setTranslateLanguage } from "/translate.js";
import {
    LANGUAGE_STORAGE, languageSelect, languages, languagesReady,
    translateCodeFor, updatePageTitle, turnState,
} from "/client.js";
//client.js imports this file back. The cycle is safe because neither
//module touches the other's bindings while it is being evaluated - every
//use is inside a function that runs later. Keep it that way: a call to
//any of the names above at this file's top level would read a const that
//client.js has not reached yet and throw.

const CLASS_STORAGE = "examClass";
const PENDING_JOIN = "pendingJoinCode";
const PENDING_CLASS = "pendingExamClass";

export const classSelect = document.getElementById("classSelect");
export const joinClassButton = document.getElementById("joinClass");
const classHint = document.getElementById("classHint");
export const planSelect = document.getElementById("planSelect");
const planLabel = document.getElementById("planLabel");
const joinOverlay = document.getElementById("joinOverlay");
const joinForm = document.getElementById("joinModal");
const joinCodeInput = document.getElementById("joinCodeInput");
const joinError = document.getElementById("joinError");
const joinSubmit = document.getElementById("joinSubmit");

let studentClasses = [];
let classPlans = [];
//the chosen class's exam plans, names and lengths only: the server never
//sends a student the set questions of an exam they have not sat yet
let plansRequest = 0;
//which class's plans the picker is waiting on. A student flicking between
//classes can have two requests in flight, and only the newest may paint
//[{id, name, language, language_label, role}] from /api/classes, archived ones
//already left out by the server

// A /join/CODE link lands here as /?join=CODE. The code is lifted out of the
// address at once and parked in sessionStorage, because a student who is not
// signed in is about to go through Google and back, and the query string does
// not survive that trip - the tab's sessionStorage does.
(function capturePendingJoin() {
    const params = new URLSearchParams(window.location.search);
    const code = params.get("join");
    if (!code) return;

    try {
        sessionStorage.setItem(PENDING_JOIN, code);
    } catch (ignored) {
        // storage refused: the code is still used below if nothing redirects
    }
    params.delete("join");
    const rest = params.toString();
    window.history.replaceState(null, "",
        window.location.pathname + (rest ? `?${rest}` : "") + window.location.hash);
    // out of the address bar, so a refresh or a bookmark does not join again
})();

// A "Sit an exam for this class" link from /classes lands here as /?class=ID.
// Parked and cleared like the join code above, and for the same reason: the
// student may be about to go through Google, which does not keep the query.
(function capturePendingClass() {
    const params = new URLSearchParams(window.location.search);
    const id = params.get("class");
    if (!id) return;

    try {
        sessionStorage.setItem(PENDING_CLASS, id);
    } catch (ignored) {
        // storage refused: the link then simply does not preselect anything
    }
    params.delete("class");
    const rest = params.toString();
    window.history.replaceState(null, "",
        window.location.pathname + (rest ? `?${rest}` : "") + window.location.hash);
})();

export function initClasses() {
    wireJoinBox();
    return loadStudentClasses().then(() => {
        let pending = null;
        try {
            pending = sessionStorage.getItem(PENDING_JOIN);
            sessionStorage.removeItem(PENDING_JOIN);
        } catch (ignored) {
            pending = null;
        }
        if (pending) {
            joinWithCode(pending);
            // a join link joins straight away: following it was the request
        }
    });
}

function loadStudentClasses() {
    return fetch("/api/classes", { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : { classes: [] }))
        .then((data) => {
            studentClasses = Array.isArray(data.classes) ? data.classes : [];
            paintClassSelect();
        })
        .catch(() => {
            studentClasses = [];
            paintClassSelect();
            // no class list is still a working exam page: private practice
        });
}

function paintClassSelect(preferId) {
    const saved = preferId || takePendingClass() ||
        localStorage.getItem(CLASS_STORAGE) || "";
    classSelect.textContent = "";

    const practice = document.createElement("option");
    practice.value = "";
    practice.textContent = "Private practice";
    classSelect.appendChild(practice);

    studentClasses.forEach((klass) => {
        const option = document.createElement("option");
        option.value = String(klass.id);
        option.textContent = `${klass.name} (${klass.language_label})`;
        classSelect.appendChild(option);
    });

    if (studentClasses.some((klass) => String(klass.id) === String(saved))) {
        classSelect.value = String(saved);
        localStorage.setItem(CLASS_STORAGE, classSelect.value);
        //written back so a class arrived at by link or by joining survives a
        //refresh: only the change handler saved it before, which a link and a
        //join both go around
    } else {
        classSelect.value = "";
        // a class the student has since left, or one that was archived, falls
        // back to private practice rather than to whichever class is first
    }
    applyClassChoice();
}

// Read once and dropped: the link picked the class for this visit, and the
// student's own last choice takes over again afterwards.
function takePendingClass() {
    let id = null;
    try {
        id = sessionStorage.getItem(PENDING_CLASS);
        sessionStorage.removeItem(PENDING_CLASS);
    } catch (ignored) {
        id = null;
    }
    return id;
}

function selectedClass() {
    const id = classSelect.value;
    return studentClasses.find((klass) => String(klass.id) === id) || null;
}

export function selectedClassId() {
    const klass = selectedClass();
    return klass ? klass.id : 0;
}

export function classLocksLanguage() {
    return Boolean(selectedClass());
    // an exam for a class is in the class's language - the server enforces
    // that anyway, so the picker shows it rather than offering a choice that
    // would be ignored
}

function applyClassChoice() {
    const klass = selectedClass();

    classHint.hidden = !klass;
    if (klass) {
        classHint.textContent =
            `Exams you sit for ${klass.name} are saved for your teacher to see. ` +
            "Choose Private practice to keep one to yourself.";
    }

    loadClassPlans(klass);

    languagesReady.then(() => {
        if (klass && languages.some((entry) => entry.id === klass.language)) {
            languageSelect.value = klass.language;
        } else if (!klass) {
            const saved = localStorage.getItem(LANGUAGE_STORAGE);
            if (saved && languages.some((entry) => entry.id === saved)) {
                languageSelect.value = saved;
            }
            // back to the student's own choice. The class's language was never
            // written to storage, so leaving a class does not overwrite it
        }
        setTranslateLanguage(translateCodeFor(languageSelect.value));
        updatePageTitle(languageSelect.value);
        languageSelect.disabled = turnState !== "idle" || Boolean(klass);
    });
    // after languagesReady: on a fresh load the class list can arrive before
    // the picker has any options to select
}

classSelect.onchange = () => {
    localStorage.setItem(CLASS_STORAGE, classSelect.value);
    applyClassChoice();
};

// ---------------------------------------------------------------------------
// the exam plan picker
// ---------------------------------------------------------------------------

function planStorageKey(klass) {
    return `examPlan:${klass.id}`;
    //per class: the plan picked for one class means nothing in another
}

function loadClassPlans(klass) {
    const request = ++plansRequest;
    if (!klass) {
        classPlans = [];
        paintPlanSelect(null);
        return;
    }
    fetch(`/api/classes/${klass.id}/plans`, { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : { plans: [] }))
        .catch(() => ({ plans: [] }))
        .then((data) => {
            if (request !== plansRequest) return;
            classPlans = Array.isArray(data.plans) ? data.plans : [];
            paintPlanSelect(klass);
        });
}

function paintPlanSelect(klass) {
    planSelect.textContent = "";
    const hasPlans = Boolean(klass) && classPlans.length > 0;
    planSelect.hidden = !hasPlans;
    planLabel.hidden = !hasPlans;
    if (!hasPlans) return;

    const standard = classPlans.find((plan) => plan.is_default);
    const first = document.createElement("option");
    first.value = "";
    first.textContent = standard ? `${standard.name} (class default)` : "Standard exam";
    planSelect.appendChild(first);
    //the first option is whatever an exam for this class runs when nothing
    //else is picked, which is also what the server does with no plan_id

    classPlans
        .filter((plan) => !plan.is_default)
        .forEach((plan) => {
            const option = document.createElement("option");
            option.value = String(plan.id);
            const minutes = plan.duration_seconds
                ? ` - ${Math.round(plan.duration_seconds / 60)} min` : "";
            option.textContent = `${plan.name}${minutes}`;
            planSelect.appendChild(option);
        });

    const saved = localStorage.getItem(planStorageKey(klass)) || "";
    planSelect.value = Array.from(planSelect.options).some((o) => o.value === saved)
        ? saved : "";
    planSelect.disabled = turnState !== "idle";
}

export function selectedPlanId() {
    return Number(planSelect.value) || 0;
}

planSelect.onchange = () => {
    const klass = selectedClass();
    if (klass) localStorage.setItem(planStorageKey(klass), planSelect.value);
};

// ---------------------------------------------------------------------------
// the join box
// ---------------------------------------------------------------------------

function wireJoinBox() {
    joinClassButton.onclick = openJoinBox;

    document.getElementById("joinCancel").onclick = closeJoinBox;
    joinOverlay.onclick = (event) => {
        if (event.target === joinOverlay) closeJoinBox();
    };

    joinForm.onsubmit = (event) => {
        event.preventDefault();
        joinWithCode(joinCodeInput.value);
    };
}

function openJoinBox() {
    joinCodeInput.value = "";
    joinError.hidden = true;
    joinSubmit.disabled = false;
    joinOverlay.hidden = false;
    joinCodeInput.focus();
}

function closeJoinBox() {
    joinOverlay.hidden = true;
}

function joinWithCode(code) {
    const trimmed = String(code || "").trim();
    if (!trimmed) {
        showJoinError("Type the code your teacher gave you.");
        return;
    }
    joinSubmit.disabled = true;

    fetch("/api/join", {
        method: "POST",
        credentials: "same-origin",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ code: trimmed }),
    })
        .then((response) =>
            response
                .json()
                .catch(() => ({}))
                .then((body) => {
                    if (!response.ok) {
                        throw new Error(body.error || "Could not join that class.");
                    }
                    return body;
                }))
        .then((body) => {
            closeJoinBox();
            localStorage.setItem(CLASS_STORAGE, String(body.class.id));
            return loadStudentClasses().then(() => {
                paintClassSelect(body.class.id);
                classHint.hidden = false;
                classHint.textContent =
                    (body.joined ? `You joined ${body.class.name}. `
                                 : `You are already in ${body.class.name}. `) +
                    classHint.textContent;
            });
        })
        .catch((error) => {
            if (joinOverlay.hidden) openJoinBox();
            joinCodeInput.value = trimmed;
            showJoinError(error.message);
            // a join link with a stale code opens the box with the code in it,
            // so the student can see what failed and correct it
        })
        .finally(() => {
            joinSubmit.disabled = false;
        });
}

function showJoinError(text) {
    joinError.textContent = text;
    joinError.hidden = false;
}
