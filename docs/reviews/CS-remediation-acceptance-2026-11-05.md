# Shared controls remediation acceptance — 2026-11-05

This closes the CR follow-ups to the [conformance assessment](CS-conformance-2026-11-05.md).
Each production repair was committed and pushed separately. Historical CS.9
screenshots remain historical; this pass uses populated production QML with
actual renderer output.

## Repair commits

| Item | Repair | Commit |
|---|---|---|
| CR.1 | Correct preview picture aspect and caption containment | `a7bea16` |
| CR.2 | Give populated verse delegates their viewport width | `be26411` |
| CR.3 | Bound minimum font size controls to the inspector rail | `057344e` |
| CR.4 | Read resolved theme appearance, retaining own/inherited distinctions | `d046092` |
| CR.5 | Wrap the Bible lookup form in narrow panes | `8192c37` |
| CR.6 | Map collection rows from actual cell geometry | `0ac0ae4` |
| CR.7 | Deliver complete thumbnail windows in bounded wire batches | `7f6f4a5` |
| CR.8 | Release Bible preview ownership while hidden | `756dc50` |
| CR.10 | Reserve header space for navigation and actions | `4ed94e4` |
| CR.11 | Keep verse indicators beside wrapped text | `c7fa22d` |
| CR.12 | Disconnect socket callbacks before transport member destruction | `34839be` |
| CR.13 | Keep parsed class-token views on owned attribute storage | `f07076b` |

CR.11–CR.13 were discovered during populated acceptance, repaired separately,
and included in the final reruns. The connected-client destruction test and
long translation-token tests pin the two lifetime errors. The parser cases
fail on the old implementation and pass on the repair; parser Memcheck reports
zero errors and zero retained blocks.

## Reproducible populated captures

`control_set_ui_tests` normally runs focused geometry and interaction checks.
Setting `OPENSHOW_CONTROL_CAPTURE` instead loads the production Main module,
starts the real local audience worker and captures a synthetic service: a
three-slide song followed by a six-slide Bible item. A scripted network reply
supplies six long verses without contacting a provider. The Warm theme has gold
text on navy with distinct Songs/Bible sizes and authored boxes.

Run from a configured desktop build (substitute its path below):

```sh
QT_SCALE_FACTOR=1 QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
OPENSHOW_CONTROL_CAPTURE=/tmp/openshow-cs-acceptance/s1/control \
./build/tests/control_set_ui_tests
```

Repeat with scales `1.5` and `2` and distinct output prefixes. One invocation
captures all three logical sizes, 1680×1020, 1380×800 and 1000×680. Five primary
views are Show, Bible Import, Edit, Themes Songs and Themes Bible. Two additional
views expose the scrolled Bible selected canvas/Add controls and the Edit
inspector when horizontal scrolling is needed. This yields **21 images per
scale, 63 total**, with a JSON manifest recording logical/pixel dimensions,
pagination and worker frame counters.

The tour checks that visible Show cards have varied renderer pixels, Bible
has current-plan first/last thumbnail frames and a current rendered Selection,
and Edit/Themes have rendered Selection pixels. Every saved frame asserts that
browsing has not put words on air or activated an audience scene. Unexpected
QML warnings fail the tour; the existing Qt StandardKey multiple-alias warning
has the same narrow exclusion as workspace tests. Each process exits cleanly.

All three invocations returned zero. All 63 manifest entries have the expected
scaled dimensions, six generated Bible slides and positive worker frame counts.
Representative Show, Bible form/selected canvas, Edit inspector and Themes
captures were visually inspected across the scale matrix. This verifies the
repaired spacing, caption containment, new-row grouping and themed content;
it is not a claim of manual pixel-by-pixel review of every image. Narrow hosts
still use their supported scrolling to expose content and controls. The chosen
DejaVu Serif family is not installed in this worker environment; the inspector
correctly reports its fallback status rather than silently rewriting the family.

Capture output is local under `/tmp/openshow-cs-acceptance/{s1,s15,s2}`;
images and generated build artifacts are not source-controlled. The checked-in
tour and manifests' format allow the evidence to be regenerated.

## Regression gates and scope

Verification uses a clean source snapshot at `/tmp/openshow-cs-remediation`,
with only these remediation changes applied. The four pre-existing partial
TV.3 files in the working tree were excluded, except CR.4's separate resolved
sample-colour metadata hunk in DocumentController. Their remaining edits were
neither staged nor certified by these results.

Both clean build configurations succeeded. The final desktop gate passed
**67/67 tests** in 422.66 seconds, including all 18 display-labelled tests
(246.36 seconds). No desktop tests skipped. The final headless gate passed
**36/36 enabled tests** in 67.74 seconds;
11 display-dependent registrations were excluded. Desktop runs use
`ctest --test-dir build --output-on-failure -j1`; headless configures
`OPENSHOW_BUILD_UI=OFF` and runs `ctest --test-dir build-headless -LE display
--output-on-failure -j1`. Display-dependent tests in the headless configuration
are excluded, not counted as passed.

The remaining TV.3, TV.4a/TV.4b, TV.5, TA.1 and physical M7 acceptance gates
remain open. These repairs do not authorize FO work or replace the operator's
forthcoming visual bug report.


## Operator follow-up evidence

The [TE acceptance record](TE-acceptance-2026-11-05.md) records the subsequent
handle, navigation, copy, font-presentation and sample repairs. It also corrects
the capture tour to switch views through the production action and require a
fresh frame; the earlier direct selectedView assignment was insufficient to
prove the Bible sample changed. Use that refreshed matrix for theme-view
comparison evidence.
