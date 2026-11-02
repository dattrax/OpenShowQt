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

## The theme's text contexts

A theme's appearance (colours, background image, typography) stays what
it is. A context is a **typography-only override** of it, addressed by
name:

- The base — today's appearance — is what song slides and everything else
  already use. Nothing changes for them.
- A theme may carry named contexts. The known consumer names are
  `scripture` (the Bible page) and, later, whatever the ASCII song import
  asks for; unknown names in stored files round-trip untouched.
- A context may override: font family, font size, line spacing,
  alignment, **maximum lines per slide**, and box inset. Colours and
  backgrounds do not belong to a context — the scripture slides of a
  theme look like that theme, they just fit more text.

Storage: `themes[].contexts` is an optional array
(`{ name, font_family?, font_size?, line_spacing?, alignment?,
max_lines?, inset? }`). Absent means exactly today. Slides gain an
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
    max_lines,                  // optional budget from the context
    reserved_height,            // reference + attribution, as BI.4
    labeler(blockIds, continued) -> slide label
}
->  TextFlowOutput { slides: [{ blockIds, lines, label, continued }], error }
```

What stays exactly as BI.4 proved it: the only measurer is the worker's
`text.measure` port (`AwaitingMeasurement` + the content-keyed cache),
greedy whole-block packing first, sentence split before word split,
continuation slides labelled, nothing clipped, nothing estimated.

`max_lines` is genuinely new: when a context declares a line budget (the
operator's "bible reading may have 6 lines"), the engine sizes the text —
binary search over the theme's allowed sizes, each candidate measured by
the worker — so a slide carries as many of the packed blocks as the
budget allows, never exceeding the box and never going below the theme's
floor. Without a budget, today's "fit everything at the chosen size"
behavior stands.

Adapters on top, thin by construction:

- **Scripture** (now): the blocks are the selected verses in canonical
  order — the whole downloaded passage as one run of text, which is the
  operator's "package all the download ascii text together" — the labeler
  produces `Gen 1:4–6` and `Gen 1:7 (continued)`, and the reference and
  attribution reserve room as BI.4 does.
- **ASCII song import** (later milestone): the blocks are the stanzas or
  paragraphs of the imported text, the labeler produces stanza numbers or
  nothing, and the base context (two display lines) applies. That
  importer writes no pagination code — it calls this engine.

## Leaves

| Leaf | Delivers | Acceptance | Depends on |
|---|---|---|---|
| **TF.1** | Theme contexts and slide `text_context` through storage and the resolver. | Optional `themes[].contexts` + per-slide `text_context` round-trip; unknown names preserved; old files load and render unchanged; `appearanceForSlide` layers base → context → slide customization; a scripture slide with a context overrides renders at them (PIL) and a song slide of the same theme is pixel-unchanged. | BI.5 |
| **TF.2** | The generic engine extracted, scripture as adapter. | `textflow` component holds packing/splitting/continuation/reserve/measurement-port logic; `paginateScripture` becomes blocks+labeler+reservation arithmetic over it with its existing suite passing unchanged; engine tests cover stanza-shaped blocks, a line budget sizing slides to it, and the no-fit error. | TF.1 |
| **TF.3** | Scripture import flows the whole passage through the engine with the theme's `scripture` context. | Live-shape chapter fixture: with a 6-line scripture context the passage flows across ~6-line slides — not one verse per slide — preview identical to output (same call), labels/notes/provenance as BI.5; a theme without the context behaves exactly as today; BIF.1's production-shape findings are the fixture's input. | TF.2 |
| **TF.4** | Theme editor UI for the scripture context; the flow stated on the Bible page; re-acceptance. | Editor section sets the reading context's lines/size; the Bible page's plan sentence names the context it used; e2e: import a chapter under an edited theme and the PGM slide matches the previewed flow; docs updated. | TF.3, BIF.4 |

Ordering: BIF.1 first regardless — it explains the live shape (font size
defaults and the attribution reservation) that TF.2's line budget then
controls properly, and the operator's Add is blocked until it lands.
BIF.2/BIF.3 stay independent. TF.1→TF.2→TF.3 is one continuous thread;
TF.4 closes it with the UI and the operator-facing re-acceptance. The
future ASCII song import is a milestone of its own against this engine,
not a leaf here.
