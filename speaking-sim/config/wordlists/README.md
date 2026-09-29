# Wordlists

Deployment data, not source. The live `*.txt` files are in `.gitignore`; this
file and the `*.txt.example` templates are tracked.

**The `.example` files are not inert templates — they are the fallback.** When
`<language>/profanity.txt` is missing, the loader reads
`<language>/profanity.txt.example` in its place, and says so once at startup.
That is deliberate: a fresh clone with no wordlist directory would otherwise
start up completely unscreened while `ready()` still reported true, which is
the one failure mode C7 is about. A school that needs its own terms copies the
`.example` to `.txt` and edits that; the copy wins and is never committed.

One directory per language id, matching `LanguagePack::id`:

    italian/profanity.txt   one term per line, matched per whitespace token
    italian/jailbreak.txt   one phrase per line, matched as a substring
    italian/escalate.txt    one phrase per line, substring, raises a teacher flag

Lines beginning with `#` and blank lines are ignored. Entries are run through
`normalise_for_match()` at load, so accents, capitals, repeated letters and the
usual character substitutions do not need their own entries: write `perché`
once, not `perche`, `perchee` and `p3rche`.

Sources to seed from:

* profanity — the LDNOOBW `List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words`
  `it` and `en` files, then cut the terms that are ordinary Italian in an exam
  about family and holidays. Over-blocking a beginner's vocabulary is itself a
  pedagogical failure.
* jailbreak — phrases only, not single words: `ignora le istruzioni`,
  `ignore the above`, `sei ora`, `pretend you are`, `system prompt`. The remote
  Prompt Shield does the real work; this list is the offline build's stand-in.
* escalate — disclosure phrasing, kept short and high-precision. Every entry
  here routes a student to a teacher, so a false positive costs a real
  conversation and the bar for adding one is high. Draft it with the school's
  wellbeing team, not alone.

Review the lists each term and record the review date in the PIA.

## Writing entries that do not misfire

`normalise_for_match()` collapses repeated letters, so check every new entry
for what it collapses *to* before adding it. Three that were cut from the seed
lists for exactly this reason:

* `ass` becomes `as`, which would mask every "as" in an English answer.
* `coon` becomes `con`, which is Italian for "with".
* `finocchio` is also fennel.

The same care applies to substring entries in `jailbreak.txt` and
`escalate.txt`, where the match is not bounded by token edges at all: anything
built on `vivere` or `leben` will fire on an answer to "where do you live",
which is a set topic in every one of these exams.
