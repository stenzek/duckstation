# DuckStation Qt Translation Workflow

## Catalog invariants

- A message is active unless its translation is `vanished` or `obsolete`.
- Both blank and populated `unfinished` translations require review. Populated values can be stale or semantically wrong.
- Numerus messages must retain their catalog-defined number of `<numerusform>` elements.
- Sources may contain newlines, rich text, accelerators, Qt/fmt/printf placeholders, and template variables.
- Parse XML for identity and correctness, but replace only selected `<translation>` nodes in the original text. Full XML
  reserialization creates large unrelated diffs.
- Stable IDs include context, source, comments, numerus state, optional message ID, and occurrence. The catalog fingerprint covers source
  identities rather than translations.

## Why the old batch workflow failed

- Four batches for a 2,000–3,000-message catalog still produced hundreds of records and hundreds of kilobytes per agent.
  A timed-out worker often returned no usable result.
- Context-count balancing did not bound serialized prompt size; a few large HTML messages could dominate a batch.
- Workers modified whole batch files and the coordinator could discover stale files through directory wildcards.
- In-place batch updates were not atomic, and applying an already-applied batch failed instead of acting as a no-op.
- Applying batches incrementally exposed a partially translated catalog when a later batch failed.

## Resumable session model

`translate_ts.py start` creates an ignored `.translation-work/` session containing:

- `manifest.json`: catalog fingerprint, selection settings, target count, task ownership, and task checksums.
- `tasks/*.jsonl`: immutable work packets bounded to 300 records or 192 KiB by default, packing small contexts together.
- `responses/*.jsonl`: one pre-created response per task. Only that task's worker may append decisions.
- `reviews.jsonl`: the coordinator-owned canonical journal, rewritten atomically after validated merges.

Workers checkpoint valid JSONL after at most 100 decisions. An interrupted append can leave a truncated final line; salvage retains individually valid earlier decisions. Merge the checkpoint, then let another worker continue the missing IDs from the same task.
The TS catalog is not a progress store. Stop the previous writer before recovery or ownership transfer.

The session CLI does not translate. Do not inspect it for an automatic translator or search for multi-agent
infrastructure. When Codex exposes `spawn_agent`, `wait_agent`, and `followup_task`, use them immediately for a session
with multiple tasks. If collaboration tools genuinely are absent, process the same bounded tasks serially in the
coordinator instead of stopping. The coordinator queues only enough tasks to fill available agent slots and reuses
workers as they finish. Never allow workers to edit the manifest, task files, review journal, catalog, or another
response.

Concurrency is not capacity. Three available child-agent slots can process an arbitrary number of tasks by cycling
through successive waves. For sessions with more than one task, use subagents automatically, merge each result as soon
as it completes, and immediately follow up with the next task. If every slot is busy, wait for a completion. Never treat
the number of remaining tasks, context compaction, or a worker timeout as a reason to return a partial session to the
user.

A requested full update is complete only after `remaining=0`, exact semantic approval, atomic apply, final validation, and diff inspection. The
session's resumability exists for internal fault recovery; it is not an acceptable final outcome unless a concrete
external conflict or missing user input makes further progress impossible.

Worker-side validation is optional. Give workers the repository root and absolute task/response paths, but keep
authoritative checking in the coordinator. A child that cannot invoke the checker is not a blocker as long as it
preserved response decisions. Do not spend a worker turn running `check` after routine checkpoints; it is only useful
at task completion or while diagnosing a response problem. A partial checkpoint is successful progress, not a failed
task.

Strict response loading intentionally rejects malformed metadata, truncated JSON, invalid decisions, and conflicting
duplicates. Recover rather than discard otherwise valid work:

```sh
python3 scripts/translation/translate_ts.py salvage SESSION RESPONSE
python3 scripts/translation/translate_ts.py salvage SESSION RESPONSE --write
python3 scripts/translation/translate_ts.py check SESSION RESPONSE
python3 scripts/translation/translate_ts.py merge SESSION RESPONSE --write
```

The dry run reports what would survive. The write atomically regenerates canonical metadata, retains valid owned
decisions in task order, and skips invalid lines. `status` identifies damaged response paths and recommends salvage
before further dispatch.

An exact singular `translation` equal to its source is an equivalent but less explicit encoding of `accept_source`.
Validation canonicalizes it to `accept_source: true` and emits a warning rather than rejecting the entire worker
checkpoint. Workers should still emit `accept_source` directly for reviewed technical identifiers; the normalization
exists to avoid spending another agent turn on a schema-only correction. Source-equal plural targets are rejected outside English locales; valid English forms may match the source. An exact source copy still requires the semantic reviewer's explicit explanation.

## Merge and conflict rules

- A response is bound to its session, task ID, and immutable task checksum.
- Each decision sets exactly one reviewed target. Empty targets, wrong singular/plural shapes, wrong plural arity,
  placeholder loss, missing required rich text, unbalanced markup, accelerator loss, and unjustified non-English plural source copies fail
  before entering the journal. Exact singular source copies are canonicalized to `accept_source` with a warning.
- Partial responses are valid and mergeable. Re-merging the same decision is a no-op.
- Different decisions for an already-reviewed ID are rejected unless the coordinator deliberately uses `--replace`.
- `status` distinguishes merged decisions, valid unmerged checkpoints, malformed responses, remaining work, and catalog
  conflicts.

## Atomic catalog update

Apply only after the journal covers every target:

1. Verify the session and every immutable task checksum.
2. Verify that source identities and locale/version metadata still match the session.
3. Require each targeted translation to match its start-of-session state or its already-applied reviewed value.
4. Construct every replacement in memory and parse the staged catalog as XML.
5. Verify every staged target is finished and exactly matches its reviewed value.
6. Require semantic approval for the exact staged targets in sessions started with `--require-semantic-review` (required by the translation skill).
7. Recheck original catalog bytes immediately before atomic replacement, preserving mode and bytes outside selected translation elements.

If any check fails, the tracked catalog remains unchanged. Repeating a successful apply is a successful no-op.
Unrelated translation edits are preserved; source changes or edits to targeted messages require a fresh session or
manual reconciliation.

If the user explicitly abandons an incomplete session and asks to preserve its completed work, salvage and merge every
response, prepare and complete semantic review with `prepare-review SESSION --reviewed-only`, then dry-run and write `apply SESSION --reviewed-only`. It validates and atomically applies only journaled
decisions while leaving unreviewed entries unfinished. Validate the catalog and verify that only translation nodes
changed before deleting the exact abandoned session. Do not use this as an automatic substitute for completion.

## Translation quality

- Read `QUALITY.md`. Translate as emulator UI using verified locale guidance, source context, and existing catalog evidence; finished translations can still be wrong.
- Preserve shortcuts and accelerators; for example, localize `&Open` without dropping the accelerator.
- Preserve all placeholders exactly. Plural forms may omit redundant placeholders only under the existing plural rules,
  and at least one form must retain the complete set.
- Required source rich-text tags must remain. Additional layout tags and newline-count changes are warnings for review.
- `accept_current` records a reviewed draft; a separate semantic pass must approve the exact candidate before apply.
- `needs_context` records a concrete question without counting it as finished or applying it.
- Every plural form must be populated; an entry marked finished may still have empty individual forms.
- `accept_source` is reserved for singular proper names or technical identifiers such as Vulkan, GLSL, PGXP, and BIOS.
- Exact singular source copies are normalized to `accept_source` and logged for audit.
- Do not use external translation services or generate unreviewed responses with scripts.

## Final checks

- `translate_ts.py status SESSION` reports no remaining, invalid, stale, or conflicted work.
- `translate_ts.py apply SESSION` succeeds as a dry run before `--write`.
- `validate_ts.py CATALOG --require-complete --strict-required-tags --strict-placeholders --strict-accelerators` succeeds for a full-locale completion.
- The diff changes only intended translation elements and preserves vanished/obsolete messages.
- Treat `git diff --check` source whitespace carefully. Source strings must continue to match the application.


## Durable semantic review

`translate_ts.py prepare-review SESSION` stages candidate XML and prints an audit path. `review_ts.py` creates bounded
immutable tasks, worker responses, a snapshot, and a canonical review journal keyed by message ID. Reviewers check
accuracy, fluency, terminology, and formatting; copied source text requires a reason. The coordinator reconciles
terminology across tasks and merges completed responses serially. Reviewer identity is provenance, not proof that a
human or independent worker performed the review.

Changing a candidate invalidates its previous approval. Regenerate prepare-review and retain journaled results only
where exact target text, locale, and guidance are unchanged. General catalog audits use the same tool and can refresh
with `review_ts.py start CATALOG --previous AUDIT`. Public findings files remain optional; internal progress journals
are required for a large audit. `QUALITY.md` describes context gathering, locale guidance, and evaluation against
confirmed user complaints. Neither a passing validator nor full coverage establishes native-language quality alone.

CI checks every catalog for tooling changes or manual runs. It compares strict diagnostics to the Git baseline and
reports unchanged legacy defects separately; changed translations must pass the checks. A failed Git selection is an
error, never an empty successful result. Churn application uses the same strict checks on new/changed translations
and preserves unchanged legacy entries without treating cleanup as permission to proofread the whole catalog.

Churn reduction marks newly inserted active translations unfinished while preserving their text and plural forms,
so human translators can find review work. It does not change the status of retained entries. The main translation
session still marks its reviewed, completed targets finished.
