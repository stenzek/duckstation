# Translation quality reference

Read this before translating or semantically reviewing a DuckStation catalog. Structural validation proves neither
accuracy nor natural language quality. A populated translation and a `finished` flag are not evidence of review.

## Locale guidance and context

Before a large pass, establish the target language and regional variant from `<TS language>`. Prepare a small JSON
guide from verified catalog examples, UI/source behavior, and any corrections confirmed by fluent speakers. Use
`translate_ts.py start --guidance GUIDE.json` or `review_ts.py start --guidance GUIDE.json` to snapshot it. For a small
update, the coordinator can provide the same context directly. Guidance is optional in the CLI, not proof of quality.

A guide can contain `language`, `style`, `terms`, `do_not_translate`, and `context_notes`. For example, an English guide
could include:

```json
{
  "language": "en_US",
  "style": {"register": "concise emulator UI; preserve warning strength"},
  "terms": [
    {"source": "Save State", "target": "Save State", "meaning": "emulator snapshot, distinct from memory-card save data", "evidence": "Save State menu and its call sites"}
  ],
  "do_not_translate": ["DuckStation", "Vulkan", "PGXP"]
}
```

The example illustrates the schema; establish actual target wording for the requested locale. Do not create guides
for every language speculatively. Existing translations, including finished ones, are evidence to assess rather than
an unquestionable terminology authority. Translation-memory suggestions are unverified and may be historical.

For each ambiguous message, inspect related labels, option descriptions, neighboring actions, comments, and the
calling C++ or `.ui` file. Use the task's repository and catalog paths to search for the exact English source.
Locations can be supplied in task context without adding `<location>` nodes to tracked catalogs. Determine whether
the string is a button, state, action, title, controller label, unit, or tooltip. Inspect call arguments to identify
what each placeholder represents. Never infer behavior from an unfamiliar technical word alone.

Keep related labels and tooltips together when assigning tasks. Reduce task sizes for complex prose or unfamiliar
subsystems. Bounded task size limits workload; it does not guarantee careful translation.

## Accuracy and terminology

Check source and target together for negation, conditional scope, comparisons, immediacy, warnings, data loss,
subject/object relationships, and added or omitted claims. Preserve qualifiers such as "may", "only", and "safer".

Distinguish save states from memory-card data, frame rate from frame pacing, runahead from rewind, fast boot from
fast-forward, rendering from presentation, and checked states from check actions. Use established names consistently
for controller buttons, backends, achievements, hardcore mode, Big Picture Mode, and other named features.

Reconcile terminology across worker tasks and contexts before application. Check identical source strings for
inconsistent targets, and check label/tooltip pairs for different names for the same option. Context may justify
different senses: a controller's `Start` and an action named `Start` are different uses. A glossary correction must
trigger review of other occurrences, including decisions already accepted in this run.

Keep proper names and intentional technical tokens where appropriate. Unchanged prose needs a concrete explanation,
such as an English plural catalog or a documented UI token. A warning emitted while normalizing copied source text
does not constitute semantic approval. If context is missing, record the question rather than inventing a meaning.

## Fluency and UI fit

Also read the translated text without the English alongside it. Check natural grammar and word order, idiomatic
phrasing, clear noun relationships, consistent register, concise labels, and references a user can recognize.
Identify literal translations that preserve dictionary meanings but sound unnatural or obscure the action.

Distinguish demonstrable fluency defects from preference between two natural alternatives. Report a fluency issue
when you can explain the grammatical error, unnatural construction, ambiguity, or usability problem and supply a
clearly better complete replacement. Do not rewrite acceptable variations merely to standardize personal style.

Where a UI preview is available, check truncation, dialog/button fit, meaningful line breaks, and accelerator
collisions within the same menu or dialog. Use Qt `lrelease` as an additional compilation check when available;
compilation cannot establish semantic quality. Do not claim visual verification if the UI was not inspected.

## Formatting and plural integrity

- Preserve Qt, fmt, printf, and template placeholders, their argument roles, and formatting specifications.
  When grammar reorders anonymous fmt arguments, use explicit indices, for example `{} {:08X}` → `{1:08X} {0}`.
  Renumbering must preserve which value appears in which position and its format.
- Fill every required plural form. Retain the locale's catalog-defined arity; investigate suspicious arity using Qt
  rather than assuming every locale has two forms. English plural forms can legitimately match the English source.
- A plural form may omit a redundant argument only when the meaning permits it; at least one form must retain the
  complete argument counts. Do not add arguments or repeat an argument beyond the source count.
- Preserve required rich-text tags, correct nesting, links/resource attributes, file-dialog filter syntax,
  accelerators, and semantic line breaks. Added layout tags and wrapping need contextual judgment.
- Preserve literal angle-bracket labels such as `<Parent Directory>` as text. Never edit `<source>` to silence a
  warning or replace the whole XML tree to change selected translations.

## Review and feedback

Review every changed message before applying a translation session. Assign review to a different worker from the
translator when collaboration is available; provide source, candidate, relevant context, and guidance without the
translator's rationale so the review can form its own judgment. Run a separate reading for fluency and reconcile
terminology across tasks. In a serial fallback, do a distinct review pass and disclose the absence of an independent
reviewer. A reviewer name in a journal records provenance; the CLI cannot verify reviewer independence or competence.

Classify concrete findings by `accuracy`, `fluency`, `terminology`, or `format`. High severity covers reversed behavior,
materially weakened data-loss warnings, and harmful instructions. Medium covers substantive mistranslations,
misleading labels, missing conditions, and fluency defects that obscure meaning. Low covers concrete typos,
unnatural wording with limited impact, and minor recognizable inconsistencies.

Turn confirmed user complaints into a small evaluation set for affected locales: record context, source, faulty
target, explanation, acceptable corrections, and who confirmed them. Do not invent confirmations or treat one exact
wording as the only acceptable translation. Compare meaning, fluency, terminology, and UI fit when testing a changed
workflow. Pilot representative labels, technical tooltips, warnings, and plurals before scaling a large pass; revisit
the pilot if it exposes systematic mistakes. Structural unit tests complement this evaluation and cannot replace it.
