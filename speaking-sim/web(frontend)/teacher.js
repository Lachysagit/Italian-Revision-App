// Teacher dashboard: classes, join codes, rosters and the exams sat for each
// class. Everything here is read from and written to /api/classes*, and the
// server decides what a teacher may see - this page only lays it out.
//
// Every piece of text that came from the server is written with textContent,
// never innerHTML: names and emails are typed by students and teachers, and a
// name is not allowed to become markup on somebody else's screen.
//
// An ES module. The page imports initTeacher and hands it to initAccount,
// which calls it once it knows who is signed in.

import htmx from "/vendor/htmx.esm.js";

let teacherLanguages = [];
let classes = [];
let currentClass = null;
//the class whose panel is open, as the detail route returned it
let preferredLanguage = "italian";
//what the new-class picker starts on: the teacher's own sign-up choice when
//there is one. The list arrives in the registry's alphabetical order, so the
//browser's default would otherwise be German for everyone
let currentPlans = [];
//the open class's exam plans, in full: the editor opens one from here
let currentOptions = null;
//topics and tense names for the open class's language, from /api/exam-options
const optionsByLanguage = {};

const PLACEMENTS = [
    ["any", "Whenever it fits"],
    ["with_topic", "While its topic is running"],
    ["opening", "As the opening question"],
];
const LENGTHS = [0, 60, 120, 180, 240, 300, 360, 480, 600];
//0 keeps the server's own exam length. The rest are filtered against the
//bounds /api/exam-options reports, so this list may offer more than a given
//server accepts and never less

function examOptions(language) {
    if (optionsByLanguage[language]) {
        return Promise.resolve(optionsByLanguage[language]);
    }
    return api("GET", `/api/exam-options?language=${encodeURIComponent(language)}`)
        .then((options) => {
            optionsByLanguage[language] = options;
            return options;
        });
}

function capitaliseTopic(group) {
    return group ? group.charAt(0).toUpperCase() + group.slice(1) : "";
}

function formatLength(seconds) {
    if (!seconds) {
        const standard = currentOptions ? currentOptions.default_duration_seconds : 300;
        return `Standard (${shortLength(standard)})`;
    }
    return shortLength(seconds);
}

// The menu's own form. Exact for the same reason describeLength is: a length
// saved through the API need not be a round number of minutes, and the menu
// now keeps such a length as an option of its own rather than rounding it away.
function shortLength(seconds) {
    if (seconds % 60 === 0) return `${seconds / 60} min`;
    return `${Math.floor(seconds / 60)} min ${seconds % 60} s`;
}

export function initTeacher(user) {
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

// ---------------------------------------------------------------------------
// routing: #class-12 opens a class, #class-12/attempt-34 one of its exams
// ---------------------------------------------------------------------------

function openFromHash() {
    const match = /^#class-(\d+)(?:\/(attempt|plan)-(\d+|new))?$/
        .exec(window.location.hash);
    if (!match) {
        showPanel(null);
        return;
    }
    const classId = Number(match[1]);
    const kind = match[2];
    const target = match[3];

    const ready = currentClass && currentClass.id === classId
        ? Promise.resolve()
        : openClass(classId);
    ready.then(() => {
        if (!currentClass) return;
        if (kind === "attempt") {
            openAttempt(Number(target));
        } else if (kind === "plan") {
            openPlanEditor(target === "new" ? null : Number(target));
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
    document.getElementById("planPanel").hidden = which !== "plan";
    document.getElementById("noClassSelected").hidden = which !== null;
}

// ---------------------------------------------------------------------------
// one class
// ---------------------------------------------------------------------------

function openClass(classId) {
    return Promise.all([
        api("GET", `/api/classes/${classId}`),
        api("GET", `/api/classes/${classId}/plans`),
        //still fetched as data, and no longer for the list beside it: the plan
        //editor opens a plan out of currentPlans, which is the one part of this
        //page that needs a plan in full rather than a line about it
    ])
        .then(([detail, plans]) =>
            examOptions(detail.class.language).then((options) => {
                currentClass = detail.class;
                currentPlans = plans.plans;
                currentOptions = options;
                paintClass(detail);
                paintReports(classId);
                paintClassList();
            }))
        // the options come after the class, because which tense names to
        // show depends on the class's language
        .catch((error) => {
            currentClass = null;
            showPanel(null);
            showTeacherNotice(error.message);
        });
}

document.body.addEventListener("class-changed", () => {
    refreshClass();
});
//sent as HX-Trigger by the routes that change a class. The card that asked for
//the change has already been replaced by the answer; this is for the ones that
//went stale beside it - the coverage report, the exam list, the sidebar count

document.body.addEventListener("htmx:responseError", (event) => {
    let message = "That did not work. Reload the page and try again.";
    try {
        message = JSON.parse(event.detail.xhr.responseText).error || message;
    } catch (error) {
        //a refusal that is not JSON, so the fixed sentence above stands
    }
    showTeacherNotice(message);
});
//htmx leaves the page alone on a 4xx or 5xx, which is what should happen - but
//it says nothing either, and a Remove that silently did nothing is worse than
//one that explains itself. The server's own wording is reused where there is one

function refreshClass() {
    if (!currentClass) return Promise.resolve();
    const id = currentClass.id;
    return Promise.all([openClass(id), loadClasses()]);
    // the list is reloaded too: student counts and the archived flag show there
}

function paintClassList() {
    const archived = document.getElementById("showArchived").checked ? "1" : "0";
    const current = currentClass ? currentClass.id : 0;
    htmx.ajax("GET",
        `/teacher/classes?archived=${archived}&current=${current}`,
        "#classListReport");
    //the filter and the open class go up as parameters. They are the only two
    //things the sidebar needs that the server could not already know, and
    //sending them is cheaper than keeping a copy of the class list here to
    //re-filter
}

function paintReports(classId) {
    for (const [path, target] of [
        ["members", "#membersReport"],
        ["attempts", "#attemptsReport"],
        ["coverage", "#coverageReport"],
        ["header", "#classHeaderReport"],
        ["join-code", "#joinCodeReport"],
        ["invites", "#invitesReport"],
        ["plans", "#plansReport"],
    ]) {
        htmx.ajax("GET", `/teacher/classes/${classId}/${path}`, target);
    }
    //deliberately outside openClass's promise chain: a card that fails to load
    //leaves the panel one card short rather than blanking a class that loaded
    //fine. Each is a whole region the server owns now, so there is nothing to
    //keep in step with it here beyond the id its answer lands in
}

function paintClass(detail) {
    const klass = detail.class;

    for (const id of ["inviteButton", "inviteEmails"]) {
        document.getElementById(id).disabled = klass.archived;
    }
    //the only two controls left on this page that the server does not render.
    //The buttons inside the fragments carry their own disabled state

    document.getElementById("newPlanLink").href = `#class-${klass.id}/plan-new`;
    document.getElementById("newPlanLink").hidden = klass.archived;
    //on the card head rather than in the list, so it stays put while the list
    //below it is replaced

    setStatus("joinStatus", "");
    setStatus("inviteStatus", "");
}

// ---------------------------------------------------------------------------
// exam plans and coverage
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// the plan editor
// ---------------------------------------------------------------------------

let editingPlan = null;
//the plan open in the editor, or null for a new one

function openPlanEditor(planId) {
    editingPlan = planId ? currentPlans.find((plan) => plan.id === planId) : null;
    if (planId && !editingPlan) {
        showTeacherNotice("That exam plan no longer exists.");
        window.location.hash = `#class-${currentClass.id}`;
        return;
    }
    const plan = editingPlan || {
        name: "", duration_seconds: 0, is_default: currentPlans.every((p) => p.archived),
        visible: true, require_opinion: true, paraphrase_ok: false,
        topics: [], questions: [], tenses: [],
    };
    //a class's first plan is ticked as its default, since a plan nobody sits
    //by default is rarely what a first plan is for

    document.getElementById("planTitle").textContent =
        editingPlan ? `Edit: ${plan.name}` : `New exam plan for ${currentClass.name}`;
    document.getElementById("planName").value = plan.name;
    document.getElementById("planDefault").checked = plan.is_default;
    document.getElementById("planVisible").checked = plan.visible;
    document.getElementById("planOpinion").checked = plan.require_opinion;
    document.getElementById("planParaphrase").checked = plan.paraphrase_ok;
    document.getElementById("planArchive").hidden = !editingPlan;

    paintDuration(plan.duration_seconds);

    const topics = document.getElementById("planTopics");
    topics.textContent = "";
    currentOptions.topics.forEach((group) => {
        const label = element("label", "inlineCheck");
        const box = element("input");
        box.type = "checkbox";
        box.value = group;
        box.checked = plan.topics.includes(group);
        box.onchange = refreshQuestionTopics;
        label.appendChild(box);
        label.appendChild(document.createTextNode(` ${capitaliseTopic(group)}`));
        topics.appendChild(label);
    });

    const questions = document.getElementById("planQuestions");
    questions.textContent = "";
    plan.questions.forEach((question) => addQuestionRow(question));

    const tenses = document.getElementById("planTenses");
    tenses.textContent = "";
    currentOptions.tenses.forEach((tense) => {
        const target = plan.tenses.find((t) => t.tense === tense.key);
        const row = element("div", "tenseRow");
        const label = element("label", "inlineCheck");
        const box = element("input");
        box.type = "checkbox";
        box.value = tense.key;
        box.checked = Boolean(target);
        label.appendChild(box);
        label.appendChild(document.createTextNode(` ${tense.label}`));
        row.appendChild(label);

        const count = element("input", "tenseCount");
        count.type = "number";
        count.min = "1";
        count.max = "5";
        count.value = String(target ? target.min_count : 1);
        count.setAttribute("aria-label", `How many questions in the ${tense.label}`);
        count.disabled = !box.checked;
        box.onchange = () => {
            count.disabled = !box.checked;
        };
        //a count for a tense that is not being practised means nothing, so it
        //only takes input once its box is ticked
        row.appendChild(count);
        row.appendChild(element("span", "muted", "times at least"));
        tenses.appendChild(row);
    });

    setStatus("planStatus", "");
    refreshQuestionTopics();
    showPanel("plan");
    window.scrollTo(0, 0);
}

// The lengths this server will accept, in the order they are offered. Standard
// is always there; the rest are the offered lengths inside the server's bounds,
// so the menu cannot put up a length that Save would then refuse.
function allowedLengths() {
    const min = currentOptions.min_duration_seconds;
    const max = currentOptions.max_duration_seconds;
    return LENGTHS.filter((seconds) =>
        seconds === 0 || (seconds >= min && seconds <= max));
}

// Fill the length menu and choose `stored`. A plan saved before the bounds
// narrowed can hold a length no longer allowed, and one saved through the API
// can hold a length that is allowed but not on the menu; both used to fall
// through to "Standard" silently, which quietly rewrote the exam's length the
// next time anybody saved an unrelated edit.
function paintDuration(stored) {
    const select = document.getElementById("planDuration");
    const note = document.getElementById("planDurationNote");
    const offered = allowedLengths();
    const min = currentOptions.min_duration_seconds;
    const max = currentOptions.max_duration_seconds;

    let chosen = stored > 0 ? stored : 0;
    let message = "";

    if (chosen > 0 && (chosen < min || chosen > max)) {
        const bound = chosen < min ? min : max;
        message = `This exam was saved as ${describeLength(stored)}, outside the ` +
            `${describeLength(min)} to ${describeLength(max)} an exam may now run. ` +
            `It is set to ${describeLength(bound)} below - save to confirm.`;
        chosen = bound;
        //snapped to the bound rather than dropped to Standard: the teacher
        //asked for a long exam, and the longest allowed is the nearest thing
        //to it. Saying so is the point - the length is changing either way,
        //and an exam whose length changed without a word is the bug
    } else if (chosen > 0 && !offered.includes(chosen)) {
        offered.push(chosen);
        offered.sort((a, b) => a - b);
        //allowed, just not one of the menu's round numbers. Kept as its own
        //option so saving an unrelated edit does not round it away
    }

    select.textContent = "";
    offered.forEach((seconds) => {
        const option = element("option", null, formatLength(seconds));
        option.value = String(seconds);
        select.appendChild(option);
    });
    select.value = String(chosen);

    note.textContent = message;
    note.hidden = !message;
}

// An exact reading of a length, for a sentence rather than the menu's "10 min".
// Exact, not rounded: a stored 30 seconds described as "1 minute" made the note
// about it read as a quarrel with itself, since the minimum is also 1 minute.
function describeLength(seconds) {
    if (seconds < 60) {
        return `${seconds} ${seconds === 1 ? "second" : "seconds"}`;
    }
    const minutes = Math.floor(seconds / 60);
    const rest = seconds % 60;
    const whole = `${minutes} ${minutes === 1 ? "minute" : "minutes"}`;
    return rest ? `${whole} ${rest}s` : whole;
}

function tickedTopics() {
    return Array.from(document.querySelectorAll("#planTopics input:checked"))
        .map((box) => box.value);
}

function addQuestionRow(question) {
    const row = element("div", "questionRow");

    const text = element("input", "questionText");
    text.type = "text";
    text.maxLength = 300;
    text.placeholder = "e.g. Dove sei andato in vacanza l'anno scorso?";
    text.value = question ? question.text : "";
    text.setAttribute("aria-label", "Question");
    row.appendChild(text);

    const topic = element("select", "questionTopic");
    topic.setAttribute("aria-label", "Topic");
    topic.dataset.wanted = question ? question.topic_group : "";
    row.appendChild(topic);

    const placement = element("select", "questionPlacement");
    placement.setAttribute("aria-label", "When to ask it");
    PLACEMENTS.forEach(([value, label]) => {
        const option = element("option", null, label);
        option.value = value;
        placement.appendChild(option);
    });
    placement.value = question ? question.placement : "any";
    row.appendChild(placement);

    const remove = element("button", "quiet small", "Remove");
    remove.type = "button";
    remove.onclick = () => {
        row.remove();
        paintFit();
    };
    row.appendChild(remove);

    document.getElementById("planQuestions").appendChild(row);
    refreshQuestionTopics();
}

// The topic menus on the question rows offer only the ticked topics, since the
// server refuses a set question about a topic the exam does not cover.
function refreshQuestionTopics() {
    const ticked = tickedTopics();
    const offered = ticked.length ? ticked : currentOptions.topics;
    document.querySelectorAll("#planQuestions .questionTopic").forEach((select) => {
        const wanted = select.value || select.dataset.wanted || "";
        select.textContent = "";
        const any = element("option", null, "Any topic");
        any.value = "";
        select.appendChild(any);
        offered.forEach((group) => {
            const option = element("option", null, capitaliseTopic(group));
            option.value = group;
            select.appendChild(option);
        });
        select.value = offered.includes(wanted) ? wanted : "";
        select.dataset.wanted = "";
    });
    paintFit();
}

function paintFit() {
    const chosen = Number(document.getElementById("planDuration").value) ||
        currentOptions.default_duration_seconds;
    const room = Math.max(0, Math.floor(chosen / currentOptions.seconds_per_question) - 1);
    const count = document.querySelectorAll("#planQuestions .questionRow").length;
    const fit = document.getElementById("planFit");
    fit.textContent = `A ${Math.round(chosen / 60)}-minute exam has room for ` +
        `${room} set ${room === 1 ? "question" : "questions"}` +
        (count > room ? ` - you have ${count}.` : ".");
    //the singular is reachable now that an exam can be one minute long: a
    //minute fits two questions, one of which is left for the examiner's own
    fit.classList.toggle("error", count > room);
    // the same sum the server checks on save, shown while typing so the
    // refusal is never a surprise
}

function readPlanForm() {
    const questions = Array.from(document.querySelectorAll("#planQuestions .questionRow"))
        .map((row) => ({
            text: row.querySelector(".questionText").value.trim(),
            topic_group: row.querySelector(".questionTopic").value,
            placement: row.querySelector(".questionPlacement").value,
        }))
        .filter((question) => question.text);

    const tenses = Array.from(document.querySelectorAll("#planTenses .tenseRow"))
        .filter((row) => row.querySelector("input[type=checkbox]").checked)
        .map((row) => ({
            tense: row.querySelector("input[type=checkbox]").value,
            min_count: Number(row.querySelector(".tenseCount").value) || 1,
        }));

    return {
        name: document.getElementById("planName").value.trim(),
        duration_seconds: Number(document.getElementById("planDuration").value),
        is_default: document.getElementById("planDefault").checked,
        visible: document.getElementById("planVisible").checked,
        require_opinion: document.getElementById("planOpinion").checked,
        paraphrase_ok: document.getElementById("planParaphrase").checked,
        topics: tickedTopics(),
        questions,
        tenses,
    };
}

function savePlan(event) {
    event.preventDefault();
    const button = document.getElementById("planSave");
    button.disabled = true;
    const body = readPlanForm();
    const request = editingPlan
        ? api("PUT", `/api/plans/${editingPlan.id}`, body)
        : api("POST", `/api/classes/${currentClass.id}/plans`, body);

    request
        .then(() => refreshClass())
        .then(() => {
            window.location.hash = `#class-${currentClass.id}`;
        })
        .catch((error) => setStatus("planStatus", error.message, true))
        .finally(() => {
            button.disabled = false;
        });
}

function archivePlan() {
    if (!editingPlan || !window.confirm(
        `Archive ${editingPlan.name}? Students will stop seeing it, and if it ` +
        "is the class default, exams go back to covering the whole syllabus.")) {
        return;
    }
    api("POST", `/api/plans/${editingPlan.id}/archive`)
        .then(() => refreshClass())
        .then(() => {
            window.location.hash = `#class-${currentClass.id}`;
        })
        .catch((error) => setStatus("planStatus", error.message, true));
}

// ---------------------------------------------------------------------------
// one exam
// ---------------------------------------------------------------------------

function openAttempt(attemptId) {
    const body = document.getElementById("attemptBody");
    body.textContent = "";
    //cleared first, so the check below is about this exam rather than the last
    //one, and so a refused request cannot leave the previous transcript on
    //screen under a new heading

    htmx.ajax("GET", `/teacher/attempts/${attemptId}`, "#attemptBody").then(() => {
        if (!body.firstElementChild) {
            window.location.hash = `#class-${currentClass ? currentClass.id : ""}`;
            return;
            //nothing was swapped in, so the request was refused: htmx leaves a
            //4xx alone and the responseError listener has already said why
        }
        showPanel("attempt");
        window.scrollTo(0, 0);
    });
}

// Why a set question got the verdict it did, in a teacher's terms. The scores
// behind it are for tuning the thresholds and stay out of the sentence; what a
// teacher needs to know is whether their words were used, and how close the
// exam came when they were not.
// ---------------------------------------------------------------------------
// the buttons
// ---------------------------------------------------------------------------

function wireDashboard() {
    document.getElementById("showArchived").onchange = paintClassList;

    document.getElementById("planForm").onsubmit = savePlan;
    document.getElementById("planArchive").onclick = archivePlan;
    document.getElementById("addQuestion").onclick = () => addQuestionRow(null);
    document.getElementById("planDuration").onchange = paintFit;
    document.getElementById("planBack").onclick = () => {
        window.location.hash = `#class-${currentClass.id}`;
    };

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

    document.getElementById("joinCodeReport").onclick = (event) => {
        const button = event.target.closest("[data-join-code]");
        if (!button) return;

        const link = `${window.location.origin}/join/${button.dataset.joinCode}`;
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
    //one listener on the card rather than on the button, because the button is
    //replaced every time the code changes. The code comes down in a data
    //attribute and the link is built here, from the address the teacher
    //actually has open - which is the one that will work when they paste it

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
