# speaking-sim — compliant flow and NSW DoE approval path

Requirement ids (A1–A9, B1–B7, C1–C8) refer to the non-negotiables checklist in
*Getting speaking-sim Into a NSW Public School*, 28 Sep 2026.

---

## 1. What blocks approval today

Taken from the current behaviour of the branch, not from the plan.

| Current behaviour | Breaks | Why |
|---|---|---|
| FLAC audio to `generativelanguage.googleapis.com` with an API key | A4, B7, C1 | Disclosure outside NSW with no s 19(2) condition met. It is also an *unapproved* AI tool under DoE's generative AI guidance, which is a separate failure from residency |
| Free-tier key: prompts may be used to improve Google's products | C4 | No-training has to be contractual, not a tier side effect |
| Translation box → Cloud Translation v2, not region locked | A4 | Same disclosure, smaller payload |
| Google sign-in, personal Google accounts | C5 | Identity must be the DoE account. A student signing into a school exam tool with a personal Google identity is its own finding |
| No filtering of any kind, either direction | B1, C7 | The Safe AI Ethics Assessment is precisely about this |
| Nothing recorded when something goes wrong | A6, C7 | Nothing to show a reviewer, nothing to notify DoE from |
| No collection notice, no consent record | A2, A5, B2 | |
| SQLite file on the dev box; no retention rule on transcripts | A3, A7, C1 | Transcripts of school assessment practice are State records |
| Audio never stored | — | Already right. Keep it, and say so first in every conversation with the school |

Two things the current design gets right and should be defended in the
documentation: audio is discarded after the turn, and the examiner is sent only
the previous question and answer, never the email, class or account. That is A1
(collect only what is necessary) already satisfied at the model boundary.

---

## 2. The structural change: cascade, not audio-to-model

`AUDIO_INPUT=gemini` sends the raw recording to the model, which transcribes and
answers in one call. That is one API call instead of two and it is why the
turn is as fast as it is. It is also incompatible with the department's filter
model, and not for a policy reason — for a mechanical one:

**there is no text to screen before the model sees it.** The filter that is
supposed to run *between* the student and the LLM has nothing to run on, because
the LLM is the transcriber. The best you can do is screen after the fact, by
which point the unscreened words are already in the model's context and its reply
is already generated from them. Prompt Shields, which is the jailbreak layer,
takes text only.

So the compliant build is a cascade, and STT becomes a separate, screened stage:

```
mic ──► 16 kHz PCM ──► Azure Speech STT ──► transcript
                       (private endpoint)       │
                                                ▼
                                    ① SafetyChain(StudentSpeech)
                                                │ allow / mask
                                                ▼
                                    Azure OpenAI, Australia East
                                    (service endpoint, text only)
                                                │
                                                ▼
                                    ② SafetyChain(ExaminerReply)
                                                │ allow
                                                ▼
                        clean_for_speech ──► socket ──► Piper (local) ──► audio
                                                │
                                                └──► store: turn + features
```

`AudioInput::Gemini` does not disappear; it becomes the development-only mode,
gated so it cannot be selected when `AUTH_REQUIRED` is on. The `InterfaceExaminer`
abstraction already makes this a backend swap rather than a rewrite — that is the
argument to make in the HSC evaluation.

Cost consequence, for the running-cost model: two calls per turn instead of one,
plus two Content Safety calls. Content Safety is billed per 1,000 text records
and the text is one short utterance, so it is noise next to the examiner. The
real cost is latency: budget 150–400 ms per Content Safety hop, twice a turn.
Mitigation is in the code already — the examiner text goes to the socket before
Piper runs, so the screening sits in a gap the student is already waiting through.

---

## 3. The seven checkpoints in a turn

| # | Where | What runs | On failure |
|---|---|---|---|
| 0 | WebSocket handshake | origin check, DoE session cookie, rate limit | connection refused |
| 0b | `start` message | `SafetyChain::ready()` | exam does not begin; C7 says an unscreened exam is not a degraded exam, it is no exam |
| 1 | after STT | wordlist → Prompt Shield → Content Safety, stage `StudentSpeech` | mask / halt / escalate, below |
| 2 | system prompt | topic constraint already in `prompts/<language>/*.txt` | — |
| 3 | after examiner call | wordlist → Content Safety, stage `ExaminerReply` | halt, one regeneration, then end the turn |
| 4 | before the socket | `clean_for_speech` (existing) | — |
| 5 | before Piper | nothing new; Piper speaks only screened text by construction | — |
| 6 | before the store | masked transcript written, raw never written | — |

Layer-for-layer against NSWEduChat: system prompt, jailbreak prevention,
profanity filter run twice, semantic content filter halting the interaction,
post-response pass. Cite that mapping directly in the assessment request — it is
the strongest single paragraph you have, because it says *we are doing what the
department already decided was sufficient*.

### What each action does to the session

**Mask** — student profanity. The masked transcript goes to the socket, the
store and the examiner. The turn continues. The student sees a one-line notice.
Rationale for the PIA: ending a language exam over a swear word punishes the
disfluent, and the examiner reading it back would be worse.

**Halt (jailbreak or semantic)** — the examiner is not called, the reserved
question is refunded exactly as an examiner failure already refunds it, the
student is told the turn was stopped, the exam continues. Three halts in one
attempt ends the attempt with reason `safety`.

**Halt (examiner reply)** — one silent regeneration at the same turn index. If
the second reply also fails, the turn ends with the existing fixed failure
string. The student never learns that the examiner said something unusable,
which is correct: that is an operator fact, not a pedagogical one.

**Escalate** — the turn stops, the attempt ends with reason `escalated`, the
student sees a fixed wellbeing message naming the school's own supports, and a
flag appears on the teacher's class view. No automated notification, no email,
no logging of the text beyond the `safety_events` row. Draft the message and the
teacher workflow with the school's wellbeing team before this ships; a tool that
detects a disclosure and does nothing visible is worse than one that does not
detect it.

---

## 4. Files

```
include/sim/safety.hpp                    InterfaceSafety, SafetyVerdict, actions
include/sim/safety/wordlist_safety.hpp    local layer
include/sim/safety/azure_safety.hpp       Content Safety + Prompt Shields
include/sim/safety/safety_chain.hpp       ordering and fail-closed policy
src/safety/wordlist_safety.cpp
src/safety/azure_safety.cpp
src/safety/safety_chain.cpp
config/wordlists/<language>/*.txt         deployment data, gitignored
```

`CMakeLists.txt`:

```cmake
    src/safety/wordlist_safety.cpp
    src/safety/azure_safety.cpp
    src/safety/safety_chain.cpp
```

---

## 5. Insertion points in `server.cpp`

Two edits inside the worker lambda, both in the region that currently runs
`respond_to_audio` and `clean_for_speech`.

**① after the transcript exists, before `record_student_turn`** (around the
`if (!answer.transcript.empty())` block, ~line 1610, and the equivalent in the
Whisper path around line 1526):

```diff
                 if (!answer.transcript.empty()) {
-                    send_transcript(handle, answer.transcript);
+                    SafetyVerdict verdict = safety_->screen(
+                        answer.transcript, SafetyStage::StudentSpeech,
+                        language->id);
+                    record_safety(*session, verdict, SafetyStage::StudentSpeech);
+
+                    if (verdict.action == SafetyAction::Escalate) {
+                        end_attempt_safely(*session, handle, "escalated");
+                        return;
+                    }
+                    if (verdict.action == SafetyAction::Halt) {
+                        store_->release_usage(*user_id, kSpeakingFeature);
+                        send_error(handle, safety_notice(verdict));
+                        return;
+                    }
+                    answer.transcript = std::move(verdict.text);
+                    //masked from here on: the socket, the store and the
+                    //examiner history all see the same words
+
+                    send_transcript(handle, answer.transcript);
                     student_turn =
                         record_student_turn(*session, answer.transcript, stt_ms);
                     session->record_answer(answer.transcript);
                 }
```

**② after `clean_for_speech`, before `record_question`** (~line 1648):

```diff
             reply = clean_for_speech(raw);
+
+            SafetyVerdict outgoing = safety_->screen(
+                reply, SafetyStage::ExaminerReply, language->id);
+            record_safety(*session, outgoing, SafetyStage::ExaminerReply);
+            if (outgoing.action != SafetyAction::Allow) {
+                throw std::runtime_error("examiner reply failed screening");
+                //the existing catch(...) backstop sends the fixed failure
+                //string and recovers the session. One regeneration is a later
+                //refinement; failing the turn is the safe first version
+            }
+            //screened after cleaning, not before: the student hears the
+            //cleaned text, so the cleaned text is what has to be clean
 
             session->record_question(reply);
```

`Server` gains `std::shared_ptr<SafetyChain> safety_`, wired in `main.cpp`
beside the examiner, and `main()` refuses to start when `config.auth_required`
is set and `safety->ready()` is false.

---

## 6. Config

```
SAFETY_MODE=azure|local|off      off is rejected when AUTH_REQUIRED=1
SAFETY_WORDLIST_DIR=config/wordlists
CONTENT_SAFETY_ENDPOINT=https://<resource>.cognitiveservices.azure.com
CONTENT_SAFETY_HALT_SEVERITY=2
SAFETY_FAIL_CLOSED=1
SAFETY_SHIELD_PROMPTS=1
```

No key in `.env` in the deployed configuration. The relay's managed identity
holds `Cognitive Services User` on the Content Safety resource, the same way it
reaches Key Vault. Validate `SAFETY_MODE` and `CONTENT_SAFETY_HALT_SEVERITY` at
startup with the existing `gemini_thinking_level` pattern — an unchecked value
here is a failure on every turn.

Add the Content Safety private endpoint and its private DNS zone to the VNet
alongside the Speech one. Content Safety is a Cognitive Services resource, so it
shares `privatelink.cognitiveservices.azure.com` with Speech: one zone, two A
records, no second zone charge. The deny-all NSG egress rule needs no new
outbound rule for it, which is the point of keeping it inside.

---

## 7. Store additions

```sql
CREATE TABLE safety_events (
    id          INTEGER PRIMARY KEY,
    attempt_id  INTEGER NOT NULL REFERENCES exam_attempts(id),
    turn_index  INTEGER NOT NULL,
    stage       TEXT NOT NULL,     -- student_speech | examiner_reply
    action      TEXT NOT NULL,     -- allow | mask | halt | escalate
    category    TEXT NOT NULL,
    severity    INTEGER NOT NULL,
    detector    TEXT NOT NULL,
    matches     TEXT NOT NULL,     -- comma-separated normalised terms
    created_at  INTEGER NOT NULL
);
CREATE INDEX safety_events_attempt ON safety_events(attempt_id);
```

```cpp
void record_safety_event(std::int64_t attempt_id, int turn_index,
                         const char* stage, const SafetyVerdict& verdict);
std::vector<SafetyEvent> attempt_safety_events(std::int64_t attempt_id);
std::vector<SafetyEvent> class_escalations(std::int64_t class_id);
```

The row holds no sentence. `matches` holds normalised terms — the word that
fired, not the utterance it sat in. The utterance is already in `turns`, under
the access rules that table has, and duplicating it into a safety log would put
the most sensitive text in the least governed place.

Retention, which A3 and A7 both want an answer for:

* audio — never written. Already true.
* transcripts — kept for the school year, then deleted or exported. Parameterise
  it (`TRANSCRIPT_RETENTION_DAYS`) so the school's records officer sets the
  number, not you.
* safety events — kept as long as the attempt they belong to.
* logs — no transcripts today. Keep it that way; add an explicit assertion to
  the review checklist.

Also needed for A7: a per-attempt export (JSON and PDF) so DoE can recover a
record, and a documented deletion path for a withdrawn consent.

---

## 8. Identity, consent, and the things code cannot fix

* **C5** — Google OAuth out, Entra ID / DoE SSO in. Mechanically this is a swap
  inside `auth/google_oauth.cpp`: same authorisation-code flow, different
  issuer, different claims. `upsert_google_user` becomes `upsert_sso_user` and
  the `google_sub` column becomes `sso_subject`. Teacher status stops coming
  from `TEACHER_EMAILS` and starts coming from a group claim.
* **A2** — a collection notice on the page before the first recording: what is
  collected (audio held in memory only, transcripts, scores), why, who receives
  it (Azure Speech and Azure OpenAI, both Australia East), how to access and
  correct it. It must appear before the microphone is armed, not in a footer.
* **A5, B2** — parental consent per app, on DoE's own template, naming the
  storage country. Not self-consent, even for 17-year-olds.
* **C6** — no mark ever counts. The existing design already does conversation
  only and defers correction; keep it that way and label the teacher view
  "AI feedback, teacher decides". This is the single largest factor in whether
  the AI Review Committee gets involved.
* **A9** — do not infer anything from accent, and do not build speaker
  identification. A voiceprint would convert the recording into sensitive
  biometric information and change the legal picture entirely.

---

## 9. Layer → requirement map

| Component | Satisfies |
|---|---|
| Cascade with STT before the LLM | B1, C7 — makes input screening possible at all |
| Wordlist layer | C7; and the offline build's only layer |
| Prompt Shields | C7, jailbreak prevention as per NSWEduChat |
| Content Safety, halt at severity 2 | C7, B1 |
| Escalation to teacher | Child Safe Standard 8, C7 |
| `safety_events` + per-attempt export | A6, A7, C7 (teacher-visible logs) |
| Fail-closed chain + `ready()` at Start | B6, C7 |
| Azure Speech + Azure OpenAI, Australia East, private/service endpoints | A4, C1, B7 |
| Audio discarded after the turn | A1, A3 |
| Minimal model payload (no email, class, account) | A1 |
| DoE SSO | C5 |
| Collection notice + consent | A2, A5, B2 |
| Retention parameters + export | A3, A7 |
| Advisory-only scoring | C6, B3 |
| Managed identity, Key Vault, deny-all egress, MFA on the portal | B5, B6 |

Unsatisfied by any code change, and worth stating plainly in the request rather
than leaving for a reviewer to find: **C8** — a legal entity able to sign DoE
contract terms and carry insurance. A Year 12 student is not one. The route
around it is a free pilot owned by the school under principal-accepted risk,
with you named as the developer, not a vendor contract.

---

## 10. The approval path

Nothing here starts with DoE. It starts with your Italian teacher.

1. **Now, for the HSC project.** No approval needed while you are the only user
   and the voices are yours or synthetic. Document the compliance design anyway
   — a designed-for-approval system with a written PIA is stronger Software
   Engineering evidence than a working one without.
2. **Switch the inference path.** Azure Speech + Azure OpenAI Australia East, as
   in your VNet plan, or Vertex AI `australia-southeast1` if you would rather
   stay close to the current Gemini code. Do this before any student other than
   you speaks into it.
3. **Build the approval pack.** Six documents:
   * a Privacy Impact Assessment;
   * an AIAF-style risk self-assessment — generative AI, elevated inherent risk,
     practice-only, human-in-the-loop, landing at medium residual;
   * a data-flow diagram naming every processor and its region (you have the
     DFD already; add regions and a trust boundary);
   * a draft collection notice and parental consent form;
   * an Essential Eight-aligned security summary;
   * an incident-response plan with DoE notification steps, referencing the
     `safety_events` table as the detection mechanism.
4. **Hand it to the teacher and principal** and ask them to raise an Assessed IT
   listing or a Safe AI Ethics Assessment request with DoE ITD. The AIAF
   obligation is DoE's, not yours; a pre-filled assessment is the thing that
   makes a busy person say yes.
5. **Expect conditions**: a named teacher supervising, a single class, a term
   limit, transcripts reviewable, and an off switch.

Dates worth watching: Children's Online Privacy Code registration by
10 Dec 2026; DoE's responses to the Audit Office recommendations by July 2027;
first DCS-2026-02 attestations 31 Oct 2027.

---

## 11. Phasing

| Phase | Work | Blocks the next |
|---|---|---|
| 1 | `InterfaceSafety`, wordlist layer, chain, `safety_events`, both hooks, `SAFETY_MODE=local` | no |
| 2 | Cascade: STT as its own stage, `AudioInput::Gemini` gated off behind `AUTH_REQUIRED` | yes — phase 3 has nothing to screen without it |
| 3 | Azure Speech + Azure OpenAI in the VNet, Content Safety layer, managed identity | yes |
| 4 | DoE SSO, collection notice, consent record, retention and export | yes |
| 5 | Teacher escalation view, per-attempt export, incident runbook | no |
| 6 | Approval pack, then the school conversation | — |

Phase 1 is a day's work and is independently useful: it is the part that still
runs on the Pi with the network cable out, and it is the part you can
demonstrate in the HSC submission without any cloud account at all.

---

*Research and design, not legal advice. Before anything beyond a supervised
in-school pilot, the school should take this to DoE Legal Services.*
