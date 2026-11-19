# Whole-item theme application and optional reflow

Implementation brief for **PT** in `PLAN.md`. IMPLEMENTED 2026-11-17:
PT.1–PT.8 are closed in the ledger, and the "current boundaries" section
below is kept as the starting-point record rather than as a live description.
The slide-tile theme menu and header default picker have been removed; the
left-hand rows and the keep/reflow dialog are the apply entry point.

## Agreed workflow

Apply a theme to a **whole song or scripture passage from its left-hand row**,
not to one slide in Show's central service grid. A Library song remembers a
portable copy of the chosen look for future services. It does not follow later
edits to a shared theme in another service. Scripture remains service content;
this task does not create a shared scripture Library.

Applying a theme replaces old per-slide text/layout overrides so the whole item
wears the chosen theme. Preserve original-case words, authored media, images,
audio, attribution and reporting metadata. Changes are off air until an explicit
Go, including when the edited item is currently live.

1. Right-click one song/passage row in the left-hand Library/Service pane and
   choose **Apply theme…**. The target is the clicked record, not a stale editing
   cursor or an unrelated multi-selection. Offer the same action on the keyboard
   context menu; keep target identity while the dialog is open.
2. Pick a theme and show the target title. For an existing service presentation,
   offer **Keep slide breaks**, **Reflow text**, and **Cancel**. Both apply the
   theme; the first keeps all current slide boundaries. Use these action labels
   instead of explanatory paragraphs. For a linked song, retain only the short
   note that its look is also remembered in the Library. Show repeated-entry
   scope only when more than one service entry is affected, and explain why
   reflow is unavailable only when blocked. In the implementation notes below,
   “Apply only” and “Apply and reflow” name these two existing operations.
3. Apply and reflow measures the current words against the selected theme,
   preserving semantic section/verse boundaries. Show preparation/progress and
   allow cancellation before commit. Failure leaves the existing theme, slides
   and Library preference unchanged; do not silently fall back to Apply only.
4. A Library song not yet prepared in this service only needs **Remember theme**
   and Cancel: it has no existing slide breaks to reflow. Its next explicit Add
   to Service paginates using the remembered look. Theme application must not
   add a hidden presentation or running-order entry as a side effect.

An item's repeated service entries share one presentation today: name the
number of affected occurrences in the dialog and update all references to that
same show. Do not silently create per-occurrence copies. Applying to a Library
song also updates its existing linked presentation in the open service, if any;
other saved services are not opened or rewritten.

Remove Show's `cueThemeMenu` theme/follow-theme/customization actions from
individual tiles. Remove its separate show-default theme picker as an alternate
unguarded apply route; the left-hand item action is the operator entry point.
Retain explicit per-slide styling/customization in Edit. Route any remaining
item-level apply shortcut/menu through the same workflow. Offer **Use service
default** as the whole-item unassignment: remove the remembered Library preference
for a linked song and return the service item to inheritance, with the same
slide-break/reflow choice where applicable.

## Current boundaries and why simply moving the menu is insufficient

- `ShowPage` currently applies `applyThemeToSlide()` to the clicked tile.
  `PlaylistPanel` has another apply menu, while `CurrentServicePanel` has no
  theme action for a passage row. The Library mixes catalog song IDs and local
  presentation IDs, which must be resolved explicitly rather than interchanged.
- `applyThemeToShow()` currently writes a theme ID onto every slide. It does not
  remember a Library preference, remove text overrides or repaginate. The current
  service theme can still mask that choice because it precedes slide/show themes.
- Themes are owned by a service package; the catalog is independent application
  data and currently holds canonical song stanzas with no theme definitions.
  Storing only a service-local theme ID in the catalog would break future use.
- Songs have canonical stanzas in the Library. Scripture import currently hands
  the service generated lines and notes plus reference/translation/attribution;
  the original selected verse structure does not survive as a separate source.
- Pagination uses worker-measured box/font/spacing/effects and the TL layout
  pair. There is no authoritative theme line-count setting to reuse.
- `upsertCatalogSong()` currently replaces a projection's slides wholesale.
  Calling it unchanged for Apply only or a media-bearing reflow would lose
  authored slide state. The Library bridge's broad item-change refresh also
  needs to distinguish a remembered-look change from lyric content changes.

## Portable remembered look and persistence

Introduce a Qt-free `ThemeSnapshot` value/validated codec shared by domain and
catalog, reusing existing appearance records and validation rather than an
opaque JSON blob or a second style vocabulary. The catalog may depend on the
Qt-free domain for this type; the domain must not depend on the catalog.

The snapshot contains the display name and complete effective theme settings:
text colour, family/size, horizontal/vertical alignment, All caps, line spacing,
weight/slant/underline, fit/minimum size, stroke/shadow, primary box placement,
resolved movement and named contexts. Resolve silent dependencies against the
chosen theme's normal Default/service fallback **at capture time**, using the
same fixture/resolver as Theme Editor, with the unrelated current-theme override
excluded. Include context names and resolved context appearances, retaining
unknown valid names. No service IDs, file paths or theme background media are
needed. Font family names are remembered; bundling fonts or guaranteeing the
same fallback fonts on another machine is outside this task.

At this checkpoint catalog/service/scene/protocol versions are **1/14/8/0.20**.
Reserve catalog **2** for `CatalogItem.themeSnapshot` (optional), and service
**15** for the explicit item assignment and reflow source below. Scene 8 and
protocol 0.20 remain sufficient because the worker receives existing resolved
scene and measurement types. Recheck allocation before implementation.

- Catalog v1 loads with no preference; strict v2 snapshot validation, existing
  theme/context limits and catalog byte budgets apply before durable writes.
  Old-format new keys and malformed partial snapshots refuse without mutation.
  Snapshot changes increment record/content revisions and retain canonical lyrics.
- Remember a look by value, never by source theme name/ID. Materialise it as an
  ordinary service-local theme when used in a service. Reuse a full-definition
  match of the resolved appearance (including inherited contexts/movement),
  not a sparse-record or same-name match; prefer the chosen local name among
  equivalent looks. Otherwise mint a fresh local ID and a unique display name
  such as Brun (2). Do not overwrite a different existing theme named Brun,
  Default, etc. Explicit creation and Rename refuse an existing trimmed name
  without mutation. New/Duplicate/import suggest a free label before creation.
  Loading or recovering an older service disambiguates duplicate labels while
  preserving every theme ID, assignment and appearance.
- A local theme edit can change shows using that local definition as today, but
  it cannot alter the Library snapshot. Only another explicit item application
  replaces the remembered look. Adding the song to a new service uses the stored
  snapshot, independently of the first service and its later theme edits.
- Saved services include their own theme definitions and remain playable with
  the Library absent. Opening an old saved service does not silently restyle or
  repaginate it against a newer Library preference.

## Whole-item assignment and override removal

Add optional `Show.appliedThemeId` in service 15. For a show with this explicit
assignment, its theme chain is **slide theme → applied item theme → Default**,
with slide customizations still winning per field. The applied theme is the
complete snapshot, so an unrelated service current theme cannot mask it.
Appearance and transition resolution must use the same branch. After an
application all old slide theme IDs have been cleared; a later deliberate
per-slide Edit choice can still override the item.

When `appliedThemeId` is absent, preserve the legacy current → slide → show
default → Default chain exactly, including services 1–14. Use service default
removes the explicit assignment and clears the target's legacy show-default and
slide theme assignments; do not clear the global current theme and change other
presentations.

Apply only sets this one item assignment and removes every slide's text colour,
font/style/layout/effects/placement customizations and old slide theme ID.
Preserve backdrop colour/gradient overrides, background asset/fit/camera settings,
authored item geometry and item order, source text, notes, groups and media/audio
references. Primary text can resolve to the new theme's box without destroying
its stored authored rectangle. Additional boxes keep their authored rectangles.
Keep slide/item IDs and slide counts unchanged for Apply only.

Do not use blanket Remove customizations: the customization record also carries
background fields that this operation must preserve. Share the domain's field
classification with the adapter so future style fields are not omitted.

## Reflow source, eligibility and word preservation

Persist a bounded, ordered `Show.reflowSource` in service 15, with a source kind,
section/verse block identities and original-case lines, plus a fingerprint of
the service text projection it describes. Song preparation captures the stanzas
actually used; scripture Add captures only the selected verses, references,
translation and attribution used by that Add. This is authored service content,
not a chapter/provider cache or an installed Bible database.

Reflow always preserves the words currently in this service. Never replace
them with the latest Library lyrics, refetch a passage, uppercase stored words,
or infer missing verses from a displayed verse range. A source record is usable
only while its projection fingerprint matches. Text edits invalidate the match;
style-only edits do not. Undo/redo, save/reopen and Library lyric refresh must
carry or invalidate source metadata consistently.

For simple legacy or edited items without a matching source:

- Songs may reuse catalog stanzas only after verifying their words/order match
  the service projection. Otherwise build conservative blocks from the current
  primary text, preserving existing section boundaries and authored line breaks;
  never merge a Chorus into a Verse or replace local wording.
- Scripture can reflow its current text as ordered paragraph blocks without
  guessing verse boundaries. New labels identify the whole known passage rather
  than inventing precise verse ranges. Retain original notes/provenance with
  the contributing source blocks and carry them to the first new slide containing
  each block. Structured imports retain their accurate verse labels.

Initial reflow eligibility is deliberately bounded: one primary text item per
content slide, optional existing leading blank cue, and a common background/
fit/camera identity across the text run. Preserve that leading cue, show audio
and common background on the new slides. Additional foreground images/text
boxes, internal wordless cues or differing per-slide media disable Reflow with
a specific reason; Apply only remains available. Picture-only/PPTX decks do not
gain a fake text-reflow action. Never flatten, duplicate or discard authored
media to make an unsupported item pass eligibility.

## Preparation, commit and history

Use one frontend apply session with explicit catalog/show target IDs, chosen
theme snapshot, keep/reflow mode and a captured service generation. Prepare a
copy of the resulting Documents so the measurement fixture already has the
future item assignment and the intended override removal. SongPlanner and
scripture/textflow reuse the worker measurement port with this captured style;
they must not consult whatever song/theme the browsing cursor later selects.
Preserve section starts, original words, All caps display semantics, caption
reservations and the existing fit policy. Do not shrink fonts merely to avoid
creating the extra slides reflow requires.

Keep work bounded/latest-wins and recheck target identity, source/Library
revisions, theme fingerprint, media state and worker generation before commit.
Cancel on service replacement, target deletion or supersession; retire delayed
measurement/preview replies. Reflow requires the measuring worker; Apply only
can work offline. Never block the UI on measurement or change the audience.

Validate and stage the complete service edit before publishing any change. For
a linked Library song, commit its snapshot durably through CatalogController
before publishing the staged in-memory service edit; a refused catalog write or
stale/invalid service plan publishes neither. Run no fallible preparation after
that durable commit. Keep service autosave/dirty-state acknowledgements under
the existing persistence contract. These are independent stores: do not claim a
crash-atomic two-file save. A crash between Library commit and service autosave
may retain the new future-use preference and the older recoverable service;
each must remain internally valid and self-contained.

One service undo restores the whole theme/slide transaction, including old IDs,
words, media and source metadata; redo restores the prepared result without
re-querying the provider. Library preference history remains Library history:
its undo restores the remembered preference without retroactively rebuilding
service presentations. Use meaningful undo labels for the two scopes. A
theme-only catalog notification must not trigger the old automatic lyric
refresh/reflow path and defeat the user's Apply only choice. New Add requests
measure using the snapshot; lyric refresh measures using the linked show's
effective item theme and preserves its common media/identity policy.

## Acceptance and delivery

Use PT.1–PT.8 in `PLAN.md`; target 0.5–2 engineer-days per leaf and split further
when needed. Preserve previous DONE milestones and keep unfinished apply paths
disabled until their persisted/rendered behaviour and UI are wired together.

- Domain/storage: v1 catalog/v14 service migration, strict malformed/version
  refusal, snapshot completeness, name collision, unknown contexts, inheritance
  and explicit item precedence without changing legacy pixels or unrelated shows.
- Library: remember a look in service A, edit/delete A's theme, restart and add
  the song to service B with a colliding theme name; its remembered look and
  measured wrapping survive. Load a saved service without the Library and verify
  playback. Preference changes never change canonical lyric/metadata content.
- Apply only: reset old slide themes and text/layout overrides across the whole
  item, preserving slide/item IDs, breaks, notes, media, other presentations and
  repeated-entry identities; a new item theme must visibly win over a current
  global theme. Preserve background overrides while removing text overrides.
- Reflow: larger/smaller boxes/fonts and All caps cause expected boundary/count
  changes using the real worker; source words/order/sections and notes/provenance
  survive. Cover structured scripture, saved legacy passages, edited service
  wording, a leading blank, common video/camera/audio, ineligible multi-box and
  mixed-media items, and impossible layouts with zero partial changes.
- Async/failure: browsing or changing the target/theme during measurement,
  Library edits/deletion, service replacement, worker reconnect, cancellation,
  refused durable catalog write and stale replies cannot commit a partial or
  mis-targeted operation. No hidden refresh repaginates Apply only.
- UI/journey: left Library songs and Service song/passage rows, keyboard context
  menus, named target/occurrence count, the three dialog outcomes and eligibility
  reasons; central slide tiles have no theme-apply menu and the old header apply
  route is gone. Preview changes off air; Go alone changes acknowledged output.
  Undo/redo and save/reopen preserve the result and continuous media identity.

Run focused catalog/domain/controller/planner/UI/worker journeys, then desktop,
headless and source guards. Update the theme/control-set/architecture contracts
and archive detailed completion evidence. No runtime or physical acceptance is
claimed by this planning document; TV/TA/ND/M7 gates remain open.
