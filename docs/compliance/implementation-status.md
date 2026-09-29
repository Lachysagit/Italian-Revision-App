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

**`safety_events` holds no sentences.** `matches` holds normalised terms — the
word that fired, not the utterance it sat in. The utterance is already in
`attempt_turns` under that table's access rules, and duplicating it into a
safety log would put the most sensitive text in the least governed place.

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
5. **The wordlists are a starting point.** They were seeded from the sources
   the README names and pruned for false positives against realistic exam
   sentences, but they have not been reviewed by a teacher or a wellbeing
   team, and the design requires that review each term with the date recorded
   in the PIA.
