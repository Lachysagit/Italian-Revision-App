# speaking-sim — build and runtime notes

Practical notes for building, running, and extending `speaking-sim/` that are
not otherwise written down anywhere in the tree. Paths are relative to
`speaking-sim/` unless stated otherwise. See `docs/system-model.md` for the
architecture and file inventory this complements.

## Building on Windows/MSVC

Building `third_party/piper` (and the main `speaking-sim` target) needs two
things the project's own build files do not mention:

1. **Run the build from a `vcvars64.bat` environment**, with the bundled Ninja
   on `PATH`. From a plain shell, piper's `ExternalProject_Add` sub-builds
   spawn nested `cmake` processes that inherit neither the compiler nor the
   generator from the outer cache, failing with "CMAKE_CXX_COMPILER not set" /
   "unable to find a build program corresponding to Ninja". The main target
   fails the same way from a plain shell, with an empty `INCLUDE`, giving
   `fatal error C1083: Cannot open include file: 'string'` (or `<atomic>`,
   `stdarg.h`, ...) — that looks like broken code but is just a missing
   compiler environment:

   ```
   cmd /c '"<VS>\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build build --config Release --target speaking-sim'
   ```

   where `<VS>` comes from `vswhere.exe -latest -property installationPath`.

2. **Force `-DCMAKE_BUILD_TYPE=Release` on `spdlog_external` and
   `piper_phonemize_external` explicitly.** Both default to Debug even when
   the outer tree is Release (`fmt_external` correctly inherits Release, they
   do not).

**Why the Debug/Release split matters:** it produces two distinct, silent
failures. Debug spdlog 1.12's bundled fmt calls
`stdext::checked_array_iterator`, removed from modern MSVC STL, so it will not
compile at all. And a Debug (`/MDd`) `piper_phonemize` linked into a Release
(`/MD`) `piper.exe` links fine but crashes: piper logs "Initialized piper" and
then segfaults on the first phonemize call, because `std::string`/`std::vector`
cross a mismatched-CRT boundary.

Rebuild each sub-project in place with Release and `--target install` before
relinking piper. Do not paper over the link error by aliasing `spdlogd.lib` to
`spdlog.lib` — that links but keeps the ABI mismatch and the segfault.

**Which tree to use:** `build/third_party/piper/piper.exe` is the fully-Release
one and what `run.ps1` drives. `build-piper-rel/`, despite its name, still has
the mismatch and segfaults on every voice — a segfault after "Initialized
piper" happens with *every* voice, so if one voice crashes, test another before
suspecting the model rather than the build.

One-command CRT mismatch check — scan a binary's imports rather than guessing:

```powershell
$b=[IO.File]::ReadAllBytes((Resolve-Path .\piper_phonemize.dll))
[regex]::Matches([Text.Encoding]::ASCII.GetString($b),
  '(?i)(VCRUNTIME140_?1?D?|MSVCP140D?|ucrtbased?)\.dll') |
  %{$_.Value} | Sort-Object -Unique
```

A trailing `D` (`MSVCP140D.dll`, `ucrtbased.dll`) on `piper_phonemize.dll` or
`espeak-ng.dll` while `piper.exe` shows plain `MSVCP140.dll` is the mismatch.

Note also: the top-level `project()` declares `LANGUAGES C CXX`. The `C` is
required because SQLite is a `.c` amalgamation; dropping it fails at generate
time with "Missing variable is: CMAKE_C_COMPILE_OBJECT", not at compile time.

## Running and smoke-testing

`speaking-sim` must be started from the `speaking-sim/` directory itself — it
opens `web/index.html` and `prompts/examiner_system.txt` by relative path.

Config comes from the process environment, not from `.env` directly:

```
set -a && . ./.env && set +a && ./build/speaking-sim.exe
```

Rebuilding the server also requires the `vcvars64.bat` shell described above —
same failure mode from a plain shell.

The failure modes here are misleading: a wrong working directory serves a 404
and silently falls back to a one-line built-in system prompt rather than
erroring, and a set-but-missing `WHISPER_MODEL_PATH` aborts startup outright.

The pipeline can be exercised without a browser by driving the WebSocket
directly — send `{"type":"start"}` for the opening examiner question, or
stream 16 kHz mono PCM16 binary frames followed by `{"type":"stop"}` for a full
STT → examiner → TTS turn. `third_party/whisper.cpp/samples/jfk.wav` is already
in the right format for that.

## Turn latency

Measured stage timings (Release build, 5 s of audio, i5-1135G7):

- whisper STT: ~3.3 s
- Gemini examiner: 5–9 s (one 12.7 s outlier)
- piper TTS: ~1.7 s

**The Gemini examiner call, not whisper, dominates turn time.** It is easy to
blame whisper because STT runs first and has an obvious local cost, but
`gemini-3.6-flash` reports `thoughtsTokenCount` of ~400–500 for a 10–25 token
examiner question — nearly all of the wall time is thinking, not generation or
network. The stage timings logged from `Server::enqueue_pipeline_job` ("turn
timings: audio ... stt ... examiner ... tts ... total") settle this — read them
before optimising anything.

The Gemini 3 thinking knob is `generationConfig.thinkingConfig.thinkingLevel`
("minimal" / "low" / "high"); the Gemini 2.5 spelling `thinkingBudget: 0` is
rejected with "Request contains an invalid argument". `thinkingLevel: "low"`
measured only a modest win (~450 → ~340 thinking tokens, ~6.2 s → ~5.1 s
median); "minimal" was never timed because the key hit its free-tier quota
first — quota pressure on the (rate-limited, free-tier) key is a plausible
cause of latency outliers too. Bigger structural wins would be
`streamGenerateContent` with TTS started on the first sentence, and reusing one
`httplib::Client` instead of building one (and a fresh TLS handshake) per turn.

## Multi-language exams

speaking-sim runs exams in more than one language from one server, picked per
session.

**The design:** `LanguagePack` (`include/sim/language.hpp`) holds everything
that varies by language — prompts, question bank, whisper code, piper voice,
opinion openers, and program-generated sentences that used to be
language-specific string literals in `session.cpp` / `question_bank.cpp` /
`gemini_examiner.cpp`. `LanguageRegistry` loads one pack per language at
startup; `Session` holds a bare `const LanguagePack*` into it. Adding a
language means one entry in `built_in_packs()`, one directory under
`prompts/`, one voice under `models/`.

**STT and TTS take a per-call parameter, not per-language instances.**
Whisper's `wparams.language` is per call and `ggml-base.bin` is multilingual,
so one 148MB model and one mutex serve every language. Piper spawns a
subprocess per turn with the model as an argument, so the voice is just
another argument.

**Topic tags stay English and shared.** `topics.cpp` maps tags onto syllabus
groups ("family life, home and neighbourhood"). They are internal — the
student never sees them — and are both the Gemini response-schema enum and the
`## ` headers in every `question_bank.txt`. Every language reuses them
byte-for-byte; only the questions under each header are in the exam language.

Two traps worth remembering when touching this code:

1. `asks_opinion` (`session.cpp`) lowercases byte-by-byte with `std::tolower`,
   so an opener containing non-ASCII letters (e.g. German ä/ö/ü/ß) **never
   matches** and the exam silently loses its opinion question. Every pack's
   `opinion_openers` must be ASCII-only.
2. `GeminiExaminer` used to detect the opening turn as "the loop emitted no
   contents", because `Session` sent the system prompt alone and the examiner
   bolted on a hardcoded language-specific opening user turn. `Session` now
   supplies that opening turn from the pack, so the count is never zero — the
   test is now "no `Role::Examiner` turn in the history". Getting this wrong
   silently drops the opening-turn temperature override and makes opening
   questions repetitive.

## Parsing HSC exam paper PDFs for listening questions

`ListeningAPP/NGBAF/parse_questions.py` mines Section I questions from exam
paper PDFs (not the transcript-only ones).

Two pieces of page geometry carry the whole parse:

- The question column sits at x≈71; the "Candidate's Notes" sidebar starts at
  x≥460. Dropping blocks at `x0 >= 450` removes the sidebar without any text
  filtering, and doubles as the crop's right edge.
- A question spans its own heading down to the next heading, which makes a
  clean `page.get_pixmap(clip=...)` rectangle.

**Why it's not simpler:** the multiple-choice pictures are *vector drawings,
not embedded images* — `get_images()` returns `[]` while `get_drawings()`
returns hundreds. The text layer holds only alt-text ("multiple choice
images"), so rasterising the region is the only way to recover them. Tables
are invisible to that same drawing test because they are flat rules, so
`page.find_tables()` catches those instead.

Three heading shapes break a naive `^Question (\d+) \((\d+) marks?\)` match,
and each silently drops a question, which then pairs every later question with
the wrong clip:

1. Sidebar text merged into the *same block* as the heading — match with
   `search`, not `match`.
2. The heading block is just `Question 8`; `(5 marks)` is a separate block —
   marks must be optional and recovered from the body.
3. Headings are not reliably their own block at all.

Guard with an assert that question numbers are contiguous 1..N *and* match the
clip count — that guard catches all three shapes above on the first run. Stems
for image questions must be trimmed to the instruction by accumulating lines
until one ends in `.:?!`; taking only the first line cuts wrapped sentences
mid-clause.
