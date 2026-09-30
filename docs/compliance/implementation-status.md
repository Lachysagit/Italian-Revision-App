# Safety screening — what is built

Companion to `compliant-flow.md`, which is the design. This file is the
honest ledger of how much of it exists in the code, so that neither an HSC
marker nor a DoE reviewer has to diff a plan against a branch to find out.

Phase numbering follows section 11 of the design.

---

## Phase 1 — built

| Piece | Where |
|---|---|
| `InterfaceSafety`, `SafetyVerdict`, stages and actions | `include/sim/safety.hpp` |
| Wordlist layer (offline, no network, no key) | `src/safety/wordlist_safety.cpp` |
| `SafetyChain` — ordering, most-severe-wins, fail-closed | `src/safety/safety_chain.cpp` |
| Azure Content Safety + Prompt Shields layer | `src/safety/azure_safety.cpp` |
| `safety_events` table and its three accessors | `src/store.cpp`, `include/sim/store.hpp` |
| Checkpoint 0b — an exam that cannot be screened does not begin | `src/server.cpp`, Start handler |
| Checkpoint 1 — student speech, on all three paths | `Server::screen_student_speech` |
| Checkpoint 3 — examiner reply, after `clean_for_speech` | `src/server.cpp`, worker lambda |
| `SAFETY_MODE` / `SAFETY_FAIL_CLOSED` / severity validation at startup | `src/config.cpp` |
| Seed wordlists for Italian and German, incl. self-harm | `speaking-sim/config/wordlists/` |
| Semantic reasoning pass — policy | `src/safety/semantic_adjudicator.cpp` |
| Semantic reasoning pass — backend | `src/safety/examiner_adjudicator.cpp` |
| Offline tests | `speaking-sim/tests/safety_tests.cpp` |

Three details worth knowing, because each is a decision rather than an
accident:

**Checkpoint 1 runs in three places, not two.** The design names the Whisper
path and the Gemini-listens path. There is a third: when the examiner call
throws, the server falls back to local transcription inside its own `catch`,
paints that transcript and records it. That path is the easiest of the three
to overlook and the least acceptable to miss — a disclosure does not become
less urgent because the turn carrying it happened to fail — so all three go
through one helper rather than three pasted blocks.

**A mask does not short-circuit the chain.** The obvious loop returns on the
first verdict that is not `Allow`. That is wrong for `Mask`: the turn
continues, so the layers behind it still have something to decide, and a
sentence carrying both a swear word and a disclosure would otherwise be
masked and waved through. The chain carries the masked copy forward and keeps
going; only `Halt` and `Escalate` stop it. There is a test for exactly this.

**The semantic pass is a tie-breaker, not an appeal court.** See the section
below; it is the part of this build most worth reading before a reviewer asks.

**`safety_events` holds no sentences.** `matches` holds normalised terms — the
word that fired, not the utterance it sat in. The utterance is already in
`attempt_turns` under that table's access rules, and duplicating it into a
safety log would put the most sensitive text in the least governed place.

---

## The semantic reasoning pass

NSWEduChat's own published description of its safety stack includes a
**semantic content filter** that "checks the meaning behind your words", and an
orchestrator that "applies one more round of semantic and profanity filters" to
the response. A meaning-level stage is therefore not an invention here.

What is an extension is letting that stage **reduce** another layer's verdict.
DoE's semantic filter is described as a filter — it blocks on meaning. Ours
additionally reasons about a trigger our own filters produced, and can clear
it. That is worth stating plainly rather than blurring, because it is the one
place where a reviewer could reasonably ask *"so an AI decides whether to
escalate a child protection concern?"*

The answer is no, and these are the mechanics that make it no:

**The rule.** Adjudication resolves disagreement between detectors. It never
overrides consensus. A self-harm verdict that both the local wordlist and Azure
Content Safety reached is untouchable — no model is asked.

**Self-harm needs four gates, all of them.**

1. **At least two detectors are configured.** One cannot disagree with itself,
   so a lone escalation is the only judgement that exists, and letting a model
   overturn it would be a veto rather than a tie-break.
2. Those detectors actually disagreed.
3. The matched phrase is not marked `!`.
4. A flag separate from the category list is on, which it is not by default.

Gate 1 is what makes the flag genuinely inert under `SAFETY_MODE=local`, where
only the wordlist exists. It was missing from the first version of this code:
the startup warning promised the flag had no effect while the policy let the
review through, because "one detector" and "two detectors that disagreed" both
arrive as `concurring_detectors == 1`. `SafetyChain` now passes its own layer
count to the policy at construction — the chain is what knows — and the test
suite fails if that gate is removed.

**Categories out of reach entirely.** `jailbreak` is never sent to a model: the
text being judged is text that just tried to subvert one, so asking a second
model whether to allow it *is* the attack. `profanity` is never sent either —
an exact match against a curated token list has no ambiguity to resolve. The
tests assert that the backend is not merely overruled in these cases but *never
called*, which is what makes the exclusion an injection defence rather than a
preference.

**Phrases out of reach.** An `escalate.txt` entry prefixed `!` can never be
reduced, whatever a model concludes. The 31 Italian and 27 German entries with
no innocent reading — `voglio morire`, `kill myself`, `togliermi la vita`,
`selbstmord` — all carry it. `hits me` and `es beenden` do not, because "the
song hits me" and "das Spiel beenden" exist.

**Floors.** A cleared verdict falls only so far:

| Category | Floor |
|---|---|
| `self_harm` | `Halt` — never `Allow`, never `Mask` |
| `hate` | `Mask` |
| `sexual`, `violence` | `Allow` |

The worst case of a wrong downgrade on self-harm is a student losing one turn
and getting the question refunded. The worst case of the opposite is a child's
disclosure discarded by a language model. Those are not comparable errors and
the floor says so.

**Everything upholds.** A downgrade needs an affirmative, well-formed,
high-confidence answer with a reason code that permits it. `genuine`,
`ambiguous`, anything below high confidence, an unrecognised code, a malformed
reply, a timeout, a transport error, an unhealthy backend — all uphold. There
is exactly one code path that reduces a verdict.

**Nothing is erased.** A cleared trigger still writes a `safety_events` row:
`action` is what was finally done, `original_action` what the filters had
decided, `adjudication` says `downgraded`. A teacher's view can list cleared
items separately and you can report a clear rate per category. There is no
free-text reason column and there must never be one — a model's explanation of
why an utterance was benign is a paraphrase of that utterance, and it would put
the sensitive text into the one table designed to hold none.

**Where it runs.** Through `InterfaceExaminer`, so it runs on whatever the
examiner runs on and follows it to Australia East rather than needing its own
compliance story. `load_config()` refuses `SAFETY_ADJUDICATOR=examiner` with
`AUTH_REQUIRED` on while the examiner is Gemini: flagged speech is the most
sensitive text this system handles, and offshore is where it must not go.

**Default off.** `SAFETY_ADJUDICATOR=off`, and self-harm review has a second
flag of its own that is also off. In `SAFETY_MODE=local` there is only one
detector, so the consensus gate can never be satisfied and self-harm review has
no effect — the server says so at startup rather than leaving it to be
discovered.

---

## Phase 2 — enforced, not yet implemented

`AUDIO_INPUT=gemini` sends the recording straight to the model, which
transcribes and answers in one call, so there is no transcript to screen
before the model reads it. The cascade (STT as its own stage) is not built.

What *is* built is the gate: `AUTH_REQUIRED=1` with `AUDIO_INPUT=gemini` is
refused at startup with a message saying why. Turning authentication on now
names the missing piece instead of quietly shipping student audio to
`generativelanguage.googleapis.com`.

## Phases 3–6 — not started

Not buildable from the repository alone. Phase 3 needs an Azure subscription
(Speech, OpenAI Australia East, Content Safety, a VNet with private
endpoints); phase 4 needs a DoE Entra tenant; phase 5's escalation wording
needs the school's wellbeing team. `AzureSafety` is written and wired so that
phase 3 is a configuration change plus a health check, not a refactor.

---

## Known gaps in what is built

Listed rather than left for a reviewer to find.

1. **The escalation message is a placeholder.** It names no support service,
   because the design is explicit that the wording and the teacher workflow
   are drafted with the school's wellbeing team first. Marked `TODO(wellbeing)`
   in `Server::safety_notice`. A tool that detects a disclosure and shows
   generic text is only barely better than one that detects nothing.
2. **There is no teacher escalation view yet.** `Store::class_escalations`
   returns the rows; nothing renders them. Until that exists, an escalation
   reaches a teacher only if someone reads the database. This is phase 5 and
   it is the single most important thing to build next.
3. **Notices reuse the error channel.** A masked turn tells the student
   through the same message type as a failure, because that is the only
   mechanism the protocol has. A dedicated notice type would read better.
4. **An escalation on the very last turn of an exam** leaves the attempt
   closed as `escalated`, but a halt on that turn returns before the
   timer/quota path closes the attempt, so it is closed as `disconnect` when
   the socket drops. The event row is still written and still correct.
5. **The reasoning pass has never run against a real model.** Its policy is
   fully tested against stub backends, and `ExaminerAdjudicator` compiles and
   is wired, but nothing here has exercised it against Gemini or Azure OpenAI.
   The prompt wording in `adjudication_system_prompt` in particular is
   untested, and `prewarm()` deliberately refuses to trust a backend that
   cannot recognise "mi piace da morire" as an idiom. Expect to tune the prompt
   the first time it runs for real.
6. **The wordlists are a starting point.** They were seeded from the sources
   the README names and pruned for false positives against realistic exam
   sentences, but they have not been reviewed by a teacher or a wellbeing
   team, and the design requires that review each term with the date recorded
   in the PIA.
