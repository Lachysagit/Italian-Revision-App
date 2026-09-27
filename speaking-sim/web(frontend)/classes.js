// The student's side of classes on the exam page: which class an exam is sat
// for, and the box that joins a class with the code a teacher hands out.
//
// Loaded with a plain <script src> before client.js. It defines functions and
// reads its own elements; everything that touches client.js's state (the
// language picker, the turn state) runs later, from initClasses and from the
// handlers, by which time client.js has loaded too.

const CLASS_STORAGE = "examClass";
const PENDING_JOIN = "pendingJoinCode";

const classSelect = document.getElementById("classSelect");
const joinClassButton = document.getElementById("joinClass");
const classHint = document.getElementById("classHint");
const joinOverlay = document.getElementById("joinOverlay");
const joinForm = document.getElementById("joinModal");
const joinCodeInput = document.getElementById("joinCodeInput");
const joinError = document.getElementById("joinError");
const joinSubmit = document.getElementById("joinSubmit");
const myClassesButton = document.getElementById("myClasses");
const classesOverlay = document.getElementById("classesOverlay");
const classesList = document.getElementById("classesList");

let studentClasses = [];
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

function initClasses() {
    wireJoinBox();
    wireClassesPanel();
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
    const saved = preferId || localStorage.getItem(CLASS_STORAGE) || "";
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
    } else {
        classSelect.value = "";
        // a class the student has since left, or one that was archived, falls
        // back to private practice rather than to whichever class is first
    }
    applyClassChoice();
}

function selectedClass() {
    const id = classSelect.value;
    return studentClasses.find((klass) => String(klass.id) === id) || null;
}

function selectedClassId() {
    const klass = selectedClass();
    return klass ? klass.id : 0;
}

function classLocksLanguage() {
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
// the class list
// ---------------------------------------------------------------------------

function wireClassesPanel() {
    myClassesButton.onclick = openClassesPanel;
    document.getElementById("classesClose").onclick = closeClassesPanel;
    classesOverlay.onclick = (event) => {
        if (event.target === classesOverlay) closeClassesPanel();
    };

    document.getElementById("classesJoin").onclick = () => {
        closeClassesPanel();
        openJoinBox();
        // the two boxes are one flow: a student who opens the list and finds
        // the class missing is one click from the code box
    };
}

// The list a student cannot get from the picker: every class they are in, its
// language and how many others are in it. Painted from studentClasses, which
// loadStudentClasses has already fetched, so opening the box costs no request.
function openClassesPanel() {
    paintClassesList();
    classesOverlay.hidden = false;
}

function closeClassesPanel() {
    classesOverlay.hidden = true;
}

function paintClassesList() {
    classesList.textContent = "";

    if (studentClasses.length === 0) {
        const empty = document.createElement("p");
        empty.className = "classesEmpty";
        empty.textContent = "You are not in a class yet. " +
            "Join one with the code your teacher gives you, or keep " +
            "practising on your own.";
        classesList.appendChild(empty);
        return;
    }

    const selected = classSelect.value;
    studentClasses.forEach((klass) => {
        const current = String(klass.id) === String(selected);

        const row = document.createElement("div");
        row.className = "classRow";
        if (current) {
            row.classList.add("current");
            //the one exams are being sat for, marked so the list and the picker
            //above it cannot appear to disagree
        }

        const name = document.createElement("span");
        name.className = "classRowName";
        name.textContent = klass.name;
        row.appendChild(name);

        const detail = document.createElement("span");
        detail.className = "classRowDetail";
        detail.textContent = classRowDetail(klass);
        row.appendChild(detail);

        const pick = document.createElement("button");
        pick.type = "button";
        pick.className = "classRowPick";
        pick.textContent = current ? "Sitting for this" : "Sit for this";
        pick.disabled = current || classSelect.disabled;
        //classSelect.disabled is setTurnState's mid-session latch: the class
        //rode on the start message, so it cannot be changed until the exam ends
        pick.onclick = () => {
            classSelect.value = String(klass.id);
            classSelect.dispatchEvent(new Event("change"));
            closeClassesPanel();
            //through the picker's own change handler rather than around it, so
            //the saved class, the language lock and the hint all still follow
        };
        row.appendChild(pick);

        classesList.appendChild(row);
    });
}

function classRowDetail(klass) {
    const parts = [];
    if (klass.language_label) parts.push(klass.language_label);
    if (klass.role === "teacher") {
        parts.push("You teach this");
    } else {
        parts.push(classmateCount(klass.student_count));
    }
    return parts.join(" · ");
    //whatever the row carries, in a fixed order: a class with no language label
    //must not render a stray separator
}

function classmateCount(count) {
    const total = Number(count) || 0;
    const others = Math.max(total - 1, 0);
    //the student reading this is one of the count, and is not their own classmate
    if (others === 0) return "You are the only student";
    return others === 1 ? "1 classmate" : others + " classmates";
}

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
