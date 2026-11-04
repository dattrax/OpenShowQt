# Pinned font corpus (task TS.5)

`manifest.json` is the statement of what the TS.5 raster and performance
fixtures render through: three families the theme-editor review pinned from the
official [Google Fonts source repository](https://github.com/google/fonts)
(`Inter`, `Lora`, `Oswald`), plus `Noto Sans` as the fixed fallback face TS.5
was told to include. The revision, every file's size and every file's SHA-256
are pinned here, and each family's own `OFL.txt` is pinned alongside it —
the four copyright lines differ, so one licence text does not cover the set.

**The fonts are not in this repository.** They total about 3.3 MB of binary
under the SIL Open Font License 1.1, which permits redistribution with the
licence text — but committing them would make the repository carry product-
sized third-party binaries to serve a handful of fixtures, and the packaging
policy has not been opened for that. Instead the corpus lives in a developer
cache, prepared by hand, and `font_fixture_tests` re-verifies every byte before
it measures a pixel:

```sh
tools/setup-font-fixtures.sh prepare --download        # once, needs network
tools/setup-font-fixtures.sh prepare --from DIR        # offline, from anywhere
tools/setup-font-fixtures.sh status | verify           # what the cache holds
```

The cache resolves to `$OPENSHOW_FONT_FIXTURE_DIR`, else
`${XDG_CACHE_HOME:-$HOME/.cache}/openshow/font-fixtures`. That is the same
directory the script writes and the test reads; nothing is installed into the
system's fonts and no fontconfig configuration outside a private scratch file
is touched.

## What the fixtures are for

See [docs/fixtures/fonts.md](../../../docs/fixtures/fonts.md). The short version:
`ctest` never downloads, an unprepared corpus is a visible **skip** (ctest
reports it as `Skipped`, with the command to run in its output), and a corpus
whose bytes disagree with this manifest is a **failure** — measuring a
different face than the one pinned would make every pixel assertion downstream
a claim about a font nobody chose.

## Licence

| Family | Upstream path at the pinned revision | Licence |
|---|---|---|
| Inter | `ofl/inter/` | SIL Open Font License 1.1 (`Inter-OFL.txt`) |
| Lora | `ofl/lora/` | SIL Open Font License 1.1 (`Lora-OFL.txt`) |
| Oswald | `ofl/oswald/` | SIL Open Font License 1.1 (`Oswald-OFL.txt`) |
| Noto Sans | `ofl/notosans/` | SIL Open Font License 1.1 (`NotoSans-OFL.txt`) |

The corpus is test stimulus only. The product neither bundles nor installs it,
and no shipped behaviour depends on these families being present: a theme that
names a family the audience's machine does not have falls back, and
`docs/fixtures/fonts.md` records where it lands.
