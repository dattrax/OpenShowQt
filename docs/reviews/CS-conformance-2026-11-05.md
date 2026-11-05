# Shared Qt controls: conformance review, 2026-11-05

Reviewed committed work `3150e55` through `00800df` against the CS brief added
in `2e02ec8`, plus the current QML and component catalogue. Assessment only:
no application fixes, commit or push performed.

The component extraction broadly follows the intended architecture. PreviewView
remains the pixel painter, the shared inspector sections exist, mutation routing
has a central InspectorAdapter, and Show groups by running-order occurrence.
However, CS.2–CS.9's completion claims overstate visual and interaction acceptance.
There are seven confirmed implementation/layout issues below, one additional
source-path lifecycle risk, and a material gap in the recorded visual sign-off.

Four files already had uncommitted TV.3 work when this review began:
`src/domain/documents.cpp`, its public `documents.h`, `src/domain/storage.cpp`
and `src/ui/DocumentController.cpp`. Those edits were preserved. Their additions
are partial view-style/provenance/migration work, not evidence that TV.3 is
complete. The existing binaries were used for regression checks; the review did
not rebuild or certify those uncommitted model changes.

## Findings, in priority order

Remediation is tracked in PLAN.md's CR ledger. **CR.1 is fixed**: card picture
height now divides by its aspect ratio; `control_set_ui_tests` checks picture
and caption containment at six thumbnail sizes. Clean-snapshot application
build, workspace and UI-load tests pass. Findings below retain the original
review evidence; the final populated acceptance is recorded separately below.

**CR.2 is fixed:** the verse list declares its missing QML id. The focused UI
fixture now loads real BibleImport data through a scripted provider, verifies
viewport widths and long-verse wrapping, and clicks a checkbox. Application
build and the warning-free focused check pass.

**CR.3 is fixed:** the minimum-size Row uses the rail width. The shared UI check
asserts usable input/reset bounds at both rail widths and performs actual
type/commit/reset gestures through theme and slide targets. Application build
and the warning-free focused check pass.

**CR.4 is fixed:** inspector values use the compiled theme sample, including
inherited Default styles/effects and resolved colour metadata. The adapter's
ownValue stays separate, and inherited style sources name Default when it
carries the record. Adapter regression plus shared UI/workspace checks pass.

**CR.5 is fixed:** the lookup form wraps and bounds its fields to the pane.
The populated UI fixture verifies containment and clicks Lookup/Retry at all
three centre widths, including 416 pixels. Application build/check pass.

**CR.6 is fixed:** fixed-cell mapping follows actual cellWidth, and fractional
scroll offsets retain a full look-ahead row. The UI test compares actual
delegates with cueRect and visible-band membership across eight widths/three
offsets. Application build, focused UI and grouped workspace checks pass.

**CR.7 is fixed:** complete visible windows are deduplicated before delivery
through the consumer's bounded multi-batch queue. The wire remains capped at
32; retained window/cache ceilings are 128 scenes/pictures and 8 MiB. Eighty
cold keys receive pictures in three packets without scrolling; settled windows
send no repeat requests, cancellation stops queued work, and the byte LRU still
evicts oldest entries. Controller, populated UI and grouped workspace checks pass.

**CR.8 is fixed:** Bible visibility explicitly controls ScriptureImport preview
ownership. A real worker proves delayed lookup and measurement completion keep
Edit's exact Selection pixels, returning reacquires Bible, and hidden cleanup
cannot clear another owner. Actual page visibility wiring and workspace checks
also pass. Full TV.5 import-plan validity remains separately open.

**CR.10 fixes the known header crowding noted below:** audience status takes
only the gap remaining after navigation and fixed actions. The actual header
has nonoverlapping navigation/status rectangles at all three required widths,
including unsaved Themes state. Application build and the focused UI check pass.

**CR.11 fixes an additional defect exposed by populated acceptance captures:**
the verse CheckBox had an empty text property and a custom label with no indicator
padding, so Qt centred the indicator over the words. Real control text plus
left/right indicator padding restores separation. The actual populated UI test
checks this geometry, wrapping and selection; application build/check pass.

**CR.12 fixes a shutdown defect exposed by the real capture tour:** member
destruction let QLocalSocket emit disconnected after RpcClient's timers and
WorkerController's font metadata had been destroyed. The explicit transport
destructor stops timers and removes socket callbacks before aborting the socket.
A connected-client destruction regression passes, as does the controller suite;
the populated capture process now exits cleanly. The memory trace also exposed
the independent class-token dangling view tracked as CR.13.

**CR.13 fixes that parser lifetime error:** class tokens now slice a view of
the owned attribute string. New long translation identifier success/mismatch
cases fail on the old code and pass on the fix. Parser memcheck reports zero
errors and no retained blocks; parser, Bible page/e2e and populated UI tests pass.

**CR.9 acceptance:** the reproducible production-QML tour now captures populated
Show, Bible, Edit and both Themes views using actual worker pixels. The new
[acceptance record](CS-remediation-acceptance-2026-11-05.md) supersedes the old
empty-state visual evidence for these repairs and records the final gates.
Remaining TV/TA, physical release gates and operator-reported issues remain
separate; this is not authorization to start FO.

### R1 — P1: shared preview cards have the aspect ratio inverted

Source: `src/ui/SlidePreviewCard.qml:51–68`. Applies to Show and Bible generated
cards. Themes currently use a surface directly for their view tiles.

The surface height is `width * aspectRatio`, while aspectRatio is 16/9. It must
divide by the ratio for a landscape picture. A source-QML geometry probe measured
a **198×147 card containing a 192×341.33 picture**; the expected picture height
is 108. Caption placement then starts below the card and its background height
clamps to zero. Neither the card nor its picture clips to the card bounds, so
the picture/placeholder can overlap later rows and their captions.

The existing `/tmp/openshow-cs9/base.png` already shows tall black tiles with
missing caption strips and stray text near lower rows. This contradicts CS.2's
aspect/chrome acceptance and CS.9's “overlap-free” inspection claim.

Repair acceptance: assert the fitted picture and caption rectangles remain
inside cards at every thumbnail size; inspect populated Show/Bible cards with
real worker pixels, including the final partially visible row. Merely checking
the outer card's position does not test this.

### R2 — P2: populated Bible verse delegates reference a missing QML id

Source: `src/ui/BibleImportPage.qml:289–318`.

The ListView has `objectName: "bibleVerseList"` but no `id: bibleVerseList`.
Its delegate binds `width: bibleVerseList.width`. objectName does not create a
QML identifier. Loading the actual source with nonempty fixture verses emitted
`ReferenceError: bibleVerseList is not defined` for each instantiated row.

Verse controls therefore lose their intended viewport width and cannot reliably
wrap long passages within the compact selection list. Empty-page tests do not
instantiate these delegates and do not expose the error.

Repair acceptance: load long and short fixture verses through the real Bible
page, assert viewport-relative delegate widths and wrapping, and treat QML
warnings during that populated journey as failures. This is CS.3 work.

### R3 — P2: the shared minimum-size field has negative width

Source: `src/ui/TextStyleControls.qml:354–376`.

The field uses `width: parent.width - parent.spacing - 60`, but its containing
Row has no width. Its natural size depends on the children, creating invalid
size arithmetic instead of taking the inspector's available width. A probe of
the real section at width 232 measured **Row width 0, field width −64 and the
Inherit button at x = −60**. This affects the shared advanced-style section in
both Themes and Edit.

Repair acceptance: assert a positive, usable input width and that the field and
reset button are within the rail at all required sizes. Scroll to this specific
middle control and type/reset a value through pointer and keyboard input.
Checking only the first and last rail controls misses it. This violates CS.5/CS.6.

### R4 — P2: inspector inheritance can disagree with the rendered theme

Source: `src/ui/InspectorAdapter.cpp:224–275` (theme read branch).

For absent typography/effect overrides, the adapter substitutes hard-coded
contract defaults and reports “the renderer default”. It does not use the
resolved theme sample, which includes the editable Default theme underneath the
selected theme. Some base fields likewise return the raw stored value instead
of the effective value described by the adapter contract.

Reproduction using the real controller/adapter: set Default's fontWeight to 800,
create a theme with no own weight and inspect it. The adapter reports **400**;
`themeSampleAppearance(themeId)` reports **800**. Similar fallback code exists
for effects. An inspector can therefore show a different style from its canvas
and misidentify the source after Inherit.

Repair acceptance: compare fieldState.value/source against the actual compiled
sample with nontrivial Default font/fit/effect overrides, then reset own values
and repeat. Retain ownValue/isSet separately. Do not introduce a second resolver
inside the control or change materialization semantics accidentally. This is a
CS.4 contract deviation, not something TV.3 is required to invent first.

### R5 — P2: Bible lookup controls clip at a required window size

Source: `src/ui/BibleImportPage.qml:126–263`.

The lookup form remains one nonwrapping Row with a minimum 220-pixel reference
field, 200-pixel translation picker, Lookup/Retry and spacing. At the required
1000×680 window with both docks open the centre is about 416 pixels wide.
The reference/picker alone need more room than that. Horizontal scrolling is
disabled, so Lookup and Retry fall outside the clipped viewport.

Confirmed in the existing `base.png.bible-1000.png` and a populated source-QML
probe at centre width 416. The form needs a responsive wrap/stack policy, or an
explicitly reachable alternative layout. This was specifically required by
CS.3/CS.9; it is not satisfied by rendering an empty page at that size.

### R6 — P2: Bible collection mapping counts different columns from GridView

Sources: `src/ui/SlidePreviewCollection.qml:47–49, 99–132` and
`src/ui/BibleImportPage.qml:349–358`.

The collection computes columns from cardWidth (default 220), but Bible overrides
cellWidth to 210 without changing cardWidth. Actual GridView placement uses
cellWidth; mapping, collection height and visible-band selection use columns.

At width 650 and height 460, a source-QML probe reports **2 columns while three
210-pixel cells fit**. cueRect(2) says x=0/y=159.375, but that cue belongs in the
third cell of the first physical row. visibleIndexes returns 0–7 even though
three physical rows expose cues 0–8. The last visible card can thus be omitted
from thumbnail requests. In other widths, collection height wastes extra rows.

Repair acceptance: derive mapping from the actual layout extent; test widths
around both 210- and 220-pixel multiples and compare mapped cue rectangles/bands
against delegate rectangles. This violates the shared collection's CS.3/CS.8
geometry contract; Show's stretchCells mode does not prove Bible's mode correct.

### R7 — P2: the 32-thumbnail limit permanently starves visible cards

Source: `src/ui/ShowPage.qml:163–179`; Bible's request loop has the same first-32
policy in `BibleImportPage.qml:50–66`.

Show truncates wanted indices to the first 32, or last 32 at the bottom, before
deduplicating by slide id. There is no continuation or rotation for remaining
visible demand. Repeating the request chooses the same subset. Small thumbnails
and a large viewport can expose more than 32 distinct slides; a collection probe
at 1000×1000 with the supported 120-pixel step returned 88 visible/look-ahead
indices. The supported large operator layout can also exceed the batch cap.

Already-cached cards may hide this during testing. New scenes outside the chosen
subset stay blank until scrolling changes the selected band. Deduplicating after
truncation can additionally waste capacity on repeated show entries.

CS.8 explicitly required all visible pictures to be served eventually without
an unbounded request loop. Repair acceptance: use a cold cache and more than 32
distinct visible cues, wait without scrolling and prove every visible key gets
pixels with bounded batches. Include repeated occurrences, bottom anchoring and
look-ahead priority. Tests that mirror slice(0,32) only establish the cap.

### R8 — P2 source-path risk: late Bible work can reclaim another page's preview

Sources: `src/ui/BibleImportPage.qml:87–97` and
`src/ui/ScriptureImport.cpp:41–46, 326–389`.

Hiding Bible stops its QML timer and clears the shared preview lanes, but does
not suspend/retire the session's lookup/measurement callbacks. publish() always
calls previewSlide(0), which requests Selection without a page-owner check.
A lookup/pagination completion after switching to Edit/Themes can therefore
issue a Bible selection request after the new page has acquired the lane.

This is established by the call path, not a newly timed live-race reproduction
in this review. The hidden-Show bandSent check does not cover this owner. CS.3
promised hidden-page request isolation; the wider generation/return-to-page work
is also explicitly pending in TV.5. Add a delayed-completion page-switch test
before claiming isolation across all three off-air hosts. Audience presents are
not implicated by this particular path.

### R9 — P2: recorded visual acceptance does not exercise the populated feature

Sources: `tests/workspace_ui_tests.cpp:3380–3395, 4155–4365`,
`PROGRESS.md:42–48` and the CS.9 completion row in PLAN.md.

The Bible capture matrix switches to the page and grabs screenshots without
loading verses or generating slides. The saved Bible captures contain an empty
plan and a malformed reference left from an earlier test. Captures are also
taken after the live worker leg, with disconnected output. These pictures cannot
prove rendered scripture fidelity, nonoverlapping generated cards, delegate
wrapping, selection or correct display of current-plan pixels.

The source review and existing Show capture directly contradict the statement
that visual inspection found no overlap. Passing tests and producing all named
PNG files are not visual acceptance. The tests need assertions about child
geometry and populated interactions, alongside human inspection of actual worker
frames. Zero warnings on a journey that never creates verse delegates says
nothing about R2.

Retain historical DONE evidence, but add a focused remediation/acceptance pass
before treating CS.9 as release-quality closure. Its original brief also said
to finish remaining TV/TA acceptance before closure; the implemented ledger now
describes those as later gates. That is a narrowed checkpoint definition, not
proof that the original integrated scope was delivered.

## What follows the plan and what remains intentionally open

- The named parts and catalogue exist; application and workspace test resources
  include the shared parts. QML composition and the existing worker renderer
  remain the implementation approach.
- Theme/slide/service writes are routed separately; Edit exposes the advanced
  style controls and keeps image/background and authored-item tools in its host.
- The grouped Show model preserves flat cue order, entry occurrence identity,
  inert filler cells and acknowledged live/pending markers. The row-break design
  itself follows the brief; card geometry and thumbnail scheduling need repair.
- TV.3, TV.4a, TV.4b, TV.5 and TA.1 are still marked TODO, and FO remains gated.
  Missing independent Bible-view effects, caption roles and full stale-plan/Add
  validity are known unfinished scope, not surprise regressions in completed TV
  work. The uncommitted TV.3 changes do not change that assessment.
- The known TopBar status/More crowding at 1000 pixels remains visible in the
  saved Themes capture. It predates this review and is already acknowledged.

## Verification and limits

Existing-build focused checks:

```sh
ctest --test-dir build -R '^(inspector_adapter_tests|workspace_ui_tests|preview_selection_tests|bible_import_page_tests)$' --output-on-failure -j1
```

`workspace_ui_tests` and `inspector_adapter_tests` passed. The worker tests first
failed because the sandbox refused Unix-domain listen operations. Re-running
`bible_import_page_tests` and `preview_selection_tests` with local socket access
passed both. Consequently all four selected checks passed with their required
environment; this does not establish that the cases above are covered.

Additional scratch probes compiled outside the repository loaded copies of the
actual QML with the real PreviewView/InspectorAdapter and existing UI libraries.
The Bible probe supplied deterministic, nonempty QML fixture data rather than
performing a provider lookup. They measured R1/R3/R4/R6, elicited R2's runtime
ReferenceError, and illustrated visible demand for R7. Probe sources/output and
a populated page capture are in `/tmp/openshow-review-2026-11-05/`; they are local
review artifacts, not permanent test coverage. Existing acceptance pictures
were inspected in `/tmp/openshow-cs9/`.

No full build/full-suite rerun, provider-network acceptance, physical projector
test or certification of the uncommitted TV.3 patch is claimed. Assessment stops
before fixes so the operator's additional visual bug list can be combined with
these findings into a single remediation plan.
