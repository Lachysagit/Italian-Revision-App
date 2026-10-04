import { initTranslate, setTranslateLanguage } from "/translate.js";
import { savePreferredLanguage } from "/account.js";
import {
    classLocksLanguage, classSelect, joinClassButton, planSelect,
    selectedClassId, selectedPlanId,
} from "/classes.js";
//classes.js imports this file back; see the note at the top of it for why
//the cycle holds and what would break it

const transcript = document.getElementById("transcript");
const startButton = document.getElementById("start");
const doneButton = document.getElementById("done");
const pauseButton = document.getElementById("pause");
const endButton = document.getElementById("end");
const exportButton = document.getElementById("export");
const settingsButton = document.getElementById("settings");
const settingsOverlay = document.getElementById("settingsOverlay");
const settingsClose = document.getElementById("settingsClose");
const geminiKeySelect = document.getElementById("geminiKeySelect");
export const languageSelect = document.getElementById("languageSelect");
const studentName = document.getElementById("studentName");
const micOverlay = document.getElementById("micOverlay");
const micReason = document.getElementById("micReason");
const micSteps = document.getElementById("micSteps");
const micRetry = document.getElementById("micRetry");
const micDismiss = document.getElementById("micDismiss");
const examTimer = document.getElementById("examTimer");
const examLoading = document.getElementById("examLoading");
const examNotice = document.getElementById("examNotice");
const usageLine = document.getElementById("usageLine");
let speakingUsage = null;
//{speaking_limit, speaking_used, paid, paid_source} from /api/me, kept so a
//question's questions_left can repaint the line without another request
const pageTitle = document.getElementById("pageTitle");
//get references to HTML elements by their ID's
//the translate box's own elements are looked up inside translate.js

const GEMINI_KEY_STORAGE = "geminiKeyName";
//persists the picked key across page reloads, same tab only
export const LANGUAGE_STORAGE = "examLanguage";
//which exam was last taken, so the picker reopens on it. Separate from the
//listening page's own "listeningLanguage": the two pages are chosen
//independently, and sharing one key would make picking a German listening
//paper silently switch the speaking exam too
const DEFAULT_LANGUAGE = "italian";
const SAVED_TURNS_STORAGE = "savedTurns";
//sessionStorage, so a refresh mid-exam does not lose an hour of picked answers
const STUDENT_NAME_STORAGE = "studentName";
//same treatment for the name, so it is typed once rather than every session

let socket = null;
let socketOpened = false;
//whether this session's socket ever finished its handshake. A refused
//handshake - signed out, or a page on the wrong origin - closes without a
//single message, and this is the only way to tell that from a normal end
let lastServerError = "";
//the last error the server sent, read back when a "refused" status follows it
let audioContext = null;
let mediaStream = null;
let micSource = null;
let processor = null;

let playbackSampleRate = null;
//rate the server says its PCM was synthesised at, read off the text message
//that arrives just before each binary audio frame

let captureState = "idle";

let headCut = 0;
//first sample of the pending block that belongs to the student
let tailCut = 0;
//one past the last sample of the pending block that belongs to the student

let blockStartTime = 0;


let pendingStop = false;



let micReady = false;
//the capture graph now finishes building after the opening request is sent, so
//a reply can beat it. armMic records the owed turn and buildCaptureGraph takes it
let pendingArm = false;


export let turnState = "idle";
//"idle" no session; "thinking" examiner is working and the mic is muted;
//"armed" student's turn, mic live and frames streaming
//"paused" the clock and the mic are both stopped until Resume

let examDurationMs = 5 * 60 * 1000;
//the length the server last announced on an opening question. Five minutes
//until one arrives, which is also the server's own default
//one exam is five minutes of the student's time

let examDeadline = null;
//wall-clock instant the exam ends, set by the first examiner question
let examTick = null;
let examPaused = false;
let examRemaining = null;
//ms left on the clock while paused, banked when it stopped. The deadline is
//wall-clock arithmetic, so a pause cannot simply stop counting: it has to put
//the remainder aside and buy a new deadline from it on resume
let pausePending = false;
//the student pressed Pause while the examiner was working. The request in
//flight is never cancelled - it comes back, is painted and is spoken - and the
//pause takes hold at the moment the mic would otherwise be handed over
let examExpired = false;
//latched for the life of the page: once the clock has run out no further
//examiner call may be made, including by starting a fresh session

const CAPTURE_SAMPLE_RATE = 16000;
//whisper.cpp only accepts 16 kHz mono, so the mic is captured at that rate

const BLOCK_SIZE = 4096;
//the ScriptProcessor block length, used to clamp the cut points

function addLog(text) { //status or text from the server, for the operator only
    console.log(text);
}

const ROLE_LABELS = {
    examiner: "Examiner",
    student: "You",
};
//the speaker is a fixed key rather than free text so a caller cannot invent a
//role that lands unstyled, or spell one two ways and split it into two looks

let currentPair = null;
//the question the next answer belongs to, so the two can be saved together
let pairCount = 0;
//identifies a pair to the staging list, and orders the exported document

function makeCard(role, label, text) {
    const card = document.createElement("div");
    card.className = `turn ${role}`;

    const heading = document.createElement("div");
    heading.className = "turn-role";
    heading.textContent = label;

    const body = document.createElement("div");
    body.className = "turn-text";
    body.textContent = text;

    card.appendChild(heading);
    card.appendChild(body);
    return card;
}

function downloadBlob(filename, blob) { //hand one file to the browser
    const url = URL.createObjectURL(blob);

    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    link.click();
    //detached on purpose: an anchor in the page would litter the transcript

    setTimeout(() => URL.revokeObjectURL(url), 0);
    //not in the same task as click(): the download reads the blob URL on a
    //later one, and revoking here cancels the save in Firefox and Safari.
    //A tick is enough for the read to have started, and then our copy can go
}

const CRC_TABLE = (() => { //one table, built once, for the zip checksums below
    const table = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
        let value = i;
        for (let bit = 0; bit < 8; bit++) {
            value = (value & 1) ? (0xEDB88320 ^ (value >>> 1)) : (value >>> 1);
        }
        table[i] = value >>> 0;
    }
    return table;
})();

function crc32(bytes) {
    let crc = 0xFFFFFFFF;
    for (let i = 0; i < bytes.length; i++) {
        crc = CRC_TABLE[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8);
    }
    return (crc ^ 0xFFFFFFFF) >>> 0;
}

function zipStored(entries) { //a .zip of {name, bytes}, stored not compressed
    const encoder = new TextEncoder();
    const local = [];
    const central = [];
    let offset = 0;
    //where the next local header starts, which the directory has to record

    for (const entry of entries) {
        const name = encoder.encode(entry.name);
        const sum = crc32(entry.bytes);
        const size = entry.bytes.length;

        const header = new DataView(new ArrayBuffer(30));
        header.setUint32(0, 0x04034B50, true);  //local file header signature
        header.setUint16(4, 20, true);          //version needed to extract
        header.setUint16(6, 0, true);           //flags
        header.setUint16(8, 0, true);           //method 0 = stored
        header.setUint16(10, 0, true);          //mod time, left at zero
        header.setUint16(12, 0, true);          //mod date, left at zero
        header.setUint32(14, sum, true);
        header.setUint32(18, size, true);       //compressed size
        header.setUint32(22, size, true);       //uncompressed size, the same
        header.setUint16(26, name.length, true);
        header.setUint16(28, 0, true);          //no extra field
        local.push(new Uint8Array(header.buffer), name, entry.bytes);

        const record = new DataView(new ArrayBuffer(46));
        record.setUint32(0, 0x02014B50, true);  //central directory signature
        record.setUint16(4, 20, true);          //version made by
        record.setUint16(6, 20, true);          //version needed to extract
        record.setUint16(8, 0, true);
        record.setUint16(10, 0, true);
        record.setUint16(12, 0, true);
        record.setUint16(14, 0, true);
        record.setUint32(16, sum, true);
        record.setUint32(20, size, true);
        record.setUint32(24, size, true);
        record.setUint16(28, name.length, true);
        record.setUint16(30, 0, true);          //extra field length
        record.setUint16(32, 0, true);          //comment length
        record.setUint16(34, 0, true);          //disk number
        record.setUint16(36, 0, true);          //internal attributes
        record.setUint32(38, 0, true);          //external attributes
        record.setUint32(42, offset, true);     //where this entry's header sits
        central.push(new Uint8Array(record.buffer), name);

        offset += 30 + name.length + size;
    }

    const centralSize = central.reduce((total, part) => total + part.length, 0);

    const end = new DataView(new ArrayBuffer(22));
    end.setUint32(0, 0x06054B50, true);         //end of central directory
    end.setUint16(4, 0, true);                  //disk number
    end.setUint16(6, 0, true);                  //disk the directory starts on
    end.setUint16(8, entries.length, true);
    end.setUint16(10, entries.length, true);
    end.setUint32(12, centralSize, true);
    end.setUint32(16, offset, true);            //where the directory starts
    end.setUint16(20, 0, true);                 //comment length

    return new Blob([...local, ...central, new Uint8Array(end.buffer)],
                    { type: DOCX_MIME });
}

const DOCX_MIME =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document";

function escapeXml(text) {
    return text
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;");
        //& first, or the other two get escaped twice
}

function docxBlob(paragraphs) { //the smallest package Word opens as a .docx
    const encoder = new TextEncoder();

    const body = paragraphs.map((paragraph) => {
        const runs = paragraph.text.split("\n").map((line, index) => {
            const brk = index === 0 ? "" : "<w:br/>";
            //a newline inside an answer breaks the line, not the paragraph
            return `<w:r>${paragraph.bold ? "<w:rPr><w:b/></w:rPr>" : ""}` +
                   `${brk}<w:t xml:space="preserve">${escapeXml(line)}</w:t></w:r>`;
        }).join("");
        return `<w:p>${runs}</w:p>`;
    }).join("");

    const documentXml =
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>' +
        '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">' +
        `<w:body>${body}</w:body></w:document>`;

    const contentTypes =
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>' +
        '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">' +
        '<Default Extension="xml" ContentType="application/xml"/>' +
        '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>' +
        '<Override PartName="/word/document.xml" ContentType="' + DOCX_MIME + '.main+xml"/>' +
        '</Types>';

    const rels =
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>' +
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' +
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>' +
        '</Relationships>';

    return zipStored([
        { name: "[Content_Types].xml", bytes: encoder.encode(contentTypes) },
        { name: "_rels/.rels", bytes: encoder.encode(rels) },
        { name: "word/document.xml", bytes: encoder.encode(documentXml) },
    ]);
    //three parts is the minimum Word accepts. Stored rather than deflated
    //because the page has no compressor of its own
}

let savedTurns = readSavedTurns();
//{id, question, answer, mode} per staged pair, mode being which halves of it
//the export prints. id is the pair number, so re-picking a pair replaces its
//entry rather than adding a second

function readSavedTurns() {
    try {
        const raw = sessionStorage.getItem(SAVED_TURNS_STORAGE);
        const parsed = raw ? JSON.parse(raw) : [];
        if (!Array.isArray(parsed)) {
            return [];
            //anything else is storage written by something other than this page
        }
        return parsed.map((entry) => ({
            ...entry,
            mode: entry.mode || (entry.withQuestion ? "qa" : "answer"),
        }));
        //entries staged before the question-only button existed carry the old
        //boolean, and would otherwise match no button and print nothing
    } catch (error) {
        addLog(`could not read the staged turns: ${error.message}`);
        return [];
    }
}

function writeSavedTurns() {
    try {
        sessionStorage.setItem(SAVED_TURNS_STORAGE, JSON.stringify(savedTurns));
    } catch (error) {
        addLog(`could not stage that turn: ${error.message}`);
        //a blocked sessionStorage must not break the exam: the in-memory
        //list is still correct, so the export keeps working
    }
}

function savedEntryFor(pair) {
    return savedTurns.find((entry) => entry.id === pair.index) || null;
}

function toggleSaved(pair, mode) {
    const at = savedTurns.findIndex((entry) => entry.id === pair.index);

    if (at !== -1 && savedTurns[at].mode === mode) {
        savedTurns.splice(at, 1);
        //the same button again unstages, so a mis-click is undone in place
    } else {
        const entry = {
            id: pair.index,
            question: pair.question,
            answer: pair.answer,
            mode,
        };
        //copied in, so a staged turn outlives the transcript it came from

        if (at !== -1) {
            savedTurns[at] = entry;
        } else {
            savedTurns.push(entry);
        }
    }

    writeSavedTurns();
    paintSaveBar(pair);
    paintExportButton();
}

function paintSaveBar(pair) {
    const entry = savedEntryFor(pair);
    for (const button of pair.element.querySelectorAll(".pair-save button")) {
        const chosen = entry !== null && entry.mode === button.dataset.mode;
        button.classList.toggle("selected", chosen);
        button.setAttribute("aria-pressed", chosen ? "true" : "false");
        //a choice held until the export, not a one-shot action
    }
}

function paintExportButton() {
    exportButton.textContent = savedTurns.length === 0
        ? "Export session"
        : `Export session (${savedTurns.length})`;
    //the count is the only feedback once the pair has scrolled off screen
}

const SAVE_MODES = [ //the ways to stage this turn for the export
    { mode: "qa", label: "Save question + answer" },
    { mode: "answer", label: "Save answer only" },
    { mode: "question", label: "Save question only" },
];

function makeSaveBar(pair) {
    const bar = document.createElement("div");
    bar.className = "pair-save";

    for (const { mode, label } of SAVE_MODES) {
        const button = document.createElement("button");
        button.dataset.mode = mode;
        button.textContent = label;
        button.onclick = () => toggleSaved(pair, mode);
        bar.appendChild(button);
    }

    return bar;
}

exportButton.onclick = () => { //one document holding every staged turn
    if (savedTurns.length === 0) {
        window.alert(
            "Nothing saved yet.\n\n" +
            "This button downloads a Word document of the turns you pick out " +
            "of the exam. Use the Save buttons under a question and answer " +
            "to add it, then come back here.");
        return;
        //the button stays clickable while empty so it can say this: disabled,
        //it explained nothing and the save bar went unnoticed
    }

    const ordered = [...savedTurns].sort((a, b) => a.id - b.id);
    //click order is not exam order, and the document reads as a transcript

    const paragraphs = [];
    const heading = studentName.value.trim()
        ? `Speaking exam - ${studentName.value.trim()}`
        : "Speaking exam";
    paragraphs.push({ text: heading, bold: true });
    paragraphs.push({ text: new Date().toLocaleString(), bold: false });
    paragraphs.push({ text: "", bold: false });

    for (const entry of ordered) {
        if (entry.mode !== "answer" && entry.question) {
            paragraphs.push({
                text: `${ROLE_LABELS.examiner}: ${entry.question}`,
                bold: true,
            });
        }
        if (entry.mode !== "question") {
            paragraphs.push({
                text: `${ROLE_LABELS.student}: ${entry.answer}`,
                bold: false,
            });
        }
        paragraphs.push({ text: "", bold: false });
        //spacing, since a minimal .docx carries no paragraph styling
    }

    const stamp = new Date().toISOString().slice(0, 10);
    downloadBlob(`speaking-sim-session-${stamp}.docx`, docxBlob(paragraphs));
    //the staging is kept, so exporting again still gives the whole selection
};

function addTurn(role, text) {
    const label = ROLE_LABELS[role];
    if (!label) {
        addLog(`ignored turn from unknown role: ${role}`);
        return;
        //an unrecognised role means the caller is wrong, so refuse to paint
        //rather than show a turn whose speaker the student cannot identify
    }

    if (role === "examiner" || !currentPair) {
        pairCount += 1;
        const group = document.createElement("div");
        group.className = "pair";
        transcript.appendChild(group);
        currentPair = { index: pairCount, element: group, question: "", answer: "" };
        //an examiner turn opens the pair its answer will join. An answer with
        //no pair open gets its own group rather than an unrelated question
    }

    currentPair.element.appendChild(makeCard(role, label, text));

    if (role === "examiner") {
        currentPair.question = text;
        setExamLoading(false);
        startExamTimer();
        //the first painted question starts the clock; every later one finds it
        //already running. Starting here rather than at the Start button keeps
        //the wait for the opening call off the student's five minutes
    } else {
        currentPair.answer = text;
        currentPair.element.appendChild(makeSaveBar(currentPair));
        paintSaveBar(currentPair);
        //only once there is an answer: a pending question offers nothing to save
    }
}

export function paintUsage(usage) {
    if (usage) {
        speakingUsage = Object.assign({}, usage);
    }
    if (!speakingUsage) {
        usageLine.hidden = true;
        return;
        //not signed in, or the count could not be read: say nothing rather
        //than a number that might be wrong
    }
    const left = Math.max(0, speakingUsage.speaking_limit - speakingUsage.speaking_used);
    const plural = (n) => (n === 1 ? "question" : "questions");
    const where = speakingUsage.paid_source === "class" ? "Your class licence"
        : speakingUsage.paid_source === "teacher" ? "Teacher account"
            : speakingUsage.paid ? "Your plan" : "Free plan";
    usageLine.textContent = speakingUsage.paid
        ? `${where}: ${left} speaking ${plural(left)} left today.`
        : `${where}: ${left} of ${speakingUsage.speaking_limit} free speaking ` +
          `${plural(speakingUsage.speaking_limit)} left today. Listening is always free.`;
    usageLine.classList.toggle("low", left <= 1);
    usageLine.hidden = false;
}

function noteQuestionsLeft(left) {
    if (!speakingUsage || typeof left !== "number") return;
    speakingUsage.speaking_used = speakingUsage.speaking_limit - left;
    paintUsage();
    //the server's count after this question, not a guess made here: a second
    //tab spending questions shows up on the next reply in this one
}

function showExamNotice(text) {
    examNotice.textContent = text;
    examNotice.hidden = false;
}

function hideExamNotice() {
    examNotice.hidden = true;
    examNotice.textContent = "";
}

function setExamLoading(waiting) {
    examLoading.hidden = !waiting;
    //only the opening wait: later "thinking" gaps have a transcript above them
    //that shows the exam is under way, and the answer just given to read back
}

function browserSteps() { //permission is reset in the browser's own UI, and every browser hides it somewhere else
    const agent = navigator.userAgent;
    const site = location.host;

    if (/Edg\//.test(agent)) {
        return [
            "Click the padlock (or the crossed-out microphone) at the left of the address bar.",
            "Set Microphone to Allow.",
            `If it is not listed, open edge://settings/content/microphone and remove ${site} from Block.`,
            "Reload the page and press Start again.",
        ];
    }
    if (/Firefox\//.test(agent)) {
        return [
            "Click the padlock at the left of the address bar.",
            'Next to "Use the Microphone - Blocked", click the X to clear the block.',
            "Reload the page and press Start again, then choose Allow when Firefox asks.",
        ];
    }
    if (/Chrome\//.test(agent)) {
        return [
            "Click the padlock (or the crossed-out microphone) at the left of the address bar.",
            "Turn Microphone on.",
            `If it is not listed, open chrome://settings/content/microphone and remove ${site} from "Not allowed".`,
            "Reload the page and press Start again.",
        ];
    }
    if (/Safari\//.test(agent)) {
        return [
            "Open Safari > Settings > Websites > Microphone.",
            `Set ${site} to Allow.`,
            "Reload the page and press Start again.",
        ];
    }
    return [
        "Open your browser's site permissions for this page.",
        `Set the microphone for ${site} to Allow.`,
        "Reload the page and press Start again.",
    ];
    //a generic fallback rather than nothing: the shape of the fix is the same
    //everywhere even when the menu names are not
}

function showMicHelp(error) { //explain a failed getUserMedia and how to undo it
    const name = error && error.name ? error.name : "";
    let reason;
    let steps;

    if (name === "NotAllowedError" || name === "SecurityError") {
        reason = "This exam needs your microphone, and the browser has blocked it. " +
                 "Nothing is recorded or stored - the audio only goes to the examiner for this session.";
        steps = browserSteps();
        //NotAllowedError covers both "dismissed the prompt" and "blocked it
        //permanently"; the steps work for either, since a cleared block puts
        //the prompt back
    } else if (name === "NotFoundError" || name === "OverconstrainedError") {
        reason = "No microphone was found. Plug one in, or pick an input device in your " +
                 "system sound settings, then press Try again.";
        steps = [];
    } else if (name === "NotReadableError" || name === "AbortError") {
        reason = "The microphone is there but another program is holding it. Close anything " +
                 "else using it - Teams, Zoom, another tab of this exam - then press Try again.";
        steps = [];
    } else if (!navigator.mediaDevices) {
        reason = "This browser will not give a page microphone access over plain HTTP. " +
                 `Open the exam from http://localhost instead of ${location.host}.`;
        steps = [];
        //mediaDevices is undefined, not an error name, when the page is served
        //from a non-local address without TLS - a likely way to meet this app
    } else {
        reason = `The microphone could not be opened (${name || "unknown error"}).`;
        steps = [];
    }

    micReason.textContent = reason;
    micSteps.replaceChildren(...steps.map((text) => {
        const item = document.createElement("li");
        item.textContent = text;
        return item;
    }));
    micOverlay.hidden = false;
    micRetry.focus();
}

micDismiss.onclick = () => {
    micOverlay.hidden = true;
};

micRetry.onclick = () => {
    micOverlay.hidden = true;
    startButton.click();
    //the whole start path again rather than getUserMedia alone: the failed
    //attempt already tore the socket and the context down. click() rather than
    //onclick(), so a Start that is latched off stays off
};

micOverlay.onclick = (event) => {
    if (event.target === micOverlay) {
        micOverlay.hidden = true;
    }
};

function startExamTimer() {
    if (examDeadline !== null || examExpired) {
        return;
        //already running, or already spent: the clock belongs to the page, not
        //to the session, so a second session cannot buy another five minutes
    }
    examDeadline = Date.now() + examDurationMs;
    examTimer.hidden = false;
    paintExamTimer();
    examTick = setInterval(paintExamTimer, 250);
    //deadline arithmetic rather than a counter, so a throttled background tab
    //comes back showing the real time left instead of the ticks it missed
}

function pauseExamTimer() { //bank the remainder and stop counting
    if (examDeadline === null || examPaused) {
        return;
    }
    examRemaining = Math.max(0, examDeadline - Date.now());
    clearInterval(examTick);
    examTick = null;
    examPaused = true;
    examTimer.classList.add("paused");
    paintExamTimer();
    //repainted from the banked remainder, so the pill freezes on the reading
    //the student stopped at rather than on whatever the last tick caught
}

function resumeExamTimer() { //buy a fresh deadline from what was banked
    if (!examPaused) {
        return;
    }
    examPaused = false;
    examTimer.classList.remove("paused");
    examDeadline = Date.now() + examRemaining;
    examRemaining = null;
    paintExamTimer();
    examTick = setInterval(paintExamTimer, 250);
}

function resetExamTimer() { //stop the clock and put five minutes back on it
    clearInterval(examTick);
    examTick = null;
    examDeadline = null;
    examExpired = false;
    examPaused = false;
    examRemaining = null;
    pausePending = false;
    //the pause belongs to the exam that just ended, not to the next one: a
    //latched pause would otherwise come up already stopped on a fresh clock
    //cleared as well as stopped, so the next Start is allowed to run and gets
    //a whole exam rather than the remainder of the one just abandoned
    examTimer.hidden = true;
    examTimer.classList.remove("low", "expired", "paused");
    examTimer.textContent = formatClock(examDurationMs);
    //repainted now rather than at the next start, so the pill does not flash
    //the abandoned session's last reading before the first question lands
}

function formatClock(ms) {
    const seconds = Math.ceil(ms / 1000);
    return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
}

function paintExamTimer() {
    const left = examPaused
        ? examRemaining
        : Math.max(0, examDeadline - Date.now());
    //while paused the remainder is the truth: the deadline it was taken from
    //is already in the past and would paint 0:00 and expire the exam
    examTimer.textContent = formatClock(left);
    examTimer.classList.toggle("low", left > 0 && left <= 60 * 1000);

    if (left === 0 && !examPaused) {
        expireExam();
        //a paused clock never runs out, however long the pause lasts: the
        //remainder is frozen, and only a resume can walk it down to zero
    }
}

function expireExam() { //the clock has run out
    examExpired = true;
    pausePending = false;
    //a pause still waiting on the examiner has nothing left to stop, and the
    //button has to come off "Pausing..." rather than sit there owing a pause
    //that will never arrive
    clearInterval(examTick);
    examTick = null;
    examTimer.classList.remove("low");
    examTimer.classList.add("expired");
    paintPauseButton();

    addLog("time is up - finish your answer, it will still be marked");
    //nothing is torn down here. A request already sent still comes back, so the
    //student gets that last question, hears it, and answers it; an answer
    //already in progress is still submitted and transcribed. What the latch
    //denies is the examiner call that would follow, and the session ends on the
    //server's "ended" status once the last answer has been written down
}

function paintPauseButton() { //label and latch, from the pause state alone
    if (examPaused) {
        pauseButton.textContent = "Resume";
        pauseButton.disabled = false;
        return;
        //never latched while paused: whatever else is true, the student has to
        //be able to start the exam moving again
    }

    pauseButton.textContent = pausePending ? "Pausing..." : "Pause";
    pauseButton.disabled =
        turnState === "idle" || examExpired || pausePending;
    //idle has no clock to stop and expired has none left to save. A pause
    //already waiting on the examiner is disabled rather than relabelled back,
    //so the second press cannot cancel a pause the student cannot see coming
}

function setTurnState(state) {
    turnState = state;
    startButton.disabled = state !== "idle" || examExpired;
    //the clock latches on expiry, so a fresh session - and the opening examiner
    //call it would make - is refused until the exam that ran out has finished
    //closing itself down and resetExamTimer has cleared the latch
    studentName.disabled = state !== "idle";
    //read once, at the start message, so editing it mid-exam would change
    //nothing the examiner sees - same reasoning as the key picker below
    doneButton.disabled = state !== "armed";
    //deliberately not latched by examExpired: an answer in progress when the
    //clock ran out is still worth transcribing, and the final flag on its stop
    //is what stops it turning into another examiner call
    paintPauseButton();
    endButton.disabled = state === "idle";
    settingsButton.disabled = state !== "idle";
    //the picked key rides on the "start" message only, so changing it once a
    //session is running would silently do nothing, and the modal holding it is
    //only reachable through this button
    languageSelect.disabled = state !== "idle" || classLocksLanguage();
    //the language sits on the page rather than behind the modal now, so unlike
    //the key it has to disable itself: it is read once at the start message.
    //A class's exam is in the class's language, so a picked class holds it too
    classSelect.disabled = state !== "idle";
    planSelect.disabled = state !== "idle";
    joinClassButton.disabled = state !== "idle";
    //the class rides on the start message like the language, so it is fixed
    //for the rest of the session
}

async function loadGeminiKeys() {
    let names = [];
    try {
        const response = await fetch("/api/gemini-keys");
        if (response.ok) {
            names = await response.json();
        }
    } catch (error) {
        addLog(`could not load gemini keys: ${error.message}`);
    }

    geminiKeySelect.innerHTML = "";

    if (names.length === 0) {
        const option = document.createElement("option");
        option.value = "";
        option.textContent = "(none configured)";
        geminiKeySelect.appendChild(option);
        geminiKeySelect.disabled = true;
        return;
    }

    geminiKeySelect.disabled = false;
    for (const name of names) {
        const option = document.createElement("option");
        option.value = name;
        option.textContent = name;
        geminiKeySelect.appendChild(option);
    }

    const saved = localStorage.getItem(GEMINI_KEY_STORAGE);
    if (saved && names.includes(saved)) {
        geminiKeySelect.value = saved;
        //an unrecognised saved name (stale after .env changed) falls back to
        //whichever option the browser selects by default, the first one
    }
}

export let languages = [];
//[{id, label, translate_code}] from /api/languages, kept so the change handler
//can map the picked id back to its translate code without a second fetch

export function translateCodeFor(id) {
    const match = languages.find((entry) => entry.id === id);
    return match ? match.translate_code : "it";
}

export function updatePageTitle(id) {
    const match = languages.find((entry) => entry.id === id);
    const label = match ? match.label : "Italian";
    pageTitle.textContent = `${label} Speaking Exam Simulator`;
}

async function loadLanguages() {
    try {
        const response = await fetch("/api/languages");
        if (response.ok) {
            languages = await response.json();
        }
    } catch (error) {
        addLog(`could not load languages: ${error.message}`);
    }

    languageSelect.innerHTML = "";

    if (languages.length === 0) {
        const option = document.createElement("option");
        option.value = DEFAULT_LANGUAGE;
        option.textContent = "Italian";
        languageSelect.appendChild(option);
        languageSelect.disabled = true;
        return DEFAULT_LANGUAGE;
        //the server answered with nothing, so the exam still runs: an empty
        //picker would leave the start message with no language at all, and the
        //backend would have fallen back to italian anyway
    }

    languageSelect.disabled = false;
    for (const entry of languages) {
        const option = document.createElement("option");
        option.value = entry.id;
        option.textContent = entry.label;
        languageSelect.appendChild(option);
    }

    const saved = localStorage.getItem(LANGUAGE_STORAGE);
    if (saved && languages.some((entry) => entry.id === saved)) {
        languageSelect.value = saved;
    } else if (languages.some((entry) => entry.id === DEFAULT_LANGUAGE)) {
        languageSelect.value = DEFAULT_LANGUAGE;
        //not "whichever the browser selects by default", which is the first
        //option: /api/languages comes back in the registry's own map order,
        //so German sorts ahead of Italian and a first visit would quietly
        //start a German exam. An unrecognised saved id lands here too
    }
    return languageSelect.value;
}

export async function applyPreferredLanguage(id) {
    // the language the account carries, which is the one the page opens on.
    // Waits on the picker's own fetch rather than assuming it has landed:
    // /api/me and /api/languages are two requests in flight at once, and
    // either can win. An empty or unrecognised id leaves the picker on
    // whatever loadLanguages settled on
    await languagesReady;
    if (!id || !languages.some((entry) => entry.id === id)) return;

    languageSelect.value = id;
    localStorage.setItem(LANGUAGE_STORAGE, id);
    setTranslateLanguage(translateCodeFor(id));
    updatePageTitle(id);
    // the same three things loadLanguages does for a saved choice, so a
    // sign-up preference lands the page in exactly the state a returning
    // student's saved one would
}

export function applyAccountName(fullName) {
    //the page greets with the name you are called rather than the one on the
    //enrolment form. It stays in this browser: localStorage and the heading of
    //a saved transcript are the whole of its reach
    if (studentName.value.trim()) return;
    //something already typed wins: the account name is a starting point, not
    //a correction. Blank-but-saved counts as typed only once it has content,
    //so clearing the box and reloading offers the account name again

    const first = (fullName || "").trim().split(/\s+/)[0] || "";
    if (!first) return;

    studentName.value = first;
    localStorage.setItem(STUDENT_NAME_STORAGE, first);
    //saved the same way typing it would, so the next load takes the early
    //localStorage path above and never waits on /api/me
}

languageSelect.onchange = () => {
    localStorage.setItem(LANGUAGE_STORAGE, languageSelect.value);
    savePreferredLanguage(languageSelect.value);
    //to the account as well as this browser, so the next device opens on the
    //same exam. localStorage stays as the offline answer for a page that
    //loads before /api/me does
    setTranslateLanguage(translateCodeFor(languageSelect.value));
    updatePageTitle(languageSelect.value);
    //the translate box follows the exam: looking up an Italian word while
    //sitting a German exam is not what the button is for
};

settingsButton.onclick = () => {
    settingsOverlay.hidden = false;
};

settingsClose.onclick = () => {
    settingsOverlay.hidden = true;
};

settingsOverlay.onclick = (event) => {
    if (event.target === settingsOverlay) {
        settingsOverlay.hidden = true;
        //click on the dimmed backdrop, not the modal card itself
    }
};

geminiKeySelect.onchange = () => {
    localStorage.setItem(GEMINI_KEY_STORAGE, geminiKeySelect.value);
};

studentName.value = localStorage.getItem(STUDENT_NAME_STORAGE) || "";
//|| "" because getItem returns null when nothing was ever saved, and null
//would be painted into the box as the literal text "null"

studentName.oninput = () => {
    localStorage.setItem(STUDENT_NAME_STORAGE, studentName.value);
    //on input rather than on change, so a name typed and then closed with the
    //backdrop click is still saved
};

initTranslate({
    log: addLog,
    language: translateCodeFor(
        localStorage.getItem(LANGUAGE_STORAGE) || DEFAULT_LANGUAGE),
});
//the box itself lives in translate.js, shared with the listening page. The
//object form is used so the box opens on the exam's language rather than
//always on Italian; loadLanguages() re-applies it once the real list has
//arrived, because translateCodeFor cannot resolve an id before that fetch

loadGeminiKeys();

export const languagesReady = loadLanguages().then((id) => {
    setTranslateLanguage(translateCodeFor(id));
    //re-applied here rather than only above: until the fetch lands, languages
    //is empty and translateCodeFor falls back to "it" for every id
    updatePageTitle(id);
});

pairCount = savedTurns.reduce((highest, entry) => Math.max(highest, entry.id), 0);
paintExportButton();
//turns staged before a reload survive, so numbering starts above them and the
//export button comes up already carrying them

startButton.onclick = async () => {
    if (turnState !== "idle" || examExpired) {
        return;
    
    }
    setTurnState("thinking");
    setExamLoading(true);
    hideExamNotice();
    socketOpened = false;
    lastServerError = "";

    audioContext = new AudioContext({ sampleRate: CAPTURE_SAMPLE_RATE });
    //synchronous and permission-free, so it is built before the socket rather
    //than behind getUserMedia: playAudio and armMic both test it, and the
    //opening reply can now land while the mic is still being opened

    const wsScheme = location.protocol === "https:" ? "wss:" : "ws:";
    socket = new WebSocket(`${wsScheme}//${location.host}/ws`);
    //derived rather than hardcoded: a browser refuses a plaintext ws:// from a
    //page served over https, so a fixed ws:// breaks the moment this sits
    //behind a reverse proxy
    socket.binaryType = "arraybuffer";
    //tell the socket to send binary data as an ArrayBuffer (raw bytes)

    socket.onopen = () => {
        socketOpened = true;
        addLog("connected");
        socket.send(JSON.stringify({
            type: "start",
            payload: "",
            language: languageSelect.value || DEFAULT_LANGUAGE,
            //picks the prompts, question bank, speech recognition language and
            //voice for this whole session. An id the server does not know
            //falls back to its own default rather than failing the start
            gemini_key: geminiKeySelect.value || "",
            class_id: selectedClassId() || undefined,
            plan_id: selectedPlanId() || undefined,
            //a named plan the student picked; absent means the class default
            //undefined drops the key, which the server reads as private
            //practice, rather than sending a 0 it would have to interpret
            //
            //NO student_name. The name is rendered in this page and nowhere
            //else: it is never posted, never stored server side and never
            //reaches the examiner. See docs/compliance/data-retention.md
        }));
        //ask the examiner for the opening question. Without this nothing is
        //sent until the student ends a turn, so the exam begins in silence.
        //gemini_key carries the settings picker's choice for this whole session
    };
    socket.onclose = () => {
        addLog("disconnected");
        if (!socketOpened) {
            showExamNotice("Could not start the exam. Check that you are " +
                "signed in, then try again.");
        }
        teardown();
        //a server-side drop must release the mic and the graph too, otherwise
        //the recording light stays on with nowhere to send the audio
    };
    socket.onerror = () => addLog("socket error");
    socket.onmessage = handleMessage;

    try {
        if (!navigator.mediaDevices) {
            throw new Error("mediaDevices unavailable");
            //an insecure origin has no mediaDevices at all, so the call below
            //would throw a TypeError showMicHelp could not tell apart
        }
        mediaStream = await navigator.mediaDevices.getUserMedia({ audio: true });
        //ask for audio only permission to get input
    } catch (error) {
        addLog(`microphone unavailable: ${error.message}`);
        teardown();
        showMicHelp(error);
        return;
        //after teardown, which clears the loading banner and closes the socket:
        //a denied permission previously left the socket open and a server-side
        //Session allocated for a client that could never speak, and said so
        //only in the console where a student would never look
    }

    if (!audioContext) {
        return;
        //the session was ended while the permission prompt was still up, so
        //teardown has already run and there is no graph to build onto
    }

    buildCaptureGraph();
    //deliberately after the socket, not in front of it. getUserMedia waits on
    //the permission prompt and the device open, and the opening request used
    //to sit behind all of it; the mic is not needed until the student speaks
};

function buildCaptureGraph() { //capture audio from users mic in browser
    micSource = audioContext.createMediaStreamSource(mediaStream);
    //create a source (mic) from mediaDevices
    processor = audioContext.createScriptProcessor(BLOCK_SIZE, 1, 1);
    //create a processing node of (buffer size, input channel, output channel)

    micSource.connect(processor);   //Connect mic audio to processor
    processor.connect(audioContext.destination);
  

    blockStartTime = audioContext.currentTime;
    //the block handed to the first callback starts filling as the graph is built

    micReady = true;
    if (pendingArm) {
        pendingArm = false;
        armMic();
        //the examiner finished speaking before the mic was open, so the turn
        //was owed to the student and is handed over now
    }

    processor.onaudioprocess = (event) => {
    //assign a function to on audio process event
        if (!audioContext) {
            return;
            //a callback queued before teardown ran
        }

        const input = event.inputBuffer.getChannelData(0);
        //getChannelData(0) accesses channel 0 samples as 32 bit floats
        let slice = null;

        if (captureState === "capturing") {
            slice = input;
            //a whole block, the common case
        } else if (captureState === "armed") {
            slice = input.subarray(headCut);
            //the first partial block: [0, headCut) is the examiner still coming
            //out of the speakers, everything after it is the student
            headCut = 0;
            captureState = "capturing";
        } else if (captureState === "stopping") {
            slice = input.subarray(headCut, tailCut);

            headCut = 0;
            captureState = "idle";
        }

        if (slice && slice.length) {
            sendPcm(slice);
        }

        if (pendingStop && captureState === "idle") {
            if (socket && socket.readyState === WebSocket.OPEN) {
                socket.send(JSON.stringify({
                    type: "stop",
                    payload: "",
                    final: examExpired,
                    //the clock has run out, so the server transcribes this
                    //answer and stops there rather than asking the examiner
                    //for a question the student has no time left to hear
                }));
            }
            pendingStop = false;
            //sent only once the trimmed block above has gone out, so the whole
            //answer is on the server before handle_control reads it
        }

        blockStartTime = audioContext.currentTime;
        
    };
}

function sendPcm(floatSamples) { //forward one slice of mic audio to the server
    if (!socket || socket.readyState !== WebSocket.OPEN) {
        return;
    } //only send audio if the socket is ready

    const resampled = downsampleTo16k(floatSamples, audioContext.sampleRate);

    socket.send(floatToInt16(resampled).buffer);
    //rewrite samples as 16 bit Int and send the raw bytes
}

function cutPoint() { //how far into the block now filling we are, in samples
    const elapsed = audioContext.currentTime - blockStartTime;
    const sample = Math.round(elapsed * audioContext.sampleRate);
    //in context-rate samples, because that is what indexes inputBuffer. The
    //context is usually already at 16 kHz, but a browser may refuse the hint
    return Math.max(0, Math.min(BLOCK_SIZE, sample));
}

function armMic() { //hand the turn to the student
    if (turnState === "idle" || !audioContext) {
        return;
        //the session was ended while the examiner's audio was still playing, so
        //onended fired against a torn-down graph
    }
    if (pausePending && !examExpired) {
        enterPause();
        return;
        //the pause the student asked for mid-request. The examiner's reply has
        //now been painted and spoken in full, so this is the first moment the
        //exam can stop without swallowing anything. Dropped rather than
        //honoured if the clock ran out while that request was in flight: there
        //is nothing left to save, and this last answer still has to be given
    }
    if (examPaused) {
        return;
        //paused already: the mic stays shut until Resume, which re-arms it
    }
    if (captureState === "armed" || captureState === "capturing") {
        return;
        //already the student's turn; re-arming would drag headCut into the
        //middle of their words
    }
    if (!micReady) {
        pendingArm = true;
        return;
        //cutPoint() reads blockStartTime, which buildCaptureGraph has not set
        //yet. Arming now would take headCut from a graph that does not exist
    }

    headCut = cutPoint();
    captureState = "armed";
    setTurnState("armed");
    addLog("your turn - press Finished Response when you have finished");
}

function stopMic() { //take the turn back from the student
    if (captureState !== "armed" && captureState !== "capturing") {
        return;
    }
    if (captureState === "capturing") {
        headCut = 0;
        //the armed block has already flushed, so the whole head of this block
        //belongs to the student
    }

    tailCut = cutPoint();
    captureState = "stopping";
    pendingStop = true;
    //the stop message goes out from onaudioprocess once the trimmed block has
    //been sent, never before it
}

doneButton.onclick = () => { //the student has finished this answer
    if (turnState !== "armed") {
        return;
    }

    stopMic();
    setTurnState("thinking");
    addLog("thinking...");
};

function sendClockMessage(type) {
    if (socket && socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ type, payload: "" }));
    }
    //the server runs the exam clock too, and cuts off an answer given after its
    //deadline. Without this a pause would stop only the countdown on screen
}

function enterPause() { //the clock stops and the mic goes quiet
    pausePending = false;
    pauseExamTimer();
    sendClockMessage("pause");

    if (captureState === "armed" || captureState === "capturing") {
        captureState = "idle";
        //dropped rather than stopped: stopMic would send the answer so far to
        //the server as a finished turn, and a pause is not an answer. The
        //samples already streamed stay in the session's buffer, and the
        //resume re-arms on top of them so the answer continues where it left off
    }

    setTurnState("paused");
    addLog("paused - press Resume to carry on");
}

function leavePause() { //the clock and the turn both start again
    resumeExamTimer();
    sendClockMessage("resume");
    setTurnState("thinking");
    armMic();
    //always the student's turn to take back: a pause is only ever entered on
    //their turn, or - through pausePending - at the moment the examiner has
    //finished speaking and was about to hand it to them anyway. armMic sets
    //the state to "armed" itself, so the "thinking" above is just the state it
    //has to pass through to satisfy its own idle guard
}

pauseButton.onclick = () => {
    if (examPaused) {
        leavePause();
        return;
    }
    if (turnState === "idle" || examExpired || pausePending) {
        return;
    }

    if (turnState === "thinking") {
        pausePending = true;
        paintPauseButton();
        addLog("pausing - the examiner is still speaking, so this question " +
               "finishes first");
        return;
        //the request in flight is left entirely alone. It comes back, is
        //painted and is spoken, and armMic takes the pause instead of the turn
    }

    enterPause();
};

endButton.onclick = () => {
    addLog("session ended");

    if (socket && socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ type: "end", payload: "" }));
        //so the record says the student ended the exam rather than that the
        //socket dropped. Sent before teardown, which closes the socket
    }

    resetExamTimer();
    //before teardown, so its setTurnState("idle") re-enables Start against the
    //cleared latch. Only this button resets: an expiry that reset itself could
    //be re-rolled for another five minutes
    teardown();
};

function teardown() { //release everything this session allocated
    setExamLoading(false);
    //a start that died before its first question - denied mic, dropped socket -
    //must not leave the page waiting on a question that is no longer coming
    currentPair = null;
    //the next session's first answer must not attach to the last question of
    //this one. The painted cards and their save buttons stay on the page
    captureState = "idle";
    micReady = false;
    pendingArm = false;
    pausePending = false;
    examPaused = false;
    examRemaining = null;
    examTimer.classList.remove("paused");
    //a session that died while paused - a dropped socket, a denied mic - must
    //not leave a Resume button over an exam there is nothing left to resume
    if (!examExpired) {
        resetExamTimer();
        //a session that died with time still on it takes the clock with it.
        //Without this a mid-exam disconnect left examDeadline set and its
        //ticker running against an exam that was already gone, so the next
        //Start met startExamTimer's "already running" guard and ran with no
        //clock at all. An exam that ran out is left exactly as expireExam
        //latched it, so its minutes cannot be re-rolled by pulling the plug;
        //End and the server's own "ended" are still what clear that latch
    }
    headCut = 0;
    tailCut = 0;
    pendingStop = false;

    if (processor) {
        processor.onaudioprocess = null;
        processor.disconnect();
        processor = null;
    }
    if (micSource) {
        micSource.disconnect();
        micSource = null;
    }
    if (mediaStream) {
        const tracks = mediaStream.getTracks();
        //get all the tracks in the mic stream (usually one audio track)
        for (const track of tracks) {
            track.stop();
            //release the microphone and turn off the browser's recording light
        }
        mediaStream = null;
    }
    if (audioContext) {
        audioContext.close();
        audioContext = null;
        //close() frees the audio thread. Without it every abandoned session
        //left a context running for the lifetime of the page
    }
    if (socket) {
        const dying = socket;
        socket = null;
        dying.onclose = null;
        //teardown is already running, so do not let close() re-enter it
        if (dying.readyState === WebSocket.OPEN ||
            dying.readyState === WebSocket.CONNECTING) {
            dying.close(1000, "session ended");
            //an explicit 1000 rather than a bare close(): a codeless close
            //frame makes Crow echo back 1005, a code reserved for local use
            //that must never appear on the wire, and the browser kills the
            //connection as a protocol error instead of closing it cleanly.
            //closing makes Crow's .onclose erase the server-side Session
        }
    }

    playbackSampleRate = null;
    setTurnState("idle");
    //teardown is idempotent: every branch is null-guarded and nulls what it
    //released, so calling it from both endButton and socket.onclose is safe
}

function handleMessage(event) { //message from server
    if (typeof event.data === "string") {
        let message;
        try {
            message = JSON.parse(event.data);
        } catch (error) {
            addLog(`ignored an unreadable frame: ${error.message}`);
            return;
            //a text frame that is not JSON is the server's mistake, not the
            //student's. Logged and dropped, because an uncaught throw here takes
            //out the handler and every later message on this socket with it
        }
        addLog(`${message.type}: ${message.payload}`);
        //add to log element the message object
        if (message.sample_rate) {
            playbackSampleRate = message.sample_rate;
            //the server sends this with the examiner reply, immediately before
            //the binary frame it describes
        }

        if (message.type === "transcript") {
            addTurn("student", message.payload);
    
        }

        if (message.type === "examiner_text") {
            if (message.exam_seconds) {
                examDurationMs = message.exam_seconds * 1000;
                //set before addTurn below, which is what starts the countdown
            }
            noteQuestionsLeft(message.questions_left);
            if (message.payload) {
                addTurn("examiner", message.payload);
            }
    

            if (!message.sample_rate) {
                armMic();
                //no audio frame follows this reply, so the turn passes to the
                //student now rather than waiting on a playback that never starts.
                //When one does follow, playAudio's source.onended re-arms instead
            }
        }

        if (message.type === "error") {
            lastServerError = message.payload;
        }

        if (message.type === "status" && message.payload === "refused") {
            showExamNotice(lastServerError || "The exam could not start.");
            teardown();
            //the server turned the Start down before asking anything - not
            //signed up yet, or a class the student is not in. Said on the page,
            //since an error otherwise only reaches the console
        }

        if (message.type === "status" && message.payload === "busy") {
            armMic();
    
        }

        if (message.type === "status" && message.payload === "quota") {
            showExamNotice(lastServerError ||
                "That was the last speaking question for today.");
            if (speakingUsage) {
                speakingUsage.speaking_used = speakingUsage.speaking_limit;
                paintUsage();
            }
            resetExamTimer();
            teardown();
            //today's allowance ran out: the answer above has been transcribed
            //and saved, and the exam ends exactly as it does when time is up
        }

        if (message.type === "status" && message.payload === "ended") {
            addLog("time is up - that was the last answer");
            resetExamTimer();
            teardown();
            //the server sends this in place of an examiner reply once a final
            //stop has been transcribed, so the session closes only after the
            //answer above has been painted. Reset for the same reason the End
            //button resets: this exam is over and gone, so clearing the latch
            //can only buy a whole new one. Without it Start and End are both
            //disabled here and the page needs a reload to be usable again
        }
    } else { //handle binary audio
        playAudio(event.data);
    }
}

function playAudio(arrayBuffer) { //handling audio from server
    const int16arrfromserver = new Int16Array(arrayBuffer);
    //create a new int16 array holding audio buffer sent by server
    if (int16arrfromserver.length === 0) {
        armMic();
        return;
        //the TTS stub returns no samples, and createBuffer rejects a length of
        //0. There is nothing to wait for, so the turn passes to the student now
    }
    if (!audioContext) {
        return;
        //a frame that raced the teardown
    }

    captureState = "idle";
    setTurnState("thinking");
    //re-mute in case the mic was armed just before this frame landed: the
    //examiner is about to speak and must not be recorded as the answer

    const floatSamples = new Float32Array(int16arrfromserver.length);
    //create new array to hold 32 bit floats of audio (browser compatible)
    for (let i = 0; i < int16arrfromserver.length; i++) { //loop through
        floatSamples[i] = int16arrfromserver[i] / 32767;
        //convert each int 16 to float 32
    }

    const rate = playbackSampleRate || audioContext.sampleRate;
    //use the rate the server synthesised at. The AudioContext runs at the
    //capture rate, so falling back to it plays the reply at the wrong pitch
    const buffer = audioContext.createBuffer(1, floatSamples.length, rate);
    //empty mono buffer, length of the input buffer's samples. An AudioBuffer
    //may carry a different rate to its context and is resampled on playback
    buffer.getChannelData(0).set(floatSamples);
    //get reference to channel 0 in sample array, then .set copies all samples over

    const source = audioContext.createBufferSource();
    //create a buffer source node
    source.buffer = buffer; //give the source node the filled audio buffer
    source.connect(audioContext.destination); //connect to speakers

    source.onended = () => {
        armMic();
        //THE RE-ARM. The turn returns to the student only once the examiner has
        //stopped speaking, so the TTS output is never fed back into the mic
    };

    source.start(); //playback immediately
}

function downsampleTo16k(floatSamples, inputRate) {
    //helper func to bring mic samples down to the rate the STT model needs
    if (inputRate === CAPTURE_SAMPLE_RATE) {
        return floatSamples;
        //the AudioContext honoured the hint, so there is nothing to do
    }

    const ratio = inputRate / CAPTURE_SAMPLE_RATE;
    //how many input samples make up one output sample, e.g. 48000/16000 = 3
    const outputLength = Math.floor(floatSamples.length / ratio);
    const resampled = new Float32Array(outputLength);

    for (let i = 0; i < outputLength; i++) { //loop through output samples
        const position = i * ratio;
        //where this output sample sits in the input, usually between two samples
        const lower = Math.floor(position);
        const upper = Math.min(lower + 1, floatSamples.length - 1);
        const weight = position - lower;
        //how far between the two input samples the position falls, 0.0 to 1.0

        resampled[i] = floatSamples[lower] * (1 - weight) + floatSamples[upper] * weight;
        //linear interpolation between the two neighbours
    }
    return resampled;
}

function floatToInt16(floatSamples) { //helper func for browser audio samples to get sent to server
    const int16arrtoserver = new Int16Array(floatSamples.length);
    //create const array of 16 bit Ints at the length of audio samples
    for (let i = 0; i < floatSamples.length; i++)  //loop through audio samples
        {
        const cappedTop = Math.min(1, floatSamples[i]);
        //cap the upper bound: return whichever is smaller of 1 and i value

        const clamped = Math.max(-1, cappedTop);
        //floor the lower bound: return whichever is bigger -1 or cappedTop

        //clamped is now guaranteed to be within -1.0 to 1.0

        const scaled = clamped * 32767;
        //scale the [-1, 1] float onto the 16-bit integer range [-32767, 32767]

        int16arrtoserver[i] = scaled;
        //store the converted sample (Int16Array truncates any fraction to an integer)
        }
    return int16arrtoserver;
}
