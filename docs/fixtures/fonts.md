# Pinned font corpus fixtures (task TS.5)

TS.1–TS.4 shipped a typography contract, a renderer that draws it, styled
measurement, and controls that edit one field at a time. Their raster fixtures
measured whatever faces the machine happened to have installed, which answers
"does a stroke grow the ink" but cannot answer "is this the typography we say
it is". `font_fixture_tests` is the leaf that closes that gap: it renders the
families the theme-editor review pinned, at the revision and checksums it
recorded, inside a fontconfig configuration that can see nothing else.

- Corpus and policy: [tests/fixtures/fonts/README.md](../../tests/fixtures/fonts/README.md)
- Setup: `tools/setup-font-fixtures.sh prepare --download` (once, needs network)
  or `prepare --from DIR` (offline). `ctest` itself never downloads.
- Design authority:
  [the review's font artifacts](../reviews/theme-editor-2026-11-03.md#fonts-and-local-inspection-artifacts)

## The pin is asserted against its source

`manifest.json` is a copy of a decision made in the review, and a copy that
drifts is the quiet kind of wrong: the fixtures would still pass, over faces
nobody chose. So the test takes the review document as `argv[2]` and asserts the
agreement — the pinned revision matches, and every TTF row in the review's table
appears in the manifest with the same SHA-256, compared in full because a
one-character drift near the end is exactly what a prefix would hide. This runs
**before** any skip decision: a manifest that has drifted from its pin is wrong
whether or not this machine holds the bytes.

## How the isolation works

The test writes a private `fonts.conf` listing only the corpus directory, with
its own `cachedir`, and sets `FONTCONFIG_FILE` to it before the engine exists;
it also points `XDG_CACHE_HOME` at the scratch directory. There is deliberately
**no** `include` of `/etc/fonts`, so the machine's fonts cannot help a fixture
pass and cannot be touched by one. `fixtureCorpusIsolation` then asserts the
isolation before any pixel is measured: the engine's own `listFonts()` scan —
the same one TS.4 put on the wire as `font.list` — has to name exactly the four
pinned families, and `matchFamilyStyle("OpenShow No Such Family 47", …)` has to
return nothing. Every claim below is a claim about a named face, so this runs
first or nothing else means anything.

The run's PNGs land in a per-PID scratch directory; they are deleted when the
suite passes and kept, with the path in the failure text, when it does not.

## What is proven, and with what ruler

| Fixture | Promise | Ruler |
|---|---|---|
| corpus isolation | the private fontconfig file is honoured; nothing else resolves | the engine's own family scan and a direct `SkFontMgr` match |
| body / caption metrics | a size means what it says at both ends of the range | `layout()` height ratio within 5% of 64/28 on every family |
| family changes pixels | four files are four faces, not one face four times | per-family coverage counts differ pairwise, and >2000 pixels differ pairwise; the condensed face is >10% narrower than the sans |
| weight ramp | weight 100 → 900 resolves a FACE, and shaping does not move or re-wrap a word | >20% more ink at 900, and every row-band centre identical within 1 device px |
| extreme effects | the widest legal stroke (32) and a sigma-64 shadow offset up-and-left | left/top/right/bottom grown by half the pen; the negative-offset copy reaching 20 px left and 15 px above the fill's own box |
| ink bounds | every pixel of a width-32 stroke under a sigma-64 northwest shadow lands inside TS.3's `ink` rect at the 0.5 snapshot scale | measured bbox against `ink` mapped through the device scale, 2 px slack; and `fillInk` **unmoved** by effects, because that box is what a `fits` verdict judges |
| fixed vs shrink | shrink fits the box, fixed keeps the size and lets the audience see what does not fit | ink height against the box's device height (40 px vs 344 px in a 130 px box) |
| missing family | a request the corpus cannot satisfy falls back to a face that is IN the corpus, and still draws words | the fallback's family name is one of the four; the ghost render is byte-identical to rendering that family directly and unlike the other three |
| Unicode coverage | each face's real coverage, claimed only where the face has the glyph, with the corpus's boundary stated | per-family glyph matrix below, plus "every script draws something and never crashes" |
| midpoint fade | fill, stroke and shadow each ride the fade, and no word moves | ink bbox compared at each frame's OWN peak (an absolute ruler reads a half-bright AA edge as a smaller word); per-track median ratio ≈ 0.5 |
| costs | no unbounded setup or shaping | cold scan vs cached scan, first shape vs steady per family |
| layout cache | occupancy never exceeds the four slots, and a steady draw shapes nothing | 12 builds then find-by-pointer: 4 resident, oldest gone, 50 draws are 50 hits on one retained layout |

Tolerances, not goldens. CPU Skia is deterministic on one machine, but
antialiasing and fontconfig ordering are not portable promises, so geometry
assertions carry explicit pixel slack and the only byte-equality assertions are
between two renders made in the same run on the same engine (`spelled 400` vs
`default weight`, and `missing family` vs `its fallback`).

## The record (this machine, 2026-11-04)

Printed by the suite itself, so a regression in setup cost is visible as a
number rather than as a green tick:

```
pinned corpus: google/fonts 2fe1e8c6b137fb13041a7cfca6fdcd8a910ea4dc, 4 families, engine up in 167.5 ms
  record: font scan 0.4 ms first ask, 0.00 ms cached
  record: Inter      first shape     0.9 ms, steady    0.00 ms
  record: Lora       first shape     0.4 ms, steady    0.01 ms
  record: Oswald     first shape     0.4 ms, steady    0.01 ms
  record: Noto Sans  first shape     0.5 ms, steady    0.01 ms
  record: Inter      body 160.0 px, caption 70.0 px, ratio 2.286
  coverage Inter:      latin=yes greek=yes cyrillic=yes hebrew=no cjk=no emoji=no
  coverage Lora:       latin=yes greek=no  cyrillic=yes hebrew=no cjk=no emoji=no
  coverage Oswald:     latin=yes greek=no  cyrillic=yes hebrew=no cjk=no emoji=no
  coverage Noto Sans:  latin=yes greek=yes cyrillic=yes hebrew=no cjk=no emoji=no
  record: fade fill   median ratio 0.581 over 4465 px
  record: fade stroke median ratio 0.492 over 4552 px
  record: fade shadow median ratio 0.502 over 2952 px
  record: 12 scene builds in 1.7 ms, 4 resident, fifth evicted
```

Three things worth reading in that: the whole-corpus font scan costs under a
millisecond and is paid once per worker lifetime (TS.4's cache); the fade
attenuates all three passes to about half, with the fill slightly above the
midpoint because group-alpha coverage compounds non-linearly — the same
non-linearity TS.2's parity fixture measured; and **no pinned face covers
Hebrew, CJK or emoji**. In isolation those code points arrive as the renderer's
placeholder box: visible, never a blank slide, never a crash. That is the
corpus's boundary, not a product promise — on a real machine the audience's
installed fonts cover more, and the fallback walk is what finds them.

The fallback face on this configuration is `Oswald`: with only the four pinned
files visible, fontconfig answers a bare character request with it, and the
missing-family fixture proves the audience's words then render exactly as they
would if the operator had typed `Oswald`. Which face that is depends on the
corpus, not on the code — the fixture asserts it is *in* the corpus, never a
specific one.

## Skips are reported as skips

Without a prepared corpus the suite exits 77, which ctest shows separately:

```
1/1 Test #47: font_fixture_tests ...............***Skipped
The following tests did not run:
 47 - font_fixture_tests (Skipped)
```

with the setup command in its own output. A corpus whose bytes disagree with
`manifest.json` is a failure, not a skip, and names the offending file and both
hashes. The difference between "you have not prepared the corpus" and "the
corpus you prepared is not the corpus we pinned" is the difference between an
honest skip and a measurement of a different font.
