# Backend review log

A full pass over the C++ backend in `speaking-sim/` (`src/`, `include/`), looking
for bugs, repeated code, places where the code and its comments or documents
disagree, and work the backend and the front-end each do separately. Every
finding is listed here with what was done about it.

Paths are relative to `speaking-sim/` unless they start with `docs/`.

## How to read this

- **Section 1** is bugs that were fixed. Each says what went wrong, why, what
  changed, and how the fix was checked.
- **Section 2** is repeated code that was folded into one place. Nothing in it
  changes what the server does.
- **Section 3** is comments and documents that had drifted from the code.
- **Section 4** is the same rule or list held in both C++ and JavaScript.
- **Section 5** is what was found and deliberately left alone, with the reason.
- **Section 6** is how all of it was verified, and what could not be.

## 1. Bugs fixed

### 1.1 An exam could be run without ever sending Start (allowance bypass)

- **Where:** `src/server.cpp`, the Stop branch of `handle_control`.
- **What happened:** a client that opened the websocket, sent audio and then
  `{"type":"stop"}` got a transcript and an examiner question back, with no
  Start at all.
- **Why it mattered:** Start is the only place that checks sign-up is finished,
  class membership, today's question allowance and that the safety chain is
  ready. It is also where the allowance is attached to the session; without it
  the limit stays at -1, which the pipeline reads as "not metered". So a free
  account could ask unlimited examiner questions, and an account that had just
  been refused ("finish signing up", "you have used your 5 questions") could
  carry on by answering anyway.
- **Fix:** `Session` now records whether an exam has started
  (`mark_exam_started`, set only after every Start check has passed). `handle_stop`
  refuses an answer on a session with no running exam, with an error and the
  `refused` status the page already understands.
- **Checked:** reproduced on the unmodified build (question returned), then on
  the new build (refused), for a fresh socket and for a not-onboarded account.

### 1.2 An exam could be continued after it had ended

- **Where:** same branch.
- **What happened:** after the End button, an escalation, the clock running out
  or the allowance running out, the server kept accepting answers. On the
  unmodified build an escalated exam ("This practice has been stopped") answered
  the next Stop by screening and escalating it again, and an exam closed with
  End went straight back to asking questions, writing turns onto an attempt row
  already marked finished.
- **Fix:** every way an exam ends now goes through one function,
  `Server::end_exam`, which marks the session over as well as closing the
  attempt row. `handle_stop` refuses an answer on a session that is over.
- **Checked:** Start, End, then audio and Stop: old build asks another
  question, new build refuses. Same for an escalated exam and a timed-out one.

### 1.3 A halted answer left the student on a dead microphone

- **Where:** `src/server.cpp`, the three places the pipeline screens a
  transcript.
- **What happened:** when the safety chain halted an answer, the job sent the
  notice and returned. The page only re-arms the microphone when it receives an
  `examiner_text` or a status, so nothing re-armed it. The notice says "Please
  try answering the question again", but the student could not.
- **Fix:** a halted turn now ends the same way an unheard one does: an empty
  `examiner_text`, which re-arms the microphone. `screen_student_speech` now
  tells its caller which of the two happened (`TurnStopped` for a halt,
  `ExamStopped` for an escalation) so the caller can hand the turn back only
  when the exam is still running.
- **Also fixed by this:** known gap 4 in
  `docs/compliance/implementation-status.md`. A halt on the final answer used to
  skip the timer close, so the attempt was filed as `disconnect`. It is now
  closed as `timer` (or `quota`) and the page is sent `ended`.
- **Checked:** with a wordlist that halts every answer, the old build sends the
  notice only; the new build sends the notice and then the re-arm, and on a
  final answer the notice and then `ended`.

### 1.4 A plan length over 32 bits was saved as a different length

- **Where:** `src/plan_json.cpp`, `plan_from_json`.
- **What happened:** `duration_seconds` was narrowed to `int` before it was
  checked. `4294967596` wraps round to `300`, passed the bounds check and was
  saved as a five minute exam.
- **Fix:** the bounds are checked on the full 64-bit value and the cast happens
  afterwards. `min_count` on a tense target is clamped the same way.
- **Checked:** old build answers 201 and stores the plan; new build answers 400
  with the real number in the message.

### 1.5 On an https deployment the session cookie was sent twice

- **Where:** `src/server.cpp`, `serve_auth_callback`.
- **What happened:** the cookie was set once without `Secure`, then set again
  with it when `PUBLIC_ORIGIN` is https. Each `set_cookie` call adds its own
  `Set-Cookie` header, so the browser was sent the session token twice, one copy
  of it not marked `Secure`.
- **Fix:** one cookie, with `Secure` added to it when the origin is https.
- **Checked:** by reading Crow's `CookieParser` (each call appends to the list
  it later writes out). Not exercised end to end, because that needs a real
  Google sign-in.

### 1.6 A missing or dead piper could kill the whole server (Linux)

- **Where:** `src/tts/piper_tts.cpp` (`run_piper`) and `src/main.cpp`.
- **What happened:** the reply is written to the piper child's stdin. If the
  child has already exited - no binary at the compiled path, a bad voice file -
  that write raises `SIGPIPE`, and the default action for `SIGPIPE` is to
  terminate the process. The code already handles the write failing, but the
  signal arrives first.
- **Fix:** `main` ignores `SIGPIPE` on non-Windows builds, so the write fails
  with an error the existing code turns into "this turn has no audio".
- **Checked:** by reasoning only. This container has no piper submodule, so the
  subprocess path is not compiled here.

### 1.7 A JSON frame that was not an object was logged as a "Worker Crash"

- **Where:** `src/protocol.cpp`, `from_json`; also `src/server.cpp`
  `serve_translate`, `serve_set_profile`, `src/plan_api.cpp` and
  `src/auth/google_oauth.cpp`.
- **What happened:** Crow's `rvalue::has()` throws on anything that is not an
  object or a list. A websocket text frame of `42` or `"hello"` is valid JSON,
  so it reached `has()` and the exception escaped the handler. Crow caught it
  one level up and logged `Worker Crash: An uncaught exception occurred`. The
  same body sent to a JSON route came back as a 500 instead of a 400.
- **Fix:** `from_json` returns an unreadable message for anything that is not
  an object. The routes read their fields through the shared `string_field` /
  `bool_field` / `int_field` helpers in `http_util`, which all start with the
  same is-it-an-object check. The decoded Google token and its claims are
  checked the same way.
- **Checked:** `42`, `"text"`, `[1,2]` and `{"type":7}` over the websocket: one
  "Worker Crash" line on the old build, none on the new.

### 1.8 Several database writes ignored their own result

- **Where:** `src/store.cpp`: `upsert_google_user`, `delete_auth_session`,
  `user_by_id` and the other account and attempt methods in the first half of
  the file.
- **What happened:** these prepared, bound, stepped and finalised statements by
  hand. `upsert_google_user` never looked at what `sqlite3_prepare_v2` returned
  and ignored the result of the UPDATE and of the identity link, so a failed
  write during sign-in went by silently. `delete_auth_session` (logout) did the
  same.
- **Fix:** all of them now use the `Statement` wrapper the second half of the
  file already used, which throws on a failed prepare or step. See 2.2.
- **Checked:** the 78-call API comparison in section 6, plus sign-up, exam and
  licence flows against a real database file.

### 1.9 A failed transaction reported the wrong error

- **Where:** `src/store.cpp`, the eight methods that open a transaction.
- **What happened:** each rolled back with `exec("ROLLBACK")`, which throws if
  no transaction is open. `upsert_google_user` re-read the user inside the `try`
  after its COMMIT, so a failure there ran ROLLBACK with nothing to roll back
  and replaced the real error with "cannot rollback - no transaction is active".
- **Fix:** one `transaction()` helper that rolls back without throwing and
  rethrows the original error. The read-back in `upsert_google_user` moved
  outside the transaction.

### 1.10 `Range: bytes=<very long number>-` was a 500

- **Where:** `src/static_files.cpp`, `parse_range_header`.
- **What happened:** `std::stoll` throws on a number that does not fit in 64
  bits, and nothing caught it.
- **Fix:** offsets are limited to 18 digits before conversion; a longer one is
  treated as a Range header this server does not understand, and the whole file
  is served.
- **Checked:** by reading. The listening clips are not in the repository, so
  there is no file to request a range of here.

### 1.11 Settings with a typo silently fell back to the default

- **Where:** `src/config.cpp`.
- **What happened:** `EXAMINER_BACKEND`, `AUDIO_INPUT` and `AUDIO_CODEC` accepted
  any text and treated everything unrecognised as the default, with nothing in
  the log. `AUDIO_INPUT=wisper` therefore meant `gemini`, which is the mode that
  sends the recording to the model before anything is screened. Separately,
  `LANGUAGE_VOICES` and `GEMINI_API_KEYS` were split on commas without trimming,
  so `italian=a.onnx, german=b.onnx` produced a language called `" german"`,
  which matched nothing and dropped that voice without a word.
- **Fix:** the three settings go through `checked_choice`, which keeps the
  fallback but prints what was wrong. The lists are trimmed.
- **Checked:** started the server with all of those mistakes; each is reported
  and both voices resolve.

### 1.12 Smaller ones

| Where | Problem | Fix |
|---|---|---|
| `src/main.cpp` | Any unrecognised option (`--purge-nwo`) was ignored and the full server started. | Refused with `unknown option`, exit code 2. |
| `src/main.cpp` | `--grant-licence ... 2027-13-45` passed the date check and failed inside SQLite as `NOT NULL constraint failed: licences.ends_at`. | The month and day ranges are checked, so it is refused as a bad date. |
| `src/server.cpp` | `serve_set_profile` called `is_in_any_class` outside any `try`, and `serve_me` did its session lookup the same way. | Both go through the shared guarded paths. |
| `src/server.cpp` | A second Start was detected by `attempt_id != 0`, which is 0 when the attempt row could not be written, so that exam accepted a second Start. | Asked of the new started flag instead. |
| `src/protocol.cpp` | A fractional `class_id` such as `12.9` was truncated to 12; the comment said a fraction meant "no class". | A fraction now reads as 0, as the comment promised. |
| `src/plan_api.cpp` | The request body was parsed twice per save, once outside any `try`. | Parsed once, inside `guarded()`. |
| `src/plan_api.cpp` | `saved = *store_->plan_by_id(...)` dereferenced an optional without checking it. | Checked. |
| `src/tts/piper_tts.cpp` | Compiler warning: `pcm_from_raw` defined but not used when piper is not built. | Compiled only where it is used. |

## 2. Repeated code folded into one place

None of these change behaviour. Net effect across the backend: about 180 fewer
lines, after counting the new shared header and the added checks.

### 2.1 String helpers: new `include/sim/text_util.hpp`

Header-only, no dependencies.

| Helper | Replaces |
|---|---|
| `trim` | Private `trimmed`/`trim` in `class_api.cpp`, `plan_json.cpp`, `question_bank.cpp`, and inline versions in `config.cpp` (twice) and `text_clean.cpp`. |
| `to_lower_ascii` | Lowercase loops in `http_util.cpp`, `server.cpp`, `session.cpp`, `text_clean.cpp`, `config.cpp`, `class_api.cpp`. |
| `split_list` | Three hand-written comma splitters in `config.cpp`. |
| `join` | Join loops in `session.cpp` (twice) and `store.cpp`. |
| `read_file` | Whole-file reads in `static_files.cpp` (twice), `language.cpp`, `piper_tts.cpp`. |

### 2.2 Database layer: `src/store.cpp`

- **One statement wrapper.** `Statement` moved to the top of the file and every
  query uses it. About 200 lines of hand-written prepare/bind/step/finalize went.
  It gained `blob`, a nullable `int64`, `col_bool`, `col_null` and a
  `std::string` constructor.
- **One transaction helper.** `transaction(db, body)` replaces eight copies of
  BEGIN / try / COMMIT / catch / ROLLBACK.
- **One row loop.** `all_rows(stmt, read)` replaces the repeated
  `while (stmt.row()) push_back(read(stmt))`.
- **Timestamps.** `CAST(strftime('%s','now') AS INTEGER)` appeared 31 times. It is now SQLite's own `unixepoch()`, which returns the same integer.
  This needs SQLite 3.38 or later; the build pins 3.50.4.
- **Bound values instead of pasted ones.** `record_turn`, `note_examiner_call`
  and the usage purge built SQL by concatenating integers. They were safe (the
  values never came from a client) but are now bound like everything else.

### 2.3 Server: `src/server.cpp`, `include/sim/server.hpp`

- **Page routes.** Sixteen identical `CROW_ROUTE` blocks, one per file, became
  one table (`kPageAssets`) and a loop.
- **`handle_control` split.** The 170-line Start branch is `handle_start`, the
  Stop branch is `handle_stop`, and the dispatcher is a `switch`.
- **`end_exam`.** Six copies of "if the attempt is recorded, close it quietly
  with this reason".
- **`finish_without_question` and `stopped_by_screening`.** The three screening
  call sites and the two "no question this turn" exits in the pipeline job.
- **`fail_turn`.** The two `catch` blocks had the same four lines.
- **`send_message`.** `send_status`, `send_error` and `send_transcript` each
  built the same message by hand.
- **`known_tenses`.** The "keep only real tense keys" loop, written twice.
- **JSON answers.** Five routes built a response and set headers by hand instead
  of calling `json_response`. See 5.6 for the one visible effect.

### 2.4 Routes: `src/class_api.cpp`, `src/plan_api.cpp`, `src/http_util.cpp`

- `json_ok()` for the four routes that answer `{"ok": true}`.
- `bool_field()` and `query_param()` beside the existing `string_field` and
  `int_field`.
- `attempts_response()` for the two attempt lists.
- `plan_response()` and `Server::plan_taught_by()` for the plan routes, which
  each repeated the "does this plan exist and does the caller teach its class"
  check.
- In `serve_attempt`, the answer block written twice is `add_answer`, and the
  condition written twice is `show_progress`.

### 2.5 Session: `src/session.cpp`, `include/sim/session.hpp`

- `plan_topics()` replaces three copies of "the plan's topics, minus unknown
  ones".
- `topic_covered()` replaces three copies of a `std::find` over
  `covered_topics_`.
- `pending(state)` was a one-line wrapper for `!state.done` and is gone.
- `required_directive` rebuilt the `"q<id>"` key that `set_plan` had already
  stored; it uses the stored one.
- `FilledPrompt` carried a field nothing read; `fill_topics` returns the text.
- `rng_` was `mutable` for a function that is no longer `const`.

### 2.6 Smaller modules

- `src/language.cpp`: `LanguagePack::tense_label()` / `tense_example()` replace
  lookup loops in `session.cpp` and `plan_api.cpp`.
- `src/topics.cpp`: `topic_menu` rebuilt the list of syllabus groups that
  `all_topic_groups()` already returns.
- `src/auth/google_oauth.cpp`: `base64url_unpadded()` for the two places that
  encoded and then stripped the padding.
- `src/main.cpp`: the four command-line blocks, each with its own `try`, are one
  `run_command`.
- `src/protocol.cpp`: `from_json` read each field with its own copy of the
  presence-and-type check.
- `src/auth/session_cookie.cpp`: deleted. It was an empty namespace whose
  comment said it would be filled in "at the OAuth phase"; that code lives in
  `store.cpp`.

### 2.7 The code-simplifier pass

After the changes above, the modified files were given to a review agent
running the instructions of the official `code-simplifier` plugin. The plugin
was installed in the session but its own agent type could not be loaded
mid-session, so its instructions were handed to a standard agent instead. It
made three small edits and left the rest alone:

- `src/protocol.cpp`: `from_json` reads its text fields with the shared
  `string_field` instead of a local copy of it.
- `src/store.cpp`: two anonymous namespaces that had ended up side by side were
  merged.
- `src/store.cpp`: four SQL strings left split across lines by the timestamp
  change were rejoined.

It noted, and did not change, that `plan_json.cpp` still reads its fields by
hand rather than through `http_util`; that file deliberately avoids including
all of Crow.

## 3. Comments and documents that disagreed with the code

| Where | What it said | What is true |
|---|---|---|
| `include/sim/protocol.hpp`, `final` | "the server keeps no clock of its own" | It does (`Session::time_up`); the comment two fields up says so. Rewritten, and the two interleaved comment blocks above it untangled. |
| `include/sim/session.hpp`, `rng_` | "mutable because build_examiner_input() is const" | It is not const. |
| `include/sim/session.hpp` | A comment about the clock mutex sat under `last_tense_order_`, and the "zero means none" comment sat under `question_limit_`, whose "none" is -1. | Moved to the members they describe. |
| `include/sim/server.hpp`, `record_student_turn` | Five lines describing `send_examiner_text`. | Moved there. |
| `src/auth/google_oauth.cpp` | The first name "is what the examiner calls the student out loud". | The name never reaches the examiner (see the note on `Message`). |
| `src/config.cpp` | `gemini_api_key` "is what main.cpp hands to the constructor". | `main` passes the key pool. |
| `src/language.cpp` | "ASCII transliterations throughout". | `tense_labels` keeps its umlauts, deliberately. |
| `src/session.cpp` | Refers to `system_prompt_`. | No such member any more. |
| `docs/speaking-sim-dev-notes.md` | The pipeline can be driven by streaming audio and sending Stop. | That now needs a Start first (1.1). Updated. |
| `docs/compliance/implementation-status.md`, gap 4 | A halt on the last turn leaves the attempt closed as `disconnect`. | Fixed by 1.3. Updated. |

## 4. The same thing held in both C++ and JavaScript

"JS" is `web(frontend)/`. Only the first row was changed, because it could be
fixed inside the backend. The rest are listed so they can be decided on;
changing them means editing the pages.

| What | C++ | JavaScript | State |
|---|---|---|---|
| Which languages can be translated | `server.cpp` had its own `it`/`en`/`de` list | `translate.js` `TRANSLATE_LANGUAGES` | **C++ side fixed:** now read from the language registry, so a new language cannot be forgotten there. The JS list is still separate. |
| The list of exam languages | `language.cpp` (the source), served by `/api/languages`; `static_files.cpp` `is_known_language` holds a second copy for the listening page | `listening.js` `LANGUAGES`, `<option>`s in `listening.html`, `DEFAULT_LANGUAGE` in `client.js` | Four copies. `client.js` already reads `/api/languages`; `listening.js` could do the same. |
| Year levels and subject levels | `server.cpp` `is_valid_year_level`, `is_valid_subject_level` | `<option>` lists written out three times in `account.js` | No route serves them. A comment in `store.hpp` said years were `'9'..'12'` while both sides accept 7 to 12; the comment is corrected. |
| How many set questions fit in an exam | `plan_json.cpp` `questions_that_fit`, and the refusal sentence | `teacher.js` `paintFit` redoes the sum and words the same sentence | The page's copy is a live preview; the server's is the rule. They agree today. |
| The "translation is paid" sentence | `server.cpp` `serve_translate` | `translate.js` `TRANSLATE_LOCKED_TEXT`, word for word | The server already sends it with `reason: paid_only`. |
| Capitalising a topic group | `topics.cpp` `topic_menu` | `teacher.js` `capitaliseTopic` | Harmless. |
| Join code formatting | `store.cpp` `normalise_join_code`, and the `/join/<code>` filter in `server.cpp` | `teacher.js` `formatCode` adds the hyphen | Intended: the server ignores the hyphen. |
| Length limits (80, 300, 1000 characters) | `class_api.cpp`, `plan_json.cpp`, `server.cpp` | `maxlength` in the HTML and `teacher.js` | Commented as mirrors on both sides. |
| Capture sample rate 16000 | `Session::kCaptureSampleRate` | `client.js` `CAPTURE_SAMPLE_RATE` | Must match; nothing enforces it. |
| The exam countdown | `Session` clock | `client.js` timer | By design: the server's is the one that counts. |
| A language's display name on an attempt | `class_json` sends `language_label`; `attempt_json` does not | `classes-page.js` works it out by capitalising the id | The server knows the label and could send it. |

Fields the backend sends that no page reads: `usage.paid_until`, member
`added_at`, plan `updated_at`.

## 5. Found and left alone

1. **No teacher sees an escalation.** `Store::class_escalations` exists and no
   route calls it, while the student is told "Your teacher has been notified".
   This is already known gap 2 in `docs/compliance/implementation-status.md`. It
   is a feature to build, not a cleanup, and the wording is marked for the
   school's wellbeing team.
2. **The escalation notice may not be shown on the page.** The server sends it
   on the error channel followed by `ended`. `client.js` stores an error in
   `lastServerError` and shows it only for `refused` and `quota`; for `ended` it
   logs "time is up". That is a front-end fix and was outside this pass. Given
   that the notice is the child-safety message, it is worth doing next.
3. **`HailoExaminer` is a stub.** `EXAMINER_BACKEND=hailo` answers every turn
   with "placeholder examiner question".
4. **Unused schema.** Tables `rooms`, `room_members`, `room_turns` and
   `profile_change_requests`, and columns such as `daily_attempt_cap` and
   `score_*`, are created and never written. They look planned. Removing them
   would change the schema, and `migrate()` only ever creates it from scratch.
5. **`Session::fact_store_`** is an unused member marked "STUB for now".
6. **Cache header on two public lists.** `/api/gemini-keys` and `/api/languages`
   now go through `json_response`, so they carry `Cache-Control: no-store` like
   every other JSON answer. Bodies are unchanged.
7. **`src/safety/` was read but not edited.** It has its own test suite (which
   passes) and it is the part where a "tidy-up" is least worth the risk.
   `SafetyChain::last_adjudication` was checked specifically, since one chain is
   shared by every worker: it is `thread_local`, so it is safe.
8. **The id token's signature is not verified** in `google_oauth.cpp`. The
   header explains why (it arrives directly from Google over TLS). Noted, not
   changed.
9. **The translate API key travels in the URL** of the outbound call. That is
   how Cloud Translation v2 takes it.
10. **When Gemini fails on audio and whisper rescues the transcript**, the
    student sees their words but that turn is not written to `attempt_turns`.
    The student is asked to answer again, so storing it would duplicate the
    turn. Left as it is.

## 6. How this was verified, and what was not

**Built** on Linux with GCC 13 and `-Wall -Wextra`: no warnings in `src/` or
`include/`.

**Self-checks:** `safety-tests`, `--check-tenses` and `--check-audio-encode` all
pass.

**Old against new.** The unmodified server and the new one were started side by
side on fresh databases with the same seeded accounts, and the same 78 HTTP
calls were sent to each: every page asset, every class, plan, invite, attempt,
profile, translate and sign-in route, as a teacher, a student and a
not-yet-onboarded account, including malformed bodies. The answers were
identical except for the plan-length bug in 1.4.

**Exam flow over the websocket**, in `GEMINI_DRY_RUN` mode: a normal turn, a
second Start, pause and resume, a final answer, Stop with no Start, Stop after
End, junk frames, a halted answer, an escalated answer, and a signed-in student
sitting a planned class exam until the daily allowance ran out. The attempt,
set-question and usage rows were read back afterwards.

**Command line:** every licence, export, purge and delete command, including the
refusal to delete a teacher who still owns a class.

**Not verified:**

- **Windows / MSVC.** The project is normally built there; this pass was built
  only on Linux.
- **Whisper and piper.** The submodules were not present, so the real
  speech-to-text and text-to-speech paths did not compile here. 1.6 was fixed by
  reasoning.
- **A real Gemini, Google sign-in or Azure call.** There are no keys in this
  environment. 1.5 was checked against Crow's source, not a browser.
- **The pinned SQLite.** `sqlite.org` is not reachable from here, so the build
  used SQLite 3.54.1 from another source in place of the pinned 3.50.4. Both
  have `unixepoch()`.
