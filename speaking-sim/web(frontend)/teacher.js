// Teacher dashboard: classes, join codes, rosters and the exams sat for each
// class. Everything here is read from and written to /api/classes*, and the
// server decides what a teacher may see - this page only lays it out.
//
// Every piece of text that came from the server is written with textContent,
// never innerHTML: names and emails are typed by students and teachers, and a
// name is not allowed to become markup on somebody else's screen.
//
// Loaded with a plain <script src> after account.js, which calls initTeacher
// once it knows who is signed in.

const END_REASONS = {
    student_end: "Ended by student",
    timer: "Time ran out",
    disconnect: "Left before the end",
    crash: "Server restarted",
};

let teacherLanguages = [];
let classes = [];
let currentClass = null;
//the class whose panel is open, as the detail route returned it
let preferredLanguage = "italian";
//what the new-class picker starts on: the teacher's own sign-up choice when
//there is one. The list arrives in the registry's alphabetical order, so the
//browser's default would otherwise be German for everyone

function initTeacher(user) {
    if (!user.is_teacher) {
        showTeacherNotice(
            "This page is for teachers. If you teach a class, ask whoever runs " +
            "this site to add your email address as a teacher.");
        return;
        // the server refuses every teacher route for this account anyway; the
        // notice is only so the page says why it is empty
    }

    document.getElementById("dashboard").hidden = false;
    if (user.preferred_language) {
        preferredLanguage = user.preferred_language;
    }
    wireDashboard();

    Promise.all([loadTeacherLanguages(), loadClasses()]).then(() => {
        openFromHash();
    });
    window.addEventListener("hashchange", openFromHash);
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

function api(method, path, body) {
    const options = {
        method,
        credentials: "same-origin",
        headers: {},
    };
    if (body !== undefined) {
        options.headers["Content-Type"] = "application/json";
        options.body = JSON.stringify(body);
    }
    return fetch(path, options).then((response) =>
        response
            .json()
            .catch(() => ({}))
            .then((data) => {
                if (!response.ok) {
                    throw new Error(data.error || `request failed (${response.status})`);
                }
                return data;
            }));
    // every route answers JSON, errors included, so one reader serves them all
}

function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = text;
    return node;
}

function formatTime(seconds) {
    if (!seconds) return "-";
    return new Date(seconds * 1000).toLocaleString("en-AU", {
        dateStyle: "medium",
        timeStyle: "short",
    });
}

function formatCode(code) {
    return code.length === 8 ? `${code.slice(0, 4)}-${code.slice(4)}` : code;
    // shown in two halves because it is read off a board; the server ignores
    // the hyphen when a student types it back
}

function capitalise(text) {
    return text ? text.charAt(0).toUpperCase() + text.slice(1) : "";
}

function showTeacherNotice(text) {
    const notice = document.getElementById("teacherNotice");
    notice.textContent = text;
    notice.hidden = false;
}

function setStatus(id, text, isError) {
    const status = document.getElementById(id);
    status.textContent = text;
    status.classList.toggle("error", Boolean(isError));
}

// ---------------------------------------------------------------------------
// the class list
// ---------------------------------------------------------------------------

function loadTeacherLanguages() {
    return fetch("/api/languages")
        .then((response) => (response.ok ? response.json() : []))
        .then((list) => {
            teacherLanguages = Array.isArray(list) ? list : [];
            const select = document.getElementById("newClassLanguage");
            select.textContent = "";
            teacherLanguages.forEach((language) => {
                const option = element("option", null, language.label);
                option.value = language.id;
                select.appendChild(option);
            });
            if (teacherLanguages.some((language) => language.id === preferredLanguage)) {
                select.value = preferredLanguage;
            }
        })
        .catch(() => {
            showTeacherNotice("Could not load the language list. Please refresh the page.");
        });
}

function loadClasses() {
    return api("GET", "/api/classes")
        .then((data) => {
            classes = data.classes.filter((entry) => entry.role === "teacher");
            // the list route also returns classes this account sits in as a
            // student; the dashboard is only about the ones it teaches
            paintClassList();
        })
        .catch((error) => showTeacherNotice(error.message));
}

function paintClassList() {
    const list = document.getElementById("classList");
    const showArchived = document.getElementById("showArchived").checked;
    list.textContent = "";

    const visible = classes.filter((entry) => showArchived || !entry.archived);
    visible.forEach((entry) => {
        const item = element("li");
        const link = element("a", "classLink");
        link.href = `#class-${entry.id}`;
        link.appendChild(element("span", "classLinkName", entry.name));
        link.appendChild(element("span", "classLinkMeta",
            `${entry.language_label} · ${entry.student_count} ` +
            (entry.student_count === 1 ? "student" : "students") +
            (entry.archived ? " · archived" : "")));
        if (currentClass && currentClass.id === entry.id) {
            link.classList.add("current");
        }
        item.appendChild(link);
        list.appendChild(item);
    });

    document.getElementById("classListEmpty").hidden = classes.length > 0;
}

// ---------------------------------------------------------------------------
// routing: #class-12 opens a class, #class-12/attempt-34 one of its exams
// ---------------------------------------------------------------------------

function openFromHash() {
    const match = /^#class-(\d+)(?:\/attempt-(\d+))?$/.exec(window.location.hash);
    if (!match) {
        showPanel(null);
        return;
    }
    const classId = Number(match[1]);
    const attemptId = match[2] ? Number(match[2]) : null;

    const ready = currentClass && currentClass.id === classId
        ? Promise.resolve()
        : openClass(classId);
    ready.then(() => {
        if (attemptId) {
            openAttempt(attemptId);
        } else {
            showPanel("class");
        }
    });
    // the hash rather than in-page state, so the back button, a refresh and a
    // bookmarked exam all land where the teacher was
}

function showPanel(which) {
    document.getElementById("classPanel").hidden = which !== "class";
    document.getElementById("attemptPanel").hidden = which !== "attempt";
    document.getElementById("noClassSelected").hidden = which !== null;
}

// ---------------------------------------------------------------------------
// one class
// ---------------------------------------------------------------------------

function openClass(classId) {
    return Promise.all([
        api("GET", `/api/classes/${classId}`),
        api("GET", `/api/classes/${classId}/attempts`),
    ])
        .then(([detail, history]) => {
            currentClass = detail.class;
            paintClass(detail, history.attempts);
            paintClassList();
        })
        .catch((error) => {
            currentClass = null;
            showPanel(null);
            showTeacherNotice(error.message);
        });
}

function refreshClass() {
    if (!currentClass) return Promise.resolve();
    const id = currentClass.id;
    return Promise.all([openClass(id), loadClasses()]);
    // the list is reloaded too: student counts and the archived flag show there
}

function paintClass(detail, attempts) {
    const klass = detail.class;
    document.getElementById("className").textContent = klass.name;
    document.getElementById("classMeta").textContent =
        `${klass.language_label} · created ${formatTime(klass.created_at)}`;

    document.getElementById("archiveButton").textContent =
        klass.archived ? "Restore class" : "Archive class";
    document.getElementById("archivedBanner").hidden = !klass.archived;

    for (const id of ["copyJoinLink", "rotateCode", "disableCode",
        "inviteButton", "inviteEmails"]) {
        document.getElementById(id).disabled = klass.archived;
    }

    paintJoinCode(klass.join_code);
    paintPending(detail.invites);
    paintMembers(detail.members);
    paintAttempts(attempts);

    setStatus("joinStatus", "");
    setStatus("inviteStatus", "");
}

function paintJoinCode(code) {
    const hasCode = Boolean(code);
    document.getElementById("joinCode").hidden = !hasCode;
    document.getElementById("joinCode").textContent = hasCode ? formatCode(code) : "";
    document.getElementById("joinCodeOff").hidden = hasCode;
    document.getElementById("copyJoinLink").hidden = !hasCode;
    document.getElementById("disableCode").hidden = !hasCode;
    document.getElementById("rotateCode").textContent =
        hasCode ? "New code" : "Turn on joining";
    // with joining off, a new code is how it comes back on, so the one button
    // says so rather than offering to replace a code that does not exist
}

function joinLink(code) {
    return `${window.location.origin}/join/${code}`;
}

function paintPending(invites) {
    const list = document.getElementById("pendingInvites");
    list.textContent = "";
    invites.forEach((invite) => {
        const item = element("li");
        item.appendChild(element("span", null, invite.email));
        const revoke = element("button", "quiet small", "Cancel");
        revoke.type = "button";
        revoke.onclick = () => {
            api("DELETE", `/api/classes/${currentClass.id}/invites/${invite.id}`)
                .then(refreshClass)
                .catch((error) => setStatus("inviteStatus", error.message, true));
        };
        item.appendChild(revoke);
        list.appendChild(item);
    });
    document.getElementById("pendingBlock").hidden = invites.length === 0;
}

function paintMembers(members) {
    const body = document.querySelector("#membersTable tbody");
    body.textContent = "";

    const students = members.filter((member) => member.role === "student");
    students.forEach((member) => {
        const row = element("tr");
        row.appendChild(element("td", null, member.name || "(no name)"));
        row.appendChild(element("td", "muted", member.email));
        row.appendChild(element("td", null, member.year_level || "-"));
        row.appendChild(element("td", null, capitalise(member.subject_level) || "-"));
        row.appendChild(element("td", "number", String(member.attempt_count)));
        row.appendChild(element("td", null, formatTime(member.last_attempt_at)));

        const actions = element("td", "actions");
        const remove = element("button", "quiet small", "Remove");
        remove.type = "button";
        remove.onclick = () => removeMember(member);
        actions.appendChild(remove);
        row.appendChild(actions);

        body.appendChild(row);
    });

    document.getElementById("membersEmpty").hidden = students.length > 0;
    document.getElementById("membersTable").hidden = students.length === 0;
}

function removeMember(member) {
    const name = member.name || member.email;
    if (!window.confirm(
        `Remove ${name} from ${currentClass.name}? Their exams for this class ` +
        "will no longer show here.")) {
        return;
    }
    api("DELETE", `/api/classes/${currentClass.id}/members/${member.user_id}`)
        .then(refreshClass)
        .catch((error) => showTeacherNotice(error.message));
}

function paintAttempts(attempts) {
    const body = document.querySelector("#attemptsTable tbody");
    body.textContent = "";

    attempts.forEach((attempt) => {
        const row = element("tr");
        row.appendChild(element("td", null, attempt.student_name || attempt.student_email));
        row.appendChild(element("td", null, formatTime(attempt.started_at)));
        row.appendChild(element("td", "number", String(attempt.turn_count)));
        row.appendChild(element("td", null, endLabel(attempt)));

        const open = element("td", "actions");
        const link = element("a", null, "View");
        link.href = `#class-${currentClass.id}/attempt-${attempt.id}`;
        open.appendChild(link);
        row.appendChild(open);

        body.appendChild(row);
    });

    document.getElementById("attemptsEmpty").hidden = attempts.length > 0;
    document.getElementById("attemptsTable").hidden = attempts.length === 0;
}

function endLabel(attempt) {
    if (!attempt.ended_at) return "In progress";
    return END_REASONS[attempt.end_reason] || attempt.end_reason || "Ended";
}

// ---------------------------------------------------------------------------
// one exam
// ---------------------------------------------------------------------------

function openAttempt(attemptId) {
    api("GET", `/api/attempts/${attemptId}`)
        .then((data) => {
            const attempt = data.attempt;
            document.getElementById("attemptTitle").textContent =
                attempt.student_name || attempt.student_email;
            document.getElementById("attemptMeta").textContent =
                `${formatTime(attempt.started_at)} · ${attempt.turn_count} turns · ` +
                endLabel(attempt);

            const container = document.getElementById("attemptTurns");
            container.textContent = "";
            data.turns.forEach((turn) => container.appendChild(turnCard(turn)));
            document.getElementById("attemptEmpty").hidden = data.turns.length > 0;

            showPanel("attempt");
            window.scrollTo(0, 0);
        })
        .catch((error) => {
            showTeacherNotice(error.message);
            window.location.hash = `#class-${currentClass ? currentClass.id : ""}`;
        });
}

function turnCard(turn) {
    const role = turn.role === "examiner" ? "examiner" : "student";
    const card = element("div", `turn ${role}`);
    // the same classes the exam page paints with, so a transcript reads the
    // same here as it did to the student

    const heading = element("div", "turn-role", role === "examiner" ? "Examiner" : "Student");
    if (turn.topic) {
        heading.appendChild(element("span", "topicTag", turn.topic));
    }
    card.appendChild(heading);
    card.appendChild(element("div", "turn-text", turn.text));
    return card;
}

// ---------------------------------------------------------------------------
// the buttons
// ---------------------------------------------------------------------------

function wireDashboard() {
    document.getElementById("showArchived").onchange = paintClassList;

    document.getElementById("newClassForm").onsubmit = (event) => {
        event.preventDefault();
        const name = document.getElementById("newClassName").value.trim();
        const language = document.getElementById("newClassLanguage").value;
        const error = document.getElementById("newClassError");
        const button = document.getElementById("newClassButton");
        error.hidden = true;
        button.disabled = true;

        api("POST", "/api/classes", { name, language })
            .then((data) => {
                document.getElementById("newClassName").value = "";
                return loadClasses().then(() => {
                    window.location.hash = `#class-${data.class.id}`;
                });
            })
            .catch((failure) => {
                error.textContent = failure.message;
                error.hidden = false;
            })
            .finally(() => {
                button.disabled = false;
            });
    };

    document.getElementById("archiveButton").onclick = () => {
        const archiving = !currentClass.archived;
        if (archiving && !window.confirm(
            `Archive ${currentClass.name}? Students will stop seeing it and ` +
            "nobody will be able to join. You can restore it later.")) {
            return;
        }
        api("POST", `/api/classes/${currentClass.id}/archive`, { archived: archiving })
            .then(refreshClass)
            .catch((error) => showTeacherNotice(error.message));
    };

    document.getElementById("copyJoinLink").onclick = () => {
        const link = joinLink(currentClass.join_code);
        const done = () => setStatus("joinStatus", `Copied ${link}`);
        if (navigator.clipboard) {
            navigator.clipboard.writeText(link).then(done,
                () => setStatus("joinStatus", link));
        } else {
            setStatus("joinStatus", link);
            // an http:// origin other than localhost has no clipboard API, so
            // the link is shown for copying by hand instead
        }
    };

    document.getElementById("rotateCode").onclick = () => {
        if (currentClass.join_code && !window.confirm(
            "Make a new code? The current one will stop working straight away.")) {
            return;
        }
        api("POST", `/api/classes/${currentClass.id}/join-code`, { action: "rotate" })
            .then((data) => {
                currentClass.join_code = data.join_code;
                paintJoinCode(data.join_code);
                setStatus("joinStatus", "New code ready.");
            })
            .catch((error) => setStatus("joinStatus", error.message, true));
    };

    document.getElementById("disableCode").onclick = () => {
        api("POST", `/api/classes/${currentClass.id}/join-code`, { action: "disable" })
            .then(() => {
                currentClass.join_code = "";
                paintJoinCode("");
                setStatus("joinStatus",
                    "Joining is off. Students already in the class stay in it.");
            })
            .catch((error) => setStatus("joinStatus", error.message, true));
    };

    document.getElementById("inviteButton").onclick = () => {
        const box = document.getElementById("inviteEmails");
        const button = document.getElementById("inviteButton");
        button.disabled = true;

        api("POST", `/api/classes/${currentClass.id}/invites`, { emails: box.value })
            .then((result) => {
                const parts = [];
                if (result.added) parts.push(`${result.added} added`);
                if (result.invited) parts.push(`${result.invited} will join when they sign in`);
                if (result.existing) parts.push(`${result.existing} already in the class`);
                let message = parts.join(", ") + ".";
                if (result.invalid.length) {
                    message += ` Not email addresses, so skipped: ${result.invalid.join(", ")}`;
                }
                box.value = result.invalid.join("\n");
                // the rejects are left in the box to be fixed and sent again
                return refreshClass().then(() =>
                    setStatus("inviteStatus", message, result.invalid.length > 0));
            })
            .catch((error) => setStatus("inviteStatus", error.message, true))
            .finally(() => {
                button.disabled = Boolean(currentClass && currentClass.archived);
            });
    };

    document.getElementById("attemptBack").onclick = () => {
        window.location.hash = `#class-${currentClass.id}`;
    };
}
