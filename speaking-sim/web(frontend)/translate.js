import { api } from "/api.js";

//the quick-translate box, shared by the exam page and the listening page. Both
//carry the same #translateBox markup; this file is the one implementation behind
//it. initTranslate() is a no-op on a page without the box, so it is safe to load
//anywhere

const TRANSLATE_DIR_STORAGE = "translateDirection";
//the direction is a habit rather than a per-session choice, so it outlives the tab

const TRANSLATE_MAX_CHARS = 1000;
//mirrors kMaxTranslateChars in server.cpp; the server still enforces it, since
//maxlength is only a courtesy to whoever is typing

const TRANSLATE_LANGUAGES = {
    it: { label: "IT" },
    de: { label: "DE" }
};
//the non-English side of the pair. English is always the other half, so only this
//end varies

export let setTranslateLanguage = () => {};
//replaced by initTranslate on a page that has the box; the listening page calls it
//when its language picker moves

export let setTranslateLocked = () => {};
//replaced the same way. account.js calls it once /api/me says whether this
//account has paid access: translation is part of it, and a free account sees
//the box shut rather than a failure on every lookup

const TRANSLATE_LOCKED_TEXT =
    "Translation is part of paid access - a class licence from your school, or " +
    "your own. Speaking practice and every listening paper stay free.";

export function initTranslate(options) {
    const translateInput = document.getElementById("translateInput");
    const translateDirection = document.getElementById("translateDirection");
    const translateGo = document.getElementById("translateGo");
    const translateResult = document.getElementById("translateResult");
    const translateCount = document.getElementById("translateCount");

    if (!translateInput) {
        return;
    }
    //the page has no translate box, so there is nothing to wire up

    const settings = typeof options === "function" ? { log: options } : (options || {});
    //the exam page passes addLog directly; the listening page passes a language too

    const addLog = settings.log || (() => {});
    //the exam page routes these to its console log; the listening page has none

    let language = TRANSLATE_LANGUAGES[settings.language] ? settings.language : "it";

    let translateBusy = false;
    let translateLocked = false;
    //one lookup at a time. A second Enter while the first is in flight would race
    //two responses into the same box, and the later one need not be the newer

    function paintTranslateCount() {
        const used = translateInput.value.length;
        const left = TRANSLATE_MAX_CHARS - used;
        translateCount.textContent = left <= 100 ? `${left} characters left` : "";
        translateCount.classList.toggle("near", left <= 100);
        //shown only near the cap: below that the number tells you nothing you were
        //going to act on
    }

    function paintTranslateDirection() {
        const label = TRANSLATE_LANGUAGES[language].label;
        const toEnglish = translateDirection.dataset.direction !== "en-foreign";
        translateDirection.textContent = toEnglish ? `${label} → EN` : `EN → ${label}`;
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
        if (!text || translateBusy || translateLocked) return;

        const toEnglish = translateDirection.dataset.direction !== "en-foreign";
        const source = toEnglish ? language : "en";
        const target = toEnglish ? "en" : language;

        translateBusy = true;
        translateGo.disabled = true;
        showTranslateResult("translating…", false);

        try {
            const payload = await api("POST", "/api/translate",
                { text, source, target }, "Translation failed.");
            showTranslateResult(payload.translation, false);
        } catch (error) {
            if (error.status) {
                showTranslateResult(error.message, true);
                addLog(`translate failed: HTTP ${error.status} ${error.message}`);
                //the server already decided what the student should read, so its
                //message is shown as-is and the status only goes to the console
            } else {
                showTranslateResult("Could not reach the server.", true);
                addLog(`translate error: ${error.message}`);
                //no status means the request never arrived: the network, not the
                //API - a different failure from the one above and worth a
                //different line in the log
            }
        } finally {
            translateBusy = false;
            translateGo.disabled = false;
        }
    }

    translateDirection.dataset.direction =
        localStorage.getItem(TRANSLATE_DIR_STORAGE) === "en-foreign" ? "en-foreign" : "foreign-en";
    //anything unrecognised, including the null of a first visit, falls back to
    //IT/DE -> EN: reading a question you did not understand is the commoner need.
    //The stored value names no language, so the habit carries across both
    paintTranslateDirection();

    setTranslateLanguage = (code) => {
        if (!TRANSLATE_LANGUAGES[code] || code === language) {
            return;
        }
        language = code;
        paintTranslateDirection();
        translateResult.hidden = true;
        //the old result is in the other language, so it no longer matches the button
    };

    translateDirection.onclick = () => {
        translateDirection.dataset.direction =
            translateDirection.dataset.direction === "en-foreign" ? "foreign-en" : "en-foreign";
        localStorage.setItem(TRANSLATE_DIR_STORAGE, translateDirection.dataset.direction);
        paintTranslateDirection();
        translateResult.hidden = true;
        //the old result was in the other direction, so leaving it up would label
        //itself with a heading it no longer matches
    };

    setTranslateLocked = (locked) => {
        translateLocked = Boolean(locked);
        translateInput.disabled = translateLocked;
        translateGo.disabled = translateLocked;
        document.getElementById("translateBox").classList.toggle("locked", translateLocked);
        if (translateLocked) {
            showTranslateResult(TRANSLATE_LOCKED_TEXT, false);
        } else {
            translateResult.hidden = true;
        }
        //the server refuses a free account's lookups either way; this is only so
        //the page says so up front instead of on the first try
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
}
