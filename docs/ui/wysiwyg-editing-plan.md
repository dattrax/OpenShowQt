# WYSIWYG editing, slide ordering and insertion (EW)

Status: implementation record. `PLAN.md` EW.1–EW.10 are the ledger; this
file is the frozen interface contract EW.1 agreed before implementation and
the place each leaf records what it actually built.

## Product rules

- The words on the stage are always the worker's render. The UI paints only
  editing chrome: caret, selection, IME composition underline and the box
  outlines/handles.
- One click selects a box. Double-click on a text box, Enter while the canvas
  holds a selected text box, or **Edit text** on the box menu enters text
  mode. F2 keeps its output meaning (clear words); it never enters text mode.
- Escape first cancels an IME composition or an active selection drag, then
  leaves text mode keeping the committed words. It never clears audience
  output.
- Clicking outside the edited box also ends text mode and keeps committed
  words, including clicks on non-focusable blank canvas. Moving to an
  inspector preserves the destination's focus; there is no Done button.
- Up/Down move across the worker's rendered lines, including soft wraps and
  empty lines. Preserve the preferred horizontal position through short
  lines; Shift extends the selection. Serialize repeated key moves through
  matching worker geometry and retire pending moves when typing or clicking.
- Text previews stay debounced through the existing `documentsChanged`
  path. Caret/selection geometry is asked per edit with latest-request-wins;
  a late reply never moves a caret or repaints another slide.
- A worker without `text.editor.v1`, or a disconnected worker, keeps the
  source-text drawer as an explicit fallback labelled **Source text**; it is
  source editing, not WYSIWYG.
- The audience never moves while editing, moving or inserting. Only an
  explicit Go presents.

## Identity

| Thing | Identity |
|---|---|
| Editing session | `showId` + `slideId` + `boxId` + draft generation |
| Layout request | the JSON-RPC request id (monotonic; `request` is echoed for diagnostics) |
| Layout reply | `request`; carries no document authority |
| Scene item | index in the compiled scene; `compileSlide` emits one item per authored box in draw order, so item index = box index in `docs.currentSlideBoxes` |
| Text offsets | UTF-16 code units of the item's source text (authored lines joined by `\n`), i.e. exactly Qt's `QString` offsets |

A reply is installed only when its request is the newest for the current
session and the session's `showId`/`slideId`/`boxId` still hold. Navigation,
box selection, insert/move, undo/redo, save, page leave and disconnect retire
the session.

## Controller verbs (EW.6)

```cpp
// Final position after removal (the domain rule). No-op when toIndex names
// the slide's own final slot; refusal leaves the model and history alone.
bool moveSlideFor(const QString& showId, const QString& slideId, int toIndex);
bool moveCurrentSlideTo(int toIndex);
bool moveCurrentSlideStep(int delta);       // -1 up, +1 down
// Insert one fresh slide before/after `anchorSlideId`; an empty anchor
// appends. Returns the new slide id, or "" with a status sentence.
QString insertSlideFor(const QString& showId, const QString& anchorSlideId, bool after);
QString insertSlideAt(const QString& showId, int index);   // empty list = 0
```

- Every verb flushes pending text, validates the show and slide/anchor ids,
  makes ONE `edit()` (one history entry), and re-anchors the selection by
  slide id. `moveCurrentSlideStep` and the rail's insertion gap both translate
  to the domain's final-position rule exactly once.
- An insertion inherits the anchor's `group`, `textContext`,
  `backgroundAssetId` and `backgroundFit`, leaves `themeId` empty (the show's
  theme answers), and never copies words, notes, foreground items or per-slide
  style overrides. With no anchor it uses document defaults.
- A leading blank cue is an ordinary slide for these verbs: an explicit
  before/after position is obeyed and the cue is neither moved nor recreated.
- Undo/redo keep the selected slide by id while it exists, then clamp; the
  fresh slide of an insert is selected immediately.

## Worker editor layout (EW.2/EW.3)

`text.editor` — capability `text.editor.v1`, additive to protocol minor 20.

Request:

```json
{
  "request": 7,
  "scene": { ...scene JSON as scene.prepare accepts... },
  "item": 0,
  "lines": ["draft line", "second"],
  "caret": 12,
  "selection": [3, 12],
  "point": {"x": 400.0, "y": 220.0}
}
```

- `scene` is the compiled slide, bounded by the existing scene codec. `item`
  must name a TEXT item. `lines` is an optional bounded draft override for
  that item (the same line/count/character caps as a scene item); absence
  means the scene's own lines.
- `caret` and `selection` are UTF-16 source offsets, clamped to the item's
  source length. `selection` is `[anchor, focus]`; the worker returns rects
  for the ordered span.
- `point` is a point in the authored 1920×1080 canvas; the reply answers the
  nearest grapheme boundary.
- At least one of `caret`/`selection`/`point` is required; an ask with none
  is refused by name.

Reply (all boxes/points in authored canvas units):

```json
{
  "request": 7,
  "item": 0,
  "box": {"x": 160.0, "y": 100.0, "width": 1600.0, "height": 880.0},
  "fontSize": 96.0,
  "lineCount": 2,
  "textLength": 25,
  "caret": {"x": 300.0, "y": 180.0, "height": 110.0, "offset": 12, "line": 0},
  "selection": [{"x": 100.0, "y": 180.0, "width": 200.0, "height": 110.0}],
  "hit": {"offset": 12, "line": 0}
}
```

- Geometry comes from the SAME `SceneTextEngine::layoutInBox` result that
  paints: paragraph origin `(paragraphLeft, paragraphTop)`, glyph cluster
  bounds for carets, `getRectsForRange` for selection. No second font
  measurement exists.
- With `allCaps` on, the worker shapes the displayed capitals and maps
  source UTF-16 offsets to display offsets (and back for hit tests) from the
  same uppercase transform that renders. Authored case is never rewritten.
- Nothing is stored, prepared, presented, cached or woken. A malformed,
  oversized, stale (`request` older than the last for this connection) or
  non-text ask is refused with a named sentence.

## Rail gestures (EW.7/EW.8)

- Each tile carries a visible grab handle (`editorSlideGrabHandle`). A press
  on the handle starts a drag with a ghost and an insertion gap drawn
  before/between/after cards. Edge autoscroll is bounded by one row per tick.
- Dropping converts the visible gap `g` (0..count) to a final position
  `from < g ? g - 1 : g` exactly once and calls `moveSlideFor`. Escape, a
  release outside the rail, a drop on the source's own gap, or a source slide
  that vanished cancels with no edit and no history.
  Capture that final position before clearing the drag source state, and
  validate both horizontal and vertical release coordinates against the lane.
- The context menu and the keyboard while the rail owns focus offer Move up,
  Move down, Insert before, Insert after. Move up/down are inactive at the
  ends; the footer's **Insert slide** inserts after the selected slide and
  position 0 for an empty presentation.
- The selected slide stays selected by id after a move; an insert selects and
  reveals the fresh slide and asks the canvas to enter text mode when this
  worker can edit in place.
