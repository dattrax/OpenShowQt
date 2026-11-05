# Theme editor operator follow-ups — 2026-11-05

The operator's five reports are tracked as TE.1–TE.5 in PLAN.md. Production
repairs are committed separately, and the in-tree frontend is rebuilt after
each repair so restarting it loads the changes.

| Item | Result | Commit |
|---|---|---|
| TE.1 | Handles use the parent rectangle's local corners; its nonzero origin is applied once | `6b2b704` |
| TE.2 | Songs/Bible thumbnail buttons stack directly beneath the bounded theme-name list; actions stay at the bottom | `77ad3f5` |
| TE.3 | Removed navigation, sample, context and transition instruction paragraphs; retained controls, provenance and actionable status | `bff16f1` |
| TE.4 | Bible family, size, alignment and spacing use resolved values and the same font browser/status as Songs; reset chips restore inheritance; both views have solid blue selection chrome | `7f064b1` |
| TE.5 | Both view samples use the same four Lorem ipsum lines, with no theme-name heading, pangram or invented scripture; normal rendering handles wrap/fit | This change |

TE.4 unifies the supported font controls. Advanced per-context typography and
effects remain the separately planned TV.3 model work; it is not certified or
staged by these fixes.

Geometry regressions check all four handle centres before/after drag and typed
resize. Navigation tests assert vertical tile order and click both views at
1000×680, 1380×800 and 1680×1020. Font tests verify resolved Bible fields, the
Browse control, an unchanged inherited size producing no override, and normal
context edit/undo/refusal behaviour. Sample tests compile both views and check
their identical four-line content. View thumbnail batches are checked against
the compiled sample for each view; identical appearances may now correctly
produce identical scenes.

The populated `OPENSHOW_CONTROL_CAPTURE` tour now calls the production
`selectView` action instead of assigning `selectedView` directly, waits beyond
the page's 120-ms debounce and requires a new worker Selection frame after a
view change. This corrects a limitation in the earlier CR capture evidence:
a direct property assignment bypassed the action's synchronization and ask.
The tour also checks actual handle corner centres at every size/scale and
verifies that Bible has an enabled worker font browser and its own size.

All three scale runs (1, 1.5, 2) returned zero: 21 frames each, 63 total,
including 18 Themes frames across both views. JSON manifests verify scaled
pixel dimensions and positive renderer counters. Representative Songs and
Bible images were visually inspected; this is not manual pixel-by-pixel
inspection of all 63 frames. Local outputs are under
`/tmp/openshow-theme-followups/{s1,s15,s2}` and are reproducible using the
[capture commands](CS-remediation-acceptance-2026-11-05.md#reproducible-populated-captures).
No screenshots/build artifacts are committed.

The final affected regression gate passed **5/5 tests** in 48.54 seconds:
workspace UI, shared controls, UI load, document controller and inspector adapter.
It uses the
clean `/tmp/openshow-cs-remediation` snapshot with these changes and excludes
the pre-existing partial TV.3 edits. The actual workspace frontend also builds.
Its earlier workspace test run had one unrelated assertion failure: TV.3 adds
context colour metadata where the old test expects no colour member. The new
handle checks passed there; this repair does not claim the TV.3 suite is clean.
Only the neutral-sample hunk of DocumentController is committed here; its
existing TV.3 additions and the three domain-file diffs are preserved.
