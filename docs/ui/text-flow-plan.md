# Text Flow (milestone TF) — the theme decides how text fills a box, for
every kind of text

The operator's first live run brought the paging symptom (every verse its
own slide, BI-F/BIF.1); their note for what they actually want:

> Ideally I'd like to package all the download ascii text together and
> intelligently split the text using a theme. The current theme has one
> view which is songs, but if the theme has a bible context also, then
> this can be part of the theme, but when showing scripture it can be
> slightly different (for example singing songs may only have two lines
> of text whereas bible reading may have 6 lines). At some point we'll
> need to add an import of songs from ASCII, so the split/sizing code
> can be reused at this point, as it'll be common.

Today a theme carries exactly one appearance and the scripture paginator
carries its own notion of a slide: whole verses packed at one size, sized
to fit, no line budget. The two halves of this milestone are the theme
learning to speak of more than one **text context**, and the packing
learning to be a **generic text-flow engine** the scripture importer and
the future ASCII-song importer both call.

**The line count is the answer, not the input.** The operator's "songs two
lines, bible reading six" is a description of what a well-sized context
*looks like*, not a number to be dialled in: the number of lines is
whatever sensibly fits the theme's bible box, never a fixed split. So a
context declares typography and geometry, and the engine lets the
measurement settle how many lines each slide carries. Nothing anywhere in
TF splits at N lines.

## The theme's text contexts

A theme's appearance (colours, background image, typography) stays what
it is. A context is a **typography-only override** of it, addressed by
name:

- The base — today's appearance — is what song slides and everything else
  already use. Nothing changes for them.
- A theme may carry named contexts. The known consumer names are
  `scripture` (the Bible page) and, later, whatever the ASCII song import
  asks for; unknown names in stored files round-trip untouched.
- A context may override: font family, font size, line spacing, alignment,
  and the text box (inset or an explicit rectangle) — the "bible part of
  the theme" the operator means. It may NOT set a line count: how many
  lines a slide carries is what the measurement says fits that box at that
  typography. Colours and backgrounds do not belong to a context — the
  scripture slides of a theme look like that theme, they just fit more
  text.

Storage: `themes[].contexts` is an optional array
(`{ name, font_family?, font_size?, line_spacing?, alignment?, inset?,
box? }`). Absent means exactly today. Slides gain an
optional `text_context` string — an imported passage carries
`"scripture"`; song slides carry nothing. Both are additive and bump the
storage format's minor; old files load with no contexts and no slide
context, and render exactly as they do now.

Resolution: one resolver, one path. `Documents::appearanceForSlide`
layers base theme → the slide's named context → the slide's own
customizations (that order is today's precedence, unchanged), and
`scene.cpp`'s single call site needs no new logic. The importer's and the
preview's appearance lookups become context-aware the same way — there is
no second resolver for previews (the BI rule: the preview runs the same
machinery as the output).

## The shared text-flow engine

`paginateScripture` already *packs* multiple whole verses per slide and
splits a too-long verse at a sentence boundary — the engine is mostly
built; it is scripture-shaped. Extract the shape out of it:

```
TextFlowInput {
    blocks:  [{ id, lines }],   // ordered units that stay whole if they can
    box, style,                 // the resolved (context-aware) appearance
    reserved_height,            // reference + attribution, as BI.4
    labeler(blockIds, continued) -> slide label
}
->  TextFlowOutput { slides: [{ blockIds, lines, label, continued }], error }
```

What stays exactly as BI.4 proved it: the only measurer is the worker's
`text.measure` port (`AwaitingMeasurement` + the content-keyed cache),
greedy whole-block packing first, sentence split before word split,
continuation slides labelled, nothing clipped, nothing estimated — and no
size shrinking: the context's size is the size.

Note what the input does **not** carry: any line count. The operator's
"six lines" is what the scripture context's box at its reading size
measures out, and "two lines" is what the song look's display size
measures out — the same engine, two contexts, no split number anywhere.
If the scripture box at its size happens to hold nine lines, it paginates
nine; the theme expresses the look and the measurement decides the rest.

Adapters on top, thin by construction:

- **Scripture** (now): the blocks are the selected verses in canonical
  order — the whole downloaded passage as one run of text, which is the
  operator's "package all the download ascii text together" — the labeler
  produces `Gen 1:4–6` and `Gen 1:7 (continued)`, and the reference and
  attribution reserve room as BI.4 does.
- **ASCII song import** (later milestone): the blocks are the stanzas or
  paragraphs of the imported text, the labeler produces stanza numbers or
  nothing, and the base (song) context applies — whose display size is why
  songs read as two lines. That importer writes no pagination code — it
  calls this engine.

## Leaves

| Leaf | Delivers | Acceptance | Depends on |
|---|---|---|---|
| **TF.1** | Theme contexts and slide `text_context` through storage and the resolver. | Optional `themes[].contexts` + per-slide `text_context` round-trip; unknown names preserved; old files load and render unchanged; `appearanceForSlide` layers base → context → slide customization; a scripture slide with a context overrides renders at them (PIL) and a song slide of the same theme is pixel-unchanged. | BI.5 |
| **TF.2** | The generic engine extracted, scripture as adapter. | `textflow` component holds packing/splitting/continuation/reserve/measurement-port logic; `paginateScripture` becomes blocks+labeler+reservation arithmetic over it with its existing suite passing unchanged; engine tests cover stanza-shaped blocks with a stanza labeler, one whole block never split when it fits alone, and the no-fit error; the input carries no line count. | TF.1 |
| **TF.3** | Scripture import flows the whole passage through the engine with the theme's `scripture` context. | Live-shape chapter fixture: with the context's reading size in force the passage packs multiple verses per slide — the line count is what the measurement fits, not a setting — never one verse per slide, preview identical to output (same call), labels/notes/provenance as BI.5; a theme without the context behaves exactly as today; BIF.1's production-shape findings are the fixture's input. | TF.2 |
| **TF.4** | Theme editor UI for the scripture context; the flow stated on the Bible page; re-acceptance. | Editor section sets the reading context's typography and box (never a line count — the editor says how many lines the box measured out, as information); the Bible page's plan sentence names the context it used; e2e: import a chapter under an edited theme and the PGM slide matches the previewed flow; docs updated. | TF.3, BIF.4 |

Ordering: BIF.1 first regardless — it explains the live shape (font size
defaults and the attribution reservation) that the scripture context then
sizes properly, and the operator's Add is blocked until it lands.
BIF.2/BIF.3 stay independent. TF.1→TF.2→TF.3 is one continuous thread;
TF.4 closes it with the UI and the operator-facing re-acceptance. The
future ASCII song import is a milestone of its own against this engine,
not a leaf here.

## As built (milestone TF delivered, 2026-11-03)

All four leaves shipped as planned; the design rule held everywhere —
**no number anywhere named, defaulted, or nudged a line count.**

- **TF.1** — format 7: optional `themes[].contexts[]`, each a `{name,
  fontFamily?, fontSize?, lineSpacing?, align?, inset?, box?}` vetted at
  load (a field that smells like a line count or a colour is refused by
  name), and per-slide `text_context`. `appearanceForSlide` layers the
  first context in chain order that carries the name, wholesale and per
  property, between the slide's customization and the themes' own values;
  colours can never arrive through a context, and a name no theme carries
  is no layer at all — old files and nameless slides resolve untouched.
- **TF.2** — `openshow::importer::textflow` is the engine: blocks of
  lines, a box, a typography, a reserved height, the `MeasureFn` port.
  Greedy whole-block packing, sentence-before-word splitting,
  continuation labels by the caller's `LabelFn`, and every refusal naming
  the block it cannot place. `paginateScripture` is the scripture
  adapter; BI.4's suite passed the extraction unchanged. The future ASCII
  import calls the same engine with a stanza labeler.
- **TF.3** — the Bible page's measurement style, off-air preview and
  added slides all name `scripture` through the one resolver. BIF.1's
  chapter on the real worker: the plain 96 px look measures 19 slides;
  the same theme's 32 px lectern context measures 4 — multiple verses per
  slide because that is what the measurement fits. Every added slide
  carries the name into format 7; labels stay in notes, provenance on
  the show.
- **TF.4** — the Theme Editor's inspector gained a **Scripture Reading**
  section: the toggle creates the context from the theme's current look
  and box, the fields commit size/alignment/spacing and the reading box
  (an empty field is silence, falling through to the themes; a refused
  value snaps the fields back), and the section states "the box holds ≈ N
  lines of this size" — measured by the same `text.measure` port the
  paginator uses, as information only. The resolver itself answers which
  context layered (`ResolvedAppearance.contextName`), so the Bible page's
  plan sentence says "…measured to fit **in the theme's scripture
  context**…" in the resolver's own word, and stays byte-identical when
  none answered. The e2e re-acceptance walks the whole route: apply the
  context through the editor's verbs, look the chapter up, Add, save,
  reopen — the schedule holds exactly the previewed slides, each saved
  scene compiles at the context's size inside its box, and the presented
  PGM picture is the previewed flow.

The engine never clipped, repeated or silently shrank a word across every
fixture (the identity assertion `identityHolds` guards each one), and the
gates closed at 64/64 desktop serial and 46/46 headless.
