# speaking-sim — retention windows, export and erasure

What the system keeps, for how long, and why that long. Requirement ids
(A1–A9, B1–B7, C1–C8) refer to the non-negotiables checklist in *Getting
speaking-sim Into a NSW Public School*, 28 Sep 2026.

This file is the one to hand to a school, a principal or a DoE assessor. It is
also the file `src/config.cpp` is checked against: the defaults below are the
defaults in the code, and the ordering rules in section 3 are enforced at
startup rather than described here and hoped for.

---

## 1. The windows

| What | Where it lives | Default | Setting |
|---|---|---|---|
| Raw audio | nowhere | **not stored at all** | — |
| Transcripts — the student's words and the examiner's | `attempt_turns` | **90 days** | `TRANSCRIPT_RETENTION_DAYS` |
| Exam records — when, how long, which plan, which tenses, which set questions | `exam_attempts` and everything cascading from it | **455 days** | `ATTEMPT_RETENTION_DAYS` |
| Safety events — that something was flagged, what category, what was done | `safety_events` | **365 days** | `SAFETY_EVENT_RETENTION_DAYS` |
| Expired sign-in sessions | `auth_sessions` | **deleted once past their own expiry** | — |
| Daily usage counters | `usage_daily`, `key_usage_daily` | follows `ATTEMPT_RETENTION_DAYS` | — |
| Inactive accounts | `users` and everything cascading from it | **off** | `INACTIVE_ACCOUNT_RETENTION_DAYS` |

A window of `0` means keep forever. For the first three that is **refused at
startup** once `AUTH_REQUIRED=1`: A3 requires records be kept no longer than
necessary, and forever is not a period.

---

## 2. Why each window is that long

**Raw audio — never stored.** The recording exists in memory for the length of
one turn and is discarded once the transcript is produced. There is no audio
file, no object store and no backup of one. This is the strongest single
sentence in the whole compliance story and it was true before any of this was
written; it is listed here so that nobody has to go and verify it in the code.

**Transcripts — 90 days.** One school term plus a margin for marking and
reporting. This is the most sensitive text the system holds: a minor's
unedited speech, including the disfluency and self-correction a language
examiner is specifically listening for. It therefore gets the shortest window
of the four.

Deleting a transcript does **not** delete the exam. The attempt summary, the
tense features (`turn_features`), the set-question verdicts and the evidence
rows all survive, and none of them carries an utterance. A teacher's progress
report is built from those, so an exam whose words are gone still reports
normally. That split is the point of the window: the pedagogical value is kept
and the personal information is not.

**Exam records — 455 days.** Thirteen months, so a full year of progress
reporting survives plus a month of overlap into the next year. It is the
parent row, so it is necessarily the longest of the three exam-related
windows.

**Safety events — 365 days.** Deliberately **longer than the transcripts they
refer to**. The record that something was flagged, and what was done about it,
should outlive the practice data it arose from — a wellbeing team asking "has
this happened before?" is asking about the event, not the sentence. This is
safe to keep longer precisely because the table holds no utterance: by design
`safety_events` stores the normalised terms that matched and never the words
they sat in, and there is a comment in the schema forbidding a `text` column.

**Expired sessions — immediately.** No window of its own. A session past its
own `expires_at` is already useless, and a dead token is one more thing a
copied database file should not contain.

**Inactive accounts — off by default.** Not refused under `AUTH_REQUIRED`,
unlike the three above. An account is deleted when a school asks for it. An
automatic sweep of children's accounts is a decision somebody signs, not a
default that arrives with a build.

---

## 3. The ordering rule, and why it is enforced

`safety_events` and `attempt_turns` both hang off `exam_attempts` with
`ON DELETE CASCADE`. Deleting an attempt therefore deletes its turns and its
safety events with it, whatever their own windows say.

So two rules hold, and `load_config()` **refuses to start** if either is
broken:

- `TRANSCRIPT_RETENTION_DAYS` ≤ `ATTEMPT_RETENTION_DAYS`
- `SAFETY_EVENT_RETENTION_DAYS` ≤ `ATTEMPT_RETENTION_DAYS`

A refusal rather than a warning, because the failure is otherwise silent: the
rows would simply not be there, and the first person to notice would be
whoever went looking for an incident.

A third case is a warning rather than a refusal — a safety window shorter than
the transcript window. That is not impossible to honour, it is just backwards:
the record that something was flagged would be gone while the words that
triggered it were still stored.

---

## 4. When the purge runs

- Once at startup, on its own thread, so a database carrying a year of old
  transcripts does not hold the listening port closed while it deletes them.
- Once every 24 hours after that, until shutdown.
- On demand: `speaking-sim --purge-now`, which prints what it removed.

A pass that finds nothing says nothing. A pass that removes something logs one
line naming each count. A pass that fails logs and retries the next day — a
purge that cannot run is a compliance problem to fix, not a reason to take an
exam server down in the middle of a lesson.

Every statement is a bounded `DELETE` taken under the same lock as every other
write, so a purge is safe beside a running server and beside the `sqlite3` CLI.

---

## 5. Export (A7)

Assessment-related records are State records. A7 requires they be
**retrievable by DoE**, not merely visible in a dashboard.

```
speaking-sim --export-attempts user <email>
speaking-sim --export-attempts class <class id>
```

Both print JSON to stdout: the attempts, their turns, tense features, set
questions and safety events. The class export also names the student against
each attempt.

Two things the export deliberately omits:

- **`safety_events.matches`.** The normalised terms are an operator fact, and
  an export is the last place to start copying them around.
- **Nothing is reconstructed.** An attempt whose transcripts are past their
  window exports with its record intact and no turns. That is the honest shape
  of it: the exam happened, the words are gone, and the export says both.

The same command answers a student or parent asking what is held about them,
which is the access half of the PPIP access-and-amendment principle.

---

## 6. Erasure

```
speaking-sim --delete-user <email>
```

Removes the account and everything that cascades from it: identities,
sessions, class memberships, usage counters, profile change requests, and
every attempt with its turns, features, set questions and safety events. It
also clears the columns that name this person as having acted on somebody
else's row, and removes any licence targeting them.

There is no confirmation prompt. This is run by an operator who was asked to
erase a record, and a prompt in a command that may be scripted is a prompt
somebody pipes `yes` into. **Export first if the records are wanted** — the
cascade does not keep a copy.

An account that still *owns* things other people depend on — classes, class
invites, exam plans — is **refused** rather than erased, in one transaction
that changes nothing on the way out. A roster with no owner is worse than a
refusal. Reassign or archive those first, then delete.

---

## 7. What this does not cover

Named so that nobody mistakes this file for the whole of A3 and A7:

- **Encryption at rest and file permissions** for the SQLite database are a
  deployment matter, not a setting in this repository (B5, B6).
- **The State Records authority number** for student assessment recordings was
  not verified. Ask the school's records officer before fixing 455 days as the
  operative figure; the setting exists so that answer can be applied without a
  rebuild.
- **Backups.** A retention window is only true if the backups honour it too. A
  deployment that snapshots the database file needs its own expiry on those
  snapshots, or the windows above are a statement about one copy of the data
  rather than about the data.
- **Databases created before `picture_url` was dropped** still carry the column
  and whatever is in it. The schema is only ever created from scratch and the
  queries name their columns explicitly, so an old file keeps working — it just
  keeps the photos too. Delete the `.db` and let it be recreated, or run
  `UPDATE users SET picture_url = ''`, before treating that column as gone.
