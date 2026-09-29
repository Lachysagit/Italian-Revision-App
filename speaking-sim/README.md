# speaking-sim



Italian Exam API
Project number: 105421562997

A local speaking-exam simulator for beginner Italian and beginner German, picked
per session from the settings modal. The browser captures
microphone audio and streams it over a WebSocket to a C++ server, which runs it
through speech-to-text, sends the transcript plus the conversation history to an
examiner LLM over HTTP, synthesises the reply to speech, and streams the audio
back to the browser to play.

Everything runs on your own machine apart from the examiner call, which goes
either to the Gemini API over HTTPS or to a local Ollama-compatible server over
plain HTTP.

## Status

The pipeline is wired end to end, but the four pluggable pieces are stubs: they
log `not implemented` and return placeholder values.

| Piece | File | Current behaviour |
| --- | --- | --- |
| Whisper STT | `src/stt/whisper_stt.cpp` | returns `"placeholder transcript"` |
| Gemini examiner | `src/examiner/gemini_examiner.cpp` | returns `"placeholder examiner question"` |
| Hailo examiner | `src/examiner/hailo_examiner.cpp` | returns `"placeholder examiner question"` |
| Piper TTS | `src/tts/piper_tts.cpp` | returns an empty audio buffer |

So the server starts, accepts a WebSocket connection, buffers microphone audio,
and pushes a placeholder reply back through the worker pool — no real inference
happens yet.

## Prerequisites

- CMake 3.16 or newer
- A C++17 compiler (GCC 9+, Clang 10+, or MSVC 19.2+)
- OpenSSL development headers and libraries
  - Debian/Ubuntu: `sudo apt install libssl-dev`
  - macOS: `brew install openssl@3`
  - If CMake cannot find it, pass `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`
- git, for the `third_party/` submodules

No Hailo hardware or Hailo SDK is needed. The Hailo examiner is only an HTTP
client pointed at a local Ollama-compatible server, and nothing in the build
links a Hailo SDK.

## Getting the source

```sh
git clone --recurse-submodules <repo-url>
cd Italian-Revision-App/speaking-sim
```

If you already cloned without `--recurse-submodules`:

```sh
git submodule update --init --recursive
```

Two submodules live under `third_party/`:

| Submodule | Upstream | Role |
| --- | --- | --- |
| `whisper.cpp` | `ggml-org/whisper.cpp` (MIT) | Linked into the server as the `whisper` library |
| `piper` | `rhasspy/piper` (MIT) | Built as a standalone binary and driven as a subprocess |

Both are optional at configure time. When they are missing CMake prints a note
and configures the server without them, so a fresh clone still builds.

### A note on piper

piper publishes no C++ library — upstream defines only `add_executable(piper …)`
— so there is nothing to link against and the TTS backend has to invoke the
binary. CMake passes its path to the source as `SIM_PIPER_EXECUTABLE`.

It is deliberately left out of the default build, because building it downloads
onnxruntime, fmt and spdlog through `ExternalProject`. Build it when you need it:

```sh
cmake --build build --target piper
```

`rhasspy/piper` is archived upstream. Its successor, `OHF-Voice/piper1-gpl`, is
actively maintained but is GPL-3.0 and Python-first, so adopting it would place
this project under the GPL. The archived MIT release is pinned here instead; it
still matches the ONNX voice models `PIPER_MODEL_PATH` points at.

## Building

Out-of-source, from this directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Crow, standalone Asio and cpp-httplib are downloaded by CMake's FetchContent on
the first configure and cached under `build/_deps/`, so the first run needs
network access.

The binary lands at `build/speaking-sim`. On Windows the whisper and ggml DLLs
are copied next to it after linking, since Windows has no rpath and only
searches the executable's own directory.

## Running

```sh
cp .env.example .env      # then fill in GEMINI_API_KEY and the model paths
set -a && . ./.env && set +a
./build/speaking-sim
```

**Run the binary from this directory.** It opens `web(frontend)/index.html` and
`prompts/<language>/*.txt` by relative path, so starting it from anywhere else
serves a 404 for the page and silently falls back to a built-in one-line system
prompt.

Each language has its own directory under `prompts/` — `prompts/italian/` and
`prompts/german/` — holding `examiner_first.txt`, `examiner_ongoing.txt` and
`question_bank.txt`. The bank's `## ` group headers are the English syllabus
names in `src/topics.cpp` in every language and must match byte for byte; only
the questions under them are in the language being examined. Adding a third
language means one entry in `built_in_packs()` in `src/language.cpp`, one
directory here, and a piper voice in `models/`.

The server reads its settings from the process environment, not from `.env`
directly — hence the `set -a && . ./.env && set +a` above, which exports every
variable in the file into your shell.

Then open <http://localhost:8080> and allow microphone access.

| Variable | Default | Purpose |
| --- | --- | --- |
| `GEMINI_API_KEY` | *(empty)* | API key for the Gemini examiner |
| `EXAMINER_BACKEND` | `gemini` | `gemini` or `hailo`; anything but `hailo` means `gemini` |
| `HAILO_OLLAMA_URL` | `http://localhost:11434` | Local Ollama-compatible server for the Hailo examiner |
| `WHISPER_MODEL_PATH` | *(empty)* | whisper.cpp GGML model |
| `PIPER_MODEL_PATH` | *(empty)* | piper ONNX voice model |
| `PORT` | `8080` | Listening port |

## Layout

```
include/sim/      Public headers. The interfaces (InterfaceSTT, InterfaceExaminer,
                  InterfaceTTS) sit at the top level; concrete backends live in
                  the matching subdirectory.
src/              Implementation, mirroring include/sim/. main.cpp picks the
                  examiner backend from config and injects the concrete pieces
                  into Server as interfaces.
web(frontend)/    Browser client: index.html and client.js (mic capture, PCM
                  conversion, WebSocket, playback).
prompts/          One directory per language (italian/, german/), each holding
                  examiner_first.txt, examiner_ongoing.txt and
                  question_bank.txt. All read once at startup.
third_party/      git submodules: whisper.cpp and piper.
models/           Model weights. Ignored by git apart from .gitkeep — download
                  the whisper and piper models here yourself. One whisper model
                  serves every language (it is multilingual and takes the
                  language per call); each language needs its own piper voice,
                  with the voice's .onnx.json beside it so its sample rate can
                  be read.
```

## Architecture

`main.cpp` reads the config, constructs the concrete STT/examiner/TTS objects and
hands them to `Server` as interface pointers — the backend choice is made in
exactly one place.

`Server` owns a Crow app with two routes: `GET /` serves the page, and `/ws` is
the WebSocket. Binary frames are appended to that connection's `Session` audio
buffer. A text frame of `{"type":"stop"}` drains the buffer and hands it to a
`WorkerPool` job, which runs STT, the examiner call and TTS off the socket
thread, then writes the reply text and PCM audio back to the connection.

## Classes and teachers

Anyone who signs in with an address listed in `TEACHER_EMAILS` is a teacher and
gets a **Teacher** link in the nav, which opens the dashboard at `/teacher`.
Everyone else is a student.

A teacher creates a class for one language. Students get into it one of three
ways, all ending in the same `class_members` row:

| Way in | How it works |
| --- | --- |
| Join code | Every class has an 8-character code, shown large on the dashboard. Students press **Join a class** on the Speaking page and type it; case, spaces and the hyphen are ignored. The teacher can replace the code or switch joining off. |
| Join link | `/join/<code>` does the same from a link, and survives the sign-in round trip for a student who is not signed in yet. |
| Email roster | The teacher pastes addresses. Anyone who already has an account joins at once; the rest join the first time they sign in with that address. |

On the Speaking page the **Class** picker chooses who an exam is for. An exam
sat for a class is stored with its `class_id`, uses the class's language, and
appears on that class's dashboard; **Private practice** is visible to nobody
but the student. A student removed from a class disappears from its dashboard
along with their exams for it. Archiving hides a class from its students and
stops joins without deleting anything.

The websocket reads the session cookie at the handshake, so every attempt is
stored against the signed-in user, and it refuses connections from other
origins. The exam clock also runs on the server (`EXAM_DURATION_SECONDS`): an
answer sent after it is transcribed but earns no further question.

The routes behind the dashboard, all JSON and all cookie-authenticated:

| Method and path | Who | Does |
| --- | --- | --- |
| `GET /api/classes` | anyone signed in | classes the caller teaches or sits in |
| `POST /api/classes` | teacher | create a class `{name, language}` |
| `GET /api/classes/<id>` | its teacher | class, members and pending invites |
| `POST /api/classes/<id>/join-code` | its teacher | `{action: "rotate" \| "disable"}` |
| `POST /api/classes/<id>/invites` | its teacher | `{emails}` as pasted text or a list |
| `DELETE /api/classes/<id>/invites/<invite>` | its teacher | cancel an unclaimed invite |
| `DELETE /api/classes/<id>/members/<user>` | its teacher | remove a student |
| `POST /api/classes/<id>/archive` | its teacher | `{archived: true \| false}` |
| `GET /api/classes/<id>/attempts` | its teacher | exams sat for the class |
| `GET /api/attempts/<id>` | the student, or the class's teacher | one exam with its turns |
| `POST /api/join` | anyone signed in | `{code}`, join as a student |

A class that is not the caller's answers 404 rather than 403, so ids cannot be
probed. Anything that changes state is refused from another origin. The route
handlers live in `src/class_api.cpp`, apart from the rest of `Server`, so the
group can move into a separate API service later without untangling it.

## Exam plans, tenses and topics

A teacher controls what an exam for their class covers with **exam plans**,
made on the dashboard (`/teacher`, a class, *New exam plan*). A plan has:

| Part | What it does |
| --- | --- |
| Topics | The syllabus groups the exam may cover. The examiner's topic tags are narrowed to them (the response schema only accepts their tags), and `{{TOPIC_TAGS}}` in the prompt files is filled to match. None ticked is the whole syllabus. |
| Set questions | Questions the examiner must ask, in the class language, each placed *as the opening question*, *while its topic is running* or *whenever it fits*. They are quoted to the examiner word for word, unless the plan allows paraphrase. |
| Tense targets | Tenses the examiner should phrase questions in, and how many times at least. |
| Length, opinion | The exam's length (or the server default), and whether an opinion question is owed. |

A class may name one plan its **default**, which every exam for the class
follows; students can also pick any plan the teacher shows by name from the
**Exam** picker on the speaking page. A student never receives a plan's set
questions before sitting it. Each attempt stores a frozen copy of its plan
(`exam_attempts.plan_json`), so editing a plan changes the next exam and none
of the last.

**How a plan runs.** `Session` gives the examiner at most one order per turn,
in this priority: a set question when the pending ones would otherwise not fit
the time left (estimated from the student's own pace so far), a topic change
(which uses a pending set question on another topic as the way to change), a
set question for the topic running, a tense order when a target is behind, then
the opinion question. A set question counts as asked when the examiner names
its id in the reply *and* the reply shares at least half its words, or on word
overlap alone at 80%. One the examiner ignores three times is given up, and any
still pending when the exam ends are recorded as missed.

**How tenses are measured.** Tenses have language-neutral keys — `present`,
`perfect`, `imperfect`, `future`, `conditional` — and each language pack names
them (passato prossimo, Perfekt). Every turn is labelled twice:

- **model** — the examiner's own reading, from `question_tenses` and
  `answer_tenses` in its structured reply, enum-constrained to the keys and
  described in the language's own terms;
- **rules** — a deterministic check in `src/tense_rules.cpp`: word lists and
  endings for Italian and German, good at the forms a beginner produces and
  never calling the present, which is too ambiguous from endings alone.
  `./build/speaking-sim --check-tenses` runs it over known sentences.

Both are stored in `turn_features` with their source, and the dashboard shows
them side by side: solid where the two agree, dashed where only one found the
tense. The class's **What students have practised** table counts, per student,
the answers in which they used each tense and the questions asked in it.

| Method and path | Who | Does |
| --- | --- | --- |
| `GET /api/classes/<id>/plans` | its teacher / its students | every plan in full / visible plans by name only |
| `POST /api/classes/<id>/plans` | its teacher | create a plan |
| `GET`, `PUT /api/plans/<id>` | its class's teacher | read or replace a plan |
| `POST /api/plans/<id>/archive` | its class's teacher | archive, and stop it being the default |
| `POST /api/classes/<id>/default-plan` | its teacher | `{plan_id}`, or 0 for none |
| `GET /api/exam-options?language=` | anyone signed in | syllabus topics and tense names for the editor |
| `GET /api/classes/<id>/coverage` | its teacher | tenses and topics per student |

A plan is checked when it is saved: known topics and tenses, set questions only
on ticked topics, one opening question at most, and no more set questions than
the exam has room for at about 30 seconds a question.

## Usage limits and paid access

Speaking is metered in **examiner questions**, the thing that costs money: each
one, the opening question included, is reserved in `usage_daily` before the
call in a single statement (so two tabs cannot both take the last one) and
refunded if the examiner fails. A free account gets `FREE_DAILY_QUESTIONS` a
day, a paid one `PAID_DAILY_QUESTIONS`. With none left an exam is refused at
Start, or — if it runs out mid-exam — the last answer is still transcribed and
saved and the exam ends with reason `quota`. The day is the server's local
date: run it with `TZ=Australia/Sydney` so the allowance resets at a student's
midnight.

**Listening is always free and unmetered. Translation is paid** — the translate
box is locked for a free account on both pages, and `/api/translate` answers
403 with `reason: "paid_only"`.

Paid access comes from a **licence** on the account, or on any unarchived class
the student is in (which is how a school licence works: per class, invoiced,
with no card details held anywhere), or from being a teacher. There is no
payment provider yet; licences are granted from the command line against the
same database, safely beside a running server:

```sh
./build/speaking-sim --grant-licence class 12 2027-12-31 "Invoice 1042"
./build/speaking-sim --grant-licence user mia@school.nsw.edu.au 2027-06-30
./build/speaking-sim --list-licences
./build/speaking-sim --revoke-licence 3
```

The routes a script could hammer are rate limited per caller in memory (sign-in
starts, exam sockets, join codes, invites, translation, plan saves), answering
429. That is separate from the daily allowance, which lives in the database.

Anonymous exams, possible only while `AUTH_REQUIRED` is off, are not metered:
there is no account to count against. Production should run with it on.

The schema is still created in one step, so **delete `speaking-sim.db` after
pulling these changes**; the new tables are created on the next start.
