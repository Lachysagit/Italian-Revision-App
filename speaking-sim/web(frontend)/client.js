const transcript = document.getElementById("transcript");
const startButton = document.getElementById("start");
const doneButton = document.getElementById("done");
const endButton = document.getElementById("end");
const exportButton = document.getElementById("export");
const settingsButton = document.getElementById("settings");
const settingsOverlay = document.getElementById("settingsOverlay");
const settingsClose = document.getElementById("settingsClose");
const geminiKeySelect = document.getElementById("geminiKeySelect");
const studentName = document.getElementById("studentName");
const examTimer = document.getElementById("examTimer");
const examLoading = document.getElementById("examLoading");
const translateInput = document.getElementById("translateInput");
const translateDirection = document.getElementById("translateDirection");
const translateGo = document.getElementById("translateGo");
const translateResult = document.getElementById("translateResult");
const translateCount = document.getElementById("translateCount");
//get references to HTML elements by their ID's

const GEMINI_KEY_STORAGE = "geminiKeyName";
//persists the picked key across page reloads, same tab only
const SAVED_TURNS_STORAGE = "savedTurns";
//sessionStorage, so a refresh mid-exam does not lose an hour of picked answers
const STUDENT_NAME_STORAGE = "studentName";
//same treatment for the name, so it is typed once rather than every session
const TRANSLATE_DIR_STORAGE = "translateDirection";
//the direction is a habit rather than a per-session choice, so it outlives the tab

let translateBusy = false;
//one lookup at a time. A second Enter while the first is in flight would race
//two responses into the same box, and the later one need not be the newer

let socket = null;
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


let pendingAudio = false;


let micReady = false;
//the capture graph now finishes building after the opening request is sent, so
//a reply can beat it. armMic records the owed turn and buildCaptureGraph takes it
let pendingArm = false;


let turnState = "idle";
//"idle" no session; "thinking" examiner is working and the mic is muted;
//"armed" student's turn, mic live and frames streaming

const EXAM_DURATION_MS = 5 * 60 * 1000;
//one exam is five minutes of the student's time

let examDeadline = null;
//wall-clock instant the exam ends, set by the first examiner question
let examTick = null;
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

    URL.revokeObjectURL(url);
    //the browser has the blob by now, and this frees our copy of it
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
    exportButton.disabled = savedTurns.length === 0;
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
        return;
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

function setExamLoading(waiting) {
    examLoading.hidden = !waiting;
    //only the opening wait: later "thinking" gaps have a transcript above them
    //that shows the exam is under way, and the answer just given to read back
}

function startExamTimer() {
    if (examDeadline !== null || examExpired) {
        return;
        //already running, or already spent: the clock belongs to the page, not
        //to the session, so a second session cannot buy another five minutes
    }
    examDeadline = Date.now() + EXAM_DURATION_MS;
    examTimer.hidden = false;
    paintExamTimer();
    examTick = setInterval(paintExamTimer, 250);
    //deadline arithmetic rather than a counter, so a throttled background tab
    //comes back showing the real time left instead of the ticks it missed
}

function resetExamTimer() { //stop the clock and put five minutes back on it
    clearInterval(examTick);
    examTick = null;
    examDeadline = null;
    examExpired = false;
    //cleared as well as stopped, so the next Start is allowed to run and gets
    //a whole exam rather than the remainder of the one just abandoned
    examTimer.hidden = true;
    examTimer.classList.remove("low", "expired");
    examTimer.textContent = "5:00";
    //repainted now rather than at the next start, so the pill does not flash
    //the abandoned session's last reading before the first question lands
}

function paintExamTimer() {
    const left = Math.max(0, examDeadline - Date.now());
    const seconds = Math.ceil(left / 1000);
    examTimer.textContent =
        `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
    examTimer.classList.toggle("low", left > 0 && left <= 60 * 1000);

    if (left === 0) {
        expireExam();
    }
}

function expireExam() { //the clock has run out
    examExpired = true;
    clearInterval(examTick);
    examTick = null;
    examTimer.classList.remove("low");
    examTimer.classList.add("expired");

    addLog("time is up - finish your answer, it will still be marked");
    //nothing is torn down here. A request already sent still comes back, so the
    //student gets that last question, hears it, and answers it; an answer
    //already in progress is still submitted and transcribed. What the latch
    //denies is the examiner call that would follow, and the session ends on the
    //server's "ended" status once the last answer has been written down
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
    endButton.disabled = state === "idle";
    settingsButton.disabled = state !== "idle";
    //the picked key rides on the "start" message only, so changing it once a
    //session is running would silently do nothing
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

const TRANSLATE_MAX_CHARS = 1000;
//mirrors kMaxTranslateChars in server.cpp; the server still enforces it, since
//maxlength is only a courtesy to whoever is typing

function paintTranslateCount() {
    const used = translateInput.value.length;
    const left = TRANSLATE_MAX_CHARS - used;
    translateCount.textContent = left <= 100 ? `${left} characters left` : "";
    translateCount.classList.toggle("near", left <= 100);
    //shown only near the cap: below that the number tells you nothing you were
    //going to act on
}

function paintTranslateDirection() {
    const toEnglish = translateDirection.dataset.direction !== "en-it";
    translateDirection.textContent = toEnglish ? "IT → EN" : "EN → IT";
    //the label states what the button will do to your text, not what pressing
    //it switches to, so it reads the same way as the result underneath
}

function showTranslateResult(text, isError) {
    translateResult.textContent = text;
    //textContent, never innerHTML: this string comes back from Google and is
    //not ours to trust as markup
    translateResult.classList.toggle("error", Boolean(isError));
    translateResult.hidden = false;
}

async function runTranslate() {
    const text = translateInput.value.trim();
    if (!text || translateBusy) return;

    const toEnglish = translateDirection.dataset.direction !== "en-it";
    const source = toEnglish ? "it" : "en";
    const target = toEnglish ? "en" : "it";

    translateBusy = true;
    translateGo.disabled = true;
    showTranslateResult("translating…", false);

    try {
        const response = await fetch("/api/translate", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ text, source, target })
        });
        const payload = await response.json();

        if (!response.ok) {
            showTranslateResult(payload.error || "Translation failed.", true);
            addLog(`translate failed: HTTP ${response.status} ${payload.error || ""}`);
            //the server already decided what the student should read, so its
            //message is shown as-is and the status only goes to the console
        } else {
            showTranslateResult(payload.translation, false);
        }
    } catch (error) {
        showTranslateResult("Could not reach the server.", true);
        addLog(`translate error: ${error.message}`);
        //a thrown fetch is the network, not the API - a different failure from
        //the one above and worth a different line in the log
    } finally {
        translateBusy = false;
        translateGo.disabled = false;
    }
}

translateDirection.dataset.direction =
    localStorage.getItem(TRANSLATE_DIR_STORAGE) === "en-it" ? "en-it" : "it-en";
//anything unrecognised, including the null of a first visit, falls back to
//IT -> EN: reading a question you did not understand is the commoner need
paintTranslateDirection();

translateDirection.onclick = () => {
    translateDirection.dataset.direction =
        translateDirection.dataset.direction === "en-it" ? "it-en" : "en-it";
    localStorage.setItem(TRANSLATE_DIR_STORAGE, translateDirection.dataset.direction);
    paintTranslateDirection();
    translateResult.hidden = true;
    //the old result was in the other direction, so leaving it up would label
    //itself with a heading it no longer matches
};

translateGo.onclick = runTranslate;

translateInput.oninput = paintTranslateCount;
paintTranslateCount();

translateInput.onkeydown = (event) => {
    if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        runTranslate();
    }
    //Enter is what "chuck a sentence in quick" actually means; the button is
    //there for the pointer, not for the keyboard. Shift+Enter is left alone so
    //the textarea can still take a second line when one is actually wanted
};

loadGeminiKeys();

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

    audioContext = new AudioContext({ sampleRate: CAPTURE_SAMPLE_RATE });
    //synchronous and permission-free, so it is built before the socket rather
    //than behind getUserMedia: playAudio and armMic both test it, and the
    //opening reply can now land while the mic is still being opened

    socket = new WebSocket(`ws://${location.host}/ws`);
    socket.binaryType = "arraybuffer";
    //tell the socket to send binary data as an ArrayBuffer (raw bytes)

    socket.onopen = () => {
        addLog("connected");
        socket.send(JSON.stringify({
            type: "start",
            payload: "",
            gemini_key: geminiKeySelect.value || "",
            student_name: studentName.value.trim(),
            //trimmed here so the server sees a real name or nothing at all;
            //Session treats a whitespace-only name as no name either way
        }));
        //ask the examiner for the opening question. Without this nothing is
        //sent until the student ends a turn, so the exam begins in silence.
        //gemini_key carries the settings picker's choice for this whole session
    };
    socket.onclose = () => {
        addLog("disconnected");
        teardown();
        //a server-side drop must release the mic and the graph too, otherwise
        //the recording light stays on with nowhere to send the audio
    };
    socket.onerror = () => addLog("socket error");
    socket.onmessage = handleMessage;

    try {
        mediaStream = await navigator.mediaDevices.getUserMedia({ audio: true });
        //ask for audio only permission to get input
    } catch (error) {
        addLog(`microphone unavailable: ${error.message}`);
        teardown();
        return;
        //a denied permission previously left the socket open and a server-side
        //Session allocated for a client that could never speak
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

endButton.onclick = () => {
    addLog("session ended");
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
    headCut = 0;
    tailCut = 0;
    pendingStop = false;
    pendingAudio = false;

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
        const message = JSON.parse(event.data);
        //parse JSON string into object
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
            if (message.payload) {
                addTurn("examiner", message.payload);
            }
    

            if (message.sample_rate) {
                pendingAudio = true;
    
            } else {
                pendingAudio = false;
                armMic();
    
            }
        }

        if (message.type === "status" && message.payload === "busy") {
            armMic();
    
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
        pendingAudio = false;
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
