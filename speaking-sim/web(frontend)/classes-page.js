// My classes: the student's side of what /teacher is for a teacher. Which
// classes they are in, and every exam they have sat.
//
// Read-only but for joining: a student cannot rename a class, remove anybody or
// see another student's exam, and the server refuses all three whatever this
// page asks. It reads /api/classes and /api/my-attempts, both of which answer
// about the caller and nobody else.
//
// Every string that came from the server is written with textContent, never
// innerHTML: class names are typed by teachers, and a name is not allowed to
// become markup on a student's screen.
//
// Loaded with a plain <script src> after account.js, which calls
// initClassesPage once it knows who is signed in.

const END_REASONS = {
    student_end: "Ended by student",
    timer: "Time ran out",
    disconnect: "Left before the end",
    crash: "Server restarted",
};

let myClasses = [];
//[{id, name, language, language_label, role, student_count}] from /api/classes
let classesLoaded = false;
//whether the list above is the server's answer or just the empty start. Set
//either way, because history names its classes from it and must not call one
//"left" merely because the other request has not landed yet

function initClassesPage() {
    document.getElementById("classesMain").hidden = false;
    wireJoinBox();

    return Promise.all([loadMyClasses(), loadMyHistory()]).then(() => {
        const pending = takePendingJoin();
        if (pending) {
            joinWithCode(pending);
            // a join link followed to this page joins straight away, the same
            // as it does on the exam page: following it was the request
        }
    });
}

// A /join/CODE link redirects to /?join=CODE, so the code normally arrives on
// the exam page. It is read here too for a link pasted with this path, and is
// taken out of the address bar either way so a refresh does not rejoin.
function takePendingJoin() {
    const params = new URLSearchParams(window.location.search);
    const code = params.get("join");
    if (!code) return null;

    params.delete("join");
    const rest = params.toString();
    window.history.replaceState(null, "",
        window.location.pathname + (rest ? `?${rest}` : ""));
    return code;
}

// ---------------------------------------------------------------------------
// the classes
// ---------------------------------------------------------------------------

function loadMyClasses() {
    return fetch("/api/classes", { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : { classes: [] }))
        .then((data) => {
            myClasses = Array.isArray(data.classes) ? data.classes : [];
            classesLoaded = true;
            paintClassCards();
            paintHistoryClassNames();
        })
        .catch(() => {
            myClasses = [];
            paintClassCards();
        });
}

function paintClassCards() {
    const cards = document.getElementById("classCards");
    cards.textContent = "";
    document.getElementById("classesEmpty").hidden = myClasses.length > 0;

    myClasses.forEach((klass) => {
        cards.appendChild(buildClassCard(klass));
    });
}

function buildClassCard(klass) {
    const card = document.createElement("article");
    card.className = "classCard";

    const name = document.createElement("h3");
    name.textContent = klass.name;
    card.appendChild(name);

    const meta = document.createElement("p");
    meta.className = "classCardMeta";
    meta.textContent = classCardMeta(klass);
    card.appendChild(meta);

    const actions = document.createElement("div");
    actions.className = "classCardActions";

    if (klass.role === "teacher") {
        const manage = document.createElement("a");
        manage.href = "/teacher#class-" + encodeURIComponent(klass.id);
        manage.textContent = "Manage this class";
        actions.appendChild(manage);
        // a teacher listed in their own class is sent to the dashboard, which
        // is the only place a class can actually be changed
    } else {
        const sit = document.createElement("a");
        sit.href = "/?class=" + encodeURIComponent(klass.id);
        sit.textContent = "Sit an exam for this class";
        actions.appendChild(sit);
    }

    card.appendChild(actions);
    return card;
}

function classCardMeta(klass) {
    const parts = [];
    if (klass.language_label) parts.push(klass.language_label);
    if (klass.role === "teacher") {
        parts.push("You teach this");
    } else {
        parts.push(classmateCount(klass.student_count));
    }
    return parts.join(" · ");
    // whatever the row carries, in a fixed order: a class with no language
    // label must not render a stray separator
}

function classmateCount(count) {
    const total = Number(count) || 0;
    const others = Math.max(total - 1, 0);
    // the student reading this is one of the count, and is not their own
    // classmate
    if (others === 0) return "You are the only student";
    return others === 1 ? "1 classmate" : others + " classmates";
}

// ---------------------------------------------------------------------------
// the exam history
// ---------------------------------------------------------------------------

function loadMyHistory() {
    return fetch("/api/my-attempts", { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : { attempts: [] }))
        .then((data) => {
            paintHistory(Array.isArray(data.attempts) ? data.attempts : []);
        })
        .catch(() => paintHistory([]));
}

function paintHistory(attempts) {
    const body = document.querySelector("#historyTable tbody");
    body.textContent = "";

    document.getElementById("historyEmpty").hidden = attempts.length > 0;
    document.getElementById("historyTable").hidden = attempts.length === 0;

    attempts.forEach((attempt) => {
        const row = document.createElement("tr");
        row.dataset.classId = String(attempt.class_id || 0);
        row.dataset.language = attempt.language || "";
        addCell(row, formatStarted(attempt.started_at));
        addCell(row, classNameFor(attempt.class_id));
        addCell(row, languageLabelFor(attempt));
        addCell(row, String(attempt.turn_count || 0));
        addCell(row, attempt.ended_at
            ? (END_REASONS[attempt.end_reason] || attempt.end_reason || "Ended")
            : "Still going");
        body.appendChild(row);
    });
}

function addCell(row, text) {
    const cell = document.createElement("td");
    cell.textContent = text;
    row.appendChild(cell);
}

function classNameFor(classId) {
    if (!classId) return "Private practice";
    const klass = myClasses.find((item) => String(item.id) === String(classId));
    if (klass) return klass.name;
    return classesLoaded ? "A class you have left" : "A class";
    // an exam sat for a class the student is no longer in is still theirs to
    // see, so the row stays and says why it has no name. Before the class list
    // has landed the honest answer is neither, so the row just says "A class"
}

// The two loads run in parallel and history usually wins, so the two columns
// that read the class list are filled in again once it has landed.
function paintHistoryClassNames() {
    const rows = document.querySelectorAll("#historyTable tbody tr");
    rows.forEach((row) => {
        if (row.dataset.classId === undefined) return;
        row.children[1].textContent = classNameFor(Number(row.dataset.classId));
        row.children[2].textContent =
            languageLabelFor({ language: row.dataset.language || "" });
    });
}

function languageLabelFor(attempt) {
    const klass = myClasses.find((item) => item.language === attempt.language);
    if (klass && klass.language_label) return klass.language_label;
    if (!attempt.language) return "";
    return attempt.language.charAt(0).toUpperCase() + attempt.language.slice(1);
    // the display name if any class on this page examines that language, and
    // the id title-cased otherwise: language_id is a lowercase folder name
}

function formatStarted(seconds) {
    if (!seconds) return "";
    const when = new Date(Number(seconds) * 1000);
    if (Number.isNaN(when.getTime())) return "";
    return when.toLocaleString(undefined, {
        day: "numeric", month: "short", hour: "numeric", minute: "2-digit",
    });
}

// ---------------------------------------------------------------------------
// the join box
// ---------------------------------------------------------------------------

function wireJoinBox() {
    const overlay = document.getElementById("joinOverlay");

    document.getElementById("joinFromPage").onclick = openJoinBox;
    document.getElementById("joinCancel").onclick = closeJoinBox;
    overlay.onclick = (event) => {
        if (event.target === overlay) closeJoinBox();
    };
    document.getElementById("joinModal").onsubmit = (event) => {
        event.preventDefault();
        joinWithCode(document.getElementById("joinCodeInput").value);
    };
}

function openJoinBox() {
    document.getElementById("joinCodeInput").value = "";
    document.getElementById("joinError").hidden = true;
    document.getElementById("joinSubmit").disabled = false;
    document.getElementById("joinOverlay").hidden = false;
    document.getElementById("joinCodeInput").focus();
}

function closeJoinBox() {
    document.getElementById("joinOverlay").hidden = true;
}

function joinWithCode(raw) {
    const code = String(raw || "").trim();
    if (!code) {
        showJoinError("Type the code your teacher gave you.");
        return;
    }

    const submit = document.getElementById("joinSubmit");
    submit.disabled = true;

    fetch("/api/join", {
        method: "POST",
        credentials: "same-origin",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ code }),
    })
        .then((response) => response.json().catch(() => ({})).then((body) => {
            if (!response.ok) {
                throw new Error(body.error || "that code could not be used");
            }
            return body;
        }))
        .then(() => {
            closeJoinBox();
            return loadMyClasses();
            // reloaded rather than pushed onto the list: the joined class comes
            // back with its language label and student count already filled
        })
        .catch((error) => {
            if (document.getElementById("joinOverlay").hidden) openJoinBox();
            document.getElementById("joinCodeInput").value = code;
            showJoinError(error.message);
            // a stale join link opens the box with the code in it, so the
            // student can see what failed and correct it
        })
        .finally(() => {
            submit.disabled = false;
        });
}

function showJoinError(message) {
    const error = document.getElementById("joinError");
    error.textContent = message;
    error.hidden = false;
}
