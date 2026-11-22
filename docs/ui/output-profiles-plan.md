# Output profiles, stage HDMI, NDI lower thirds and layer routing (FO)

Status: **FO.0 frozen (2026-11-22).** This document is the contract every FO
leaf implements against; the PLAN entry keeps the research findings and the
task ledger. Nothing here activates a physical display or an NDI sender, and
no SDK header may reach the GPL frontend or worker.

## 1. The three dimensions

A cue is drawn differently for different destinations. Three independent
concepts say "what" and "where", and conflating any two of them is the bug
this section exists to prevent:

| Dimension | Values | Lives in |
|---|---|---|
| Content | Songs (`""`), Bible (`"scripture"`), any future named context | the slide's `textContext`, unchanged |
| Output role | `audience`, `stage`, `broadcast` | `domain::OutputRole` |
| Transport | HDMI monitor, NDI stream | machine workspace settings, never a theme |

- **NDI is not a content type.** The Theme Editor's "NDI" tile edits
  `broadcast`: the same bytes OBS receives could also be a hardware router's
  input tomorrow. `slide.textContext = "ndi"` does not exist and will not.
- **Stage is not a scroll of the audience picture.** It is its own profile
  with its own geometry and its own layer set.
- The audience keeps today's chain exactly: `audience` has no profile record
  and no new field. Every existing service file resolves to exactly the
  pixels it drew before FO.

The editor presents four tiles — Songs, Bible, Stage, NDI — where Stage and
NDI each carry a Songs/Bible content selector. Internally that is the pair
`(role, content)`:

| Tile + selector | role | content |
|---|---|---|
| Songs | `audience` | `""` |
| Bible | `audience` | `"scripture"` |
| Stage → Songs | `stage` | `""` |
| Stage → Bible | `stage` | `"scripture"` |
| NDI → Songs | `broadcast` | `""` |
| NDI → Bible | `broadcast` | `"scripture"` |

## 2. Domain record: `OutputProfile`

An output profile is an OPTIONAL, NAMED appendage of a theme. It is not a
second theme and it never carries service ids, asset paths or background
media:

```cpp
enum class OutputRole { Audience = 0, Stage = 1, Broadcast = 2 };

struct OutputProfile {
    OutputRole role;          // kStage or kBroadcast only
    std::string content;      // "" = Songs, "scripture" = Bible, other names round-trip
    // Sparse, exactly the Theme/TextContext vocabulary:
    std::optional<std::string> textColor;      // profile MAY restyle colour
    std::optional<std::string> fontFamily;
    std::optional<double> fontSize;
    std::optional<double> lineSpacing;
    std::optional<std::string> align;
    std::optional<BoxRect> box;                // the profile's rectangle
    std::optional<double> inset;               // or a shrink, never both
    std::optional<std::string> verticalAlign;
    std::optional<bool> allCaps;
    std::optional<int> fontWeight;
    std::optional<std::string> fontStyle;
    std::optional<bool> fontUnderline;
    std::optional<std::string> fitMode;
    std::optional<double> minFontSize;
    std::optional<TextStroke> stroke;
    std::optional<TextShadow> shadow;
};
```

`Theme::profiles` is an ordered list. Names are unique by
`(role, content)`: setting the same pair replaces it in place, keeping its
position. Other content names are retained verbatim (the `TextContext` rule),
so a future importer can name a view before this build styles it.

Validation is the domain's existing sentences, not new ones: colour syntax,
`align` word set, weight band, `fontStyle`/`fitMode` word sets, effect bands,
a placeable box, inset `>= 0`, non-duplicate `(role, content)`, and the
content name bounded like a context name. A profile naming `audience` is
refused: the audience has no profile record.

## 3. Resolution and precedence

`Documents::appearanceForSlideOn(showId, slideId, role)` is the one entry
point. For `Audience` it returns `appearanceForSlide` unchanged.

For `Stage` and `Broadcast` the answer is built in three layers, nearest
first:

1. **Authored profile.** The first theme in the slide's normal chain
   (`themeChainForSlide`: slide theme → applied item theme or current theme →
   show default → Default) that carries a profile for the slide's
   `(role, textContext)`. Each property is taken independently; an explicit
   `false`/`enabled:false` is an answer. A profile silent on a property falls
   through.
2. **The role's built-in geometry.** When the profile does not place a box,
   the role's own default box applies. This is what makes the stage box
   independent of the audience geometry no matter which slide is on air.
3. **The audience resolution.** Everything else (colour, type, effects,
   alignment) starts from the look the audience would draw, including slide
   customizations. Silence therefore inherits, and a person can style a
   whole service once.

Consequence: **per-slide customizations never move stage/broadcast geometry**
and profiles never change audience pixels. A profile can restyle colour and
type; an explicit audience choice made per slide is inherited only where the
profile is silent.

Built-in role geometry (authored 1920×1080 canvas units):

| Role | x | y | width | height | Anchor | Fit |
|---|---|---|---|---|---|---|
| Stage | 160 | 120 | 1600 | 560 | centre | inherit (shrink) |
| Broadcast | 120 | 760 | 1680 | 240 | bottom | inherit (shrink) |

The stage box leaves the lower two-fifths for the next-cue text of FO.5b; the
broadcast box is the lower third. Both are *defaults* an operator overrides
with the tile's own box fields. A profile's `fitMode` is honoured, and a
sentence says when the selected words cannot fit the box at the selected
minimum: the preview shows a reason and the compile refuses that target.
Truncation, independent slide advancement and service reflow are forbidden.

## 4. Persistence

### 4.1 Service format 16

`kServiceFormat` becomes **16**. Format 15 and every older format stay
readable. The only new key is a theme's optional `"outputProfiles"` array:

```json
"outputProfiles": [
  {"role":"broadcast","content":"scripture",
   "box":{"x":120,"y":760,"width":1680,"height":240},
   "fontSize":64,"allCaps":false}
]
```

Rules, in the file's existing style:

- The key is written only when a theme has profiles. A format-15 file loads
  with every theme carrying none — no enabled output, no changed pixel.
- Unknown keys inside a profile are refused naming themselves. A profile with
  an unknown `role`, `audience`, a duplicate `(role, content)` or an invalid
  value refuses the whole package; nothing is half-loaded.
- `"outputProfiles"` inside a pre-16 file is another build's meaning and is
  refused by era guard, not adopted.
- `box` and `inset` are mutually exclusive, exactly as on a text context.

### 4.2 Theme snapshots (portable looks)

`ThemeSnapshot` gains `std::vector<OutputProfile> profiles`, captured with the
theme's concrete audience look. Re-applying a remembered song or Library look
copies the profiles verbatim; the snapshot codec writes `"profiles"` with the
same record spelling and the same validators. Applying a snapshot over a
prepared service replaces that service's item theme profiles with the
snapshot's (value semantics: the look is what travels).

### 4.3 Machine workspace settings (NOT portable)

Hardware routes and the layer matrix belong to the machine, not the service:

```json
"output": {
  "audience": {"enabled": true,  "monitor": -1, "layers": ["background","images","words"]},
  "stage":    {"enabled": false, "monitor": -1, "layers": ["words","next"]},
  "broadcast":{"enabled": false, "source": "OpenShow", "mode": "lower-third",
               "layers": ["words"], "fps": 30}
}
```

Stored by `AppSettings` under its own group (the WorkspacePanels pattern), with
`monitor = -1` meaning "not chosen". Copying a service package or a Library
snapshot never carries a route or an enabled flag. A worker restart and an
application restart both start every destination **disabled**; enabled state
is never silently restored, only the desired routes/masks are.

## 5. Layer routing matrix

Rows are production layers, columns are destinations. The columns are the
routes; there is no global set of layer checkboxes.

| Layer (`RoutingLayer`) | Audience | Stage | NDI |
|---|---|---|---|
| Background | supported, default on | supported, default off | supported, default off |
| Images | supported, default on | supported, default off | supported, default off |
| Words | supported, default on | supported, default on | supported, default on |
| Next cue | unavailable | supported, default on once FO.5b lands | unavailable |
| Notes | unavailable | supported, default off (speaker notes source is FO.5b+) | unavailable |
| Clock | unavailable | unavailable | unavailable |

Rules:

- Background, Images and Words are selectable on **all three** destinations.
  Words-only NDI is a default, not a prohibition.
- "Next cue" and "Notes" are stage-only. An audience or broadcast mask naming
  one of them is refused by name at the RPC, at the settings codec and in the
  UI; a malformed peer cannot make notes route to the projector.
- `Clock` and the demo `Aux` badge are not in the matrix at all.
- An unchecked broadcast Background leaves alpha zero outside other routed
  content. An unchecked stage Background still clears the stage surface
  black (the stage is opaque by definition).
- Layer choice filters kinds out of the authored draw stack; it never
  reorders text and pictures relative to each other.
- Masks are independent. Editing or disabling one column changes no other.
- Clearing a destination (Clear All, or disabling its column) also invalidates
  any held broadcast frame so a late NDI receiver cannot be sent stale words.

`domain::OutputRouting` (Qt-free) owns the masks and the disabled cells; the
worker receives them verbatim. The default masks are the table's defaults.

## 6. Clear, restore and restart

- The per-layer clear/restore actions stay audience-scoped, exactly as today.
- **Clear All** clears every ENABLED visual destination (audience, stage and
  broadcast) and keeps its existing audio-stop meaning.
- **Restore** returns the acknowledged cue to every enabled visual
  destination that had it.
- A disabled column receives nothing. Checking a cell never starts a disabled
  column.
- Worker restart, driver loss, monitor removal: that destination goes black,
  reports `disabled`/`lost` and requires an explicit re-enable. It never
  migrates onto the operator's screen and never leaves a half-frame on air.

## 7. Transport and timing contract

- Audience HDMI: the operator's chosen monitor (today's window).
- Stage HDMI: a separate worker-owned borderless window on an explicitly
  selected monitor. Creating, recreating or closing it never terminates GLFW
  for the audience and never waits unboundedly on GPU work (FO.5a decides the
  host split from measurements).
- Broadcast: full 1920×1080 premultiplied render, converted once to straight
  alpha for NDI, initially 30/1 fps. Submission is recorded separately from
  presentation: a send return proves nothing about OBS.
- One scene timeline and one audio owner. Stage/NDI never start a second
  audio player and never own an independent show timeline.

## 8. Wire and UI shapes

- Theme sample compile grows an optional `role`:

  ```
  themeSampleSceneJson(themeId, contextName, role)      // role: "audience"|"stage"|"broadcast"
  themeSampleAppearance(themeId, contextName, role)
  ```

  Absent role means `audience`, so old QML and old controllers behave
  unchanged. Browsing never opens a display or starts a sender.
- The worker's scene wire is UNCHANGED (scene format 8): profiles are compiled
  in the frontend into concrete scene values. No new worker capability is
  needed for theme samples.
- Routing travels separately (protocol minor 22):
  `output.routing`  `{revision, audience:{enabled,layers}, stage:{...},
  broadcast:{...}}` → ack `{revision, accepted:true|false, reason}`. Capability
  `output.routing.v1`. Legacy workers keep the audience-only path; a frontend
  that does not know the capability never sends it and enables no extra
  output.
- Preview identity: any per-output preview negotiation (FO.2c) must use
  distinct target kinds; an unknown target id must never fall back to the
  audience stream.

## 9. Budgets

- A profile is bounded like a text context: 64 profiles per theme, name
  ≤ 256 chars; the profile record reuses the existing field bounds.
- Broadcast full-frame RGBA is 8,294,400 B; the readback pool is fixed before
  allocation (FO.2b measures before choosing 2 or 3 slots). No synchronous
  1080p `readPixels` on the audience loop.
- Layer masks are `uint32_t` bit sets; a mask naming a bit this build does not
  define is refused, never masked away.

## 10. Work ledger

| ID | Status after this contract |
|---|---|
| FO.0 | DONE — this document |
| FO.1a | domain role/profile model, resolution, validators |
| FO.1b | service format 16 + snapshot profiles |
| FO.1c | role-aware compile |
| FO.1d | controller/sample role + routing wire |
| FO.1e | worker host (not started) |
| FO.2a–c | compositor/export/preview (not started) |
| FO.3a–b | NDI sender spike + helper (not started) |
| FO.4a–b | NDI client + matrix UI (not started) |
| FO.5a–c | stage display/content/destination (not started) |
| FO.6a–c | acceptance, physical, packaging (not started) |

## 11. Open product choices (recommendations)

1. **Stage content split.** Recommended: current cue in the upper box and the
   next cue in a second, separately styled box below (FO.5b), both frozen to
   the profile's type. Alternative: one combined box; rejected because next
   cue must be independently routable.
2. **Per-slide stage overrides.** Recommended: none in v1. A stage look is a
   theme concern; the editor's Stage tile is the one place it changes.
3. **NDI source naming.** Recommended: `"<service name> — lower third"` with a
   per-machine source name override; service name only fills the default.
4. **Broadcast rate negotiation.** Recommended: 30/1 default, then 25/1 and
   60/1 if the receiver asks; the sender never switches mid-cue.
5. **Notes layer.** Recommended: off and unavailable in v1; speaker notes live
   in slide notes today, and reinterpreting slide notes (which carry section
   labels) as speaker notes is explicitly rejected.
