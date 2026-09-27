// The class list: a grid of cards, one per class this account belongs to, each
// a link through to the practice exam.
//
// Deliberately thin. The cards carry no state of their own and nothing on this
// page is live, so a fresh /api/classes on every load is both simpler and more
// honest than anything cached - a teacher who has just created a class arrives
// here by a redirect and must see it.
//
// Loaded with a plain <script src> after account.js, which calls loadClasses()
// from its onReady. Load order is the only dependency.

const SUBJECT_LABELS = {
    beginners: "Beginners",
    continuers: "Continuers",
    advanced: "Advanced",
    extension: "Extension",
};

function loadClasses(user) {
    const grid = document.getElementById("classGrid");
    if (!grid) return Promise.resolve(null);

    return fetch("/api/classes", { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : null))
        .then((body) => {
            const classes = body && Array.isArray(body.classes)
                ? body.classes
                : null;
            if (!classes) {
                showClassesMessage("Your classes could not be loaded. " +
                    "Please refresh the page.");
                return null;
            }
            paintClasses(classes, user);
            return classes;
        })
        .catch(() => {
            showClassesMessage("Your classes could not be loaded. " +
                "Please refresh the page.");
            return null;
        });
}

function paintClasses(classes, user) {
    const grid = document.getElementById("classGrid");
    grid.textContent = "";

    if (classes.length === 0) {
        showClassesMessage(user && user.is_teacher
            ? "You have not created a class yet."
            : "You are not in a class yet. Your teacher will add you to one.");
        return;
        // the two audiences have different next steps, and a single "no
        // classes" line would leave one of them waiting on the other's
    }

    classes.forEach((item) => {
        grid.appendChild(buildClassCard(item));
    });
}

// One card. An <a> rather than a div with a click handler, so it opens in a new
// tab on a middle click and reads as a link to a screen reader.
function buildClassCard(item) {
    const card = document.createElement("a");
    card.className = "classCard";
    card.href = "/";
    // through to the practice exam, which is the only thing a class leads to
    // for now. The id is not in the URL yet because nothing downstream reads it

    const name = document.createElement("span");
    name.className = "classCardName";
    name.textContent = item.name;
    card.appendChild(name);

    const detail = document.createElement("span");
    detail.className = "classCardDetail";
    detail.textContent = classDetailLine(item);
    card.appendChild(detail);

    const foot = document.createElement("span");
    foot.className = "classCardFoot";
    foot.textContent = item.role === "teacher"
        ? memberCountLabel(item.member_count)
        : "Enrolled";
    card.appendChild(foot);
    // a student is told they are in it; a teacher is told how many are, which
    // is the number they actually came to look at

    const enter = document.createElement("span");
    enter.className = "classCardEnter";
    enter.textContent = "Enter";
    card.appendChild(enter);

    return card;
}

function classDetailLine(item) {
    const parts = [];
    if (item.year_level) parts.push("Year " + item.year_level);
    if (item.subject_level) {
        parts.push(SUBJECT_LABELS[item.subject_level] || item.subject_level);
    }
    if (item.language_id) parts.push(titleCase(item.language_id));
    return parts.join(" · ");
    // whatever is set, in a fixed order. A class row carrying a blank field is
    // possible - the columns default to '' - and must not render a stray dot
}

function memberCountLabel(count) {
    const total = Number(count) || 0;
    const students = Math.max(total - 1, 0);
    // the teacher is a member of their own class, and is not one of the
    // students they are counting
    if (students === 0) return "No students yet";
    return students === 1 ? "1 student" : students + " students";
}

function titleCase(text) {
    if (!text) return "";
    return text.charAt(0).toUpperCase() + text.slice(1);
    // language_id is a lowercase directory name ("italian"), and the label the
    // picker shows lives on the server. This is enough for a card
}

function showClassesMessage(message) {
    const grid = document.getElementById("classGrid");
    if (!grid) return;
    grid.textContent = "";

    const line = document.createElement("p");
    line.className = "classesMsg";
    line.textContent = message;
    grid.appendChild(line);
}
