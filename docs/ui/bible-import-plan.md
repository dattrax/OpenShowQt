# Bible import — passage-only lookup into today's schedule (milestone BI)

Planned 2026-11-02. The parent statement lives in
[PLAN.md § Bible import](../../PLAN.md); this document is the detailed
model, the provider-access record and the acceptance ledger for the leaves.

## Provider access — the approach, recorded before implementation (BI.1)

**Decision: fetch the operator's ONE requested passage page over HTTPS from
Bible Gateway (`https://www.biblegateway.com/passage/?search=<ref>&version=<v>`)
and parse it. Nothing else.**

Verified facts behind the decision, 2026-11-02:

- **robots.txt** (retrieved 2026-11-02): `/passage/` is NOT disallowed for
  any user-agent; the disallows cover `/user`, `/feedback`, study-bible
  chapter paths and a crawl-delay of 15 seconds. A single interactive lookup
  per operator action sits inside that allowance; the importer keeps a
  minimum spacing between fetches so a clicking operator cannot turn into a
  crawler.
- **No open API**: Bible Gateway offers no key-free JSON API. Keyed APIs
  (Boulder Key / scriptureapi.com) need third-party registration and per
  translation keys — out of scope for an app that must work with the
  operator's browser-level access and no accounts.
- **Terms**: the site's terms forbid bulk download and redistribution. The
  importer's design is the compliant shape: one page per explicit operator
  action, no corpus download, no prefetch, no cache to disk, no history, and
  the translation's own copyright/attribution block kept and carried with
  the imported text. The plan's constraints (never a full Bible, never a
  local scripture library) are not just product rules; they are what makes
  this access pattern defensible.
- **Markup, recorded from live pages the same day** (fixture structure in
  `tests/bible_import_tests.cpp`; no fetched passage bytes are stored in
  the repository):
  - the passage lives in `div.passage-text` → `.passage-content` →
    `div.version-<ID>.result-text-style-normal.text-html`;
  - each verse is a `span class="text <OSIS-book>-<chapter>-<verse>"`
    (e.g. `1Cor-1-2` — numbered books start with a digit); verse 1 of a
    chapter carries a `span.chapternum` and later verses a
    `sup.versenum` — page furniture, not scripture;
  - inline `sup.crossreference` (with escaped anchor HTML in `data-link`)
    and `sup` footnote markers carry letters, not text;
  - section headings are `h3 > span.text <same book-chapter-verse token>` —
    the same class token as the verse they head, so headings must be skipped
    by subtree, not by class;
  - the licence block is `div.publisher-info-bottom`: the version name in
    `strong` and the copyright paragraph(s) as siblings;
  - an invalid reference answers with NO `passage-text` container and the
    sentence `No results found.`.

**Failure rules** (implemented in `openshow::importer` and pinned by
`bible_import_tests`): a page that is a block/captcha page ("access
restricted"/"unusual traffic"), an invalid reference, a passage container
whose verse markup the parser no longer recognises, or a page answering a
DIFFERENT translation than the one requested, all refuse with their own
sentence and import nothing. There is no fallback that scans the rest of
the page: the importer would rather say "the markup changed" than put
someone's navigation bar on the audience output.

## Model

- `Passage` (translation, attribution, ordered `PassageVerse`s with
  `reference` "chapter:verse" and clean plain text) — `src/importer/`,
  Qt-free, parse-only. The fetcher (BI.2) lives in the frontend, the
  schedule-owned item (BI.3) in the domain, pagination (BI.4) measured by
  the worker, the surfaces (BI.5) in the UI.
- "Today's schedule" is the playlist surface the workspace already has
  (the seeded `Service` playlist is today's schedule); the imported passage
  becomes a **schedule-owned presentation** — a show that belongs to that
  playlist and is NOT part of the reusable Library, with no path from it
  back into the Library.
- **Delivered shape (BI.3, 2026-11-02):** `Show.scheduleOwned` plus
  `Show.scripture = ScriptureProvenance{reference, translation, attribution}`,
  stored as optional format-6 show keys (`"scheduleOwned": true`, a
  `"scripture"` object) written only when they speak — an ordinary file's
  entries gain no keys, a pre-6 file carrying either key or provenance on a
  show that is not schedule-owned is refused naming the key. The controller's
  `addScripturePassage(playlistId, reference, translation, attribution,
  slides)` performs the plan's one destination action as ONE history unit
  against an explicit target (a vanished schedule is refused, not
  redirected); the Library list, the Library count and the drawer search
  all exclude schedule-owned shows, while the schedule's own rows and the
  presenter's cue see them normally.

## Leaves

| Leaf | Scope | Acceptance |
|---|---|---|
| BI.1 | access record above; `openshow::importer` parser + translation menu; synthetic-markup regressions | `bible_import_tests`: live-shape parse, verse boundaries and entities/Unicode, headings/crossrefs/nav excluded, invalid-reference, blocked and changed-markup refusals import nothing, translation mismatch refused naming both |
| BI.2 | async bounded HTTPS lookup in the frontend + transient importer state | cancel/timeout/offline/retry and superseded replies; stale replies cannot replace the current passage; nothing fetched is written outside the schedule (no cache, no temp files) |
| BI.3 | schedule-owned scripture presentation + undo/save/recovery | add lands once on today's schedule, undo removes it whole, save/reload keeps the selected text and metadata, the show never appears in the Library, existing documents still load |
| BI.4 | theme-aware measured pagination via the worker's text layout | verse packing/continuation through the resolved theme's box and typography measured with the real engine; no clipping, no repeats, no silent shrink |
| BI.5 | importer UI: translation, reference, verse selection, theme preview, Add to today's schedule only | online notice, actionable failures, cancel writes nothing, preview off-air, no Library/live actions |
| BI.6 | end-to-end acceptance and operator documentation | connected import, disconnected failure, offline playback of saved slides; fixtures stay synthetic; nothing but the schedule keeps text |

Task order: BI.1 → BI.2 → BI.3 → BI.4 → BI.5 → BI.6, one commit per leaf.
