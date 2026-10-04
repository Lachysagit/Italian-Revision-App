// My classes: the student's side of what /teacher is for a teacher. Which
// classes they are in, and every exam they have sat.
//
// Read-only but for joining: a student cannot rename a class, remove anybody or
// see another student's exam, and the server refuses all three whatever this
// page asks.
//
// Both cards are rendered by the server - /me/classes and /me/attempts - and
// swapped in by htmx, so nothing here builds markup. What is left is the join
// box, which is a form with its own validation and its own error line: local,
// unsaved state, which is the side of the line the browser keeps.
//
// An ES module. The page imports initClassesPage and hands it to initAccount,
// which calls it once it knows who is signed in.

import htmx from "/vendor/htmx.esm.js";
import { api } from "/api.js";

export function initClassesPage() {
    document.getElementById("classesMain").hidden = false;
    wireJoinBox();
    paintCards();

    const pending = takePendingJoin();
    if (pending) {
        joinWithCode(pending);
        // a join link followed to this page joins straight away, the same as it
        // does on the exam page: following it was the request
    }
}

function paintCards() {
    for (const [path, target] of [["/me/classes", "#classCardsReport"],
                                  ["/me/attempts", "#historyReport"]]) {
        htmx.ajax("GET", path, { target, source: document.querySelector(target) });
    }
    //source as well as target: given a bare selector htmx queues every request
    //against the body, so the second of these was being dropped before it was
    //sent. Each container is its own source and so has its own queue
    // two requests still, but no longer racing each other: the class names and
    // language labels in the history table are resolved by the server from the
    // same query, so neither card has to wait for the other or be filled in
    // twice. paintHistoryClassNames existed only to patch that up
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

    api("POST", "/api/join", { code }, "that code could not be used")
        .then(() => {
            closeJoinBox();
            paintCards();
            // both cards, not just the classes one: an exam sat for a class the
            // student is rejoining gets its name back in the history table
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
