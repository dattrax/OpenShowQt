# OpenShowQt progress and session handoff

Updated: 2026-10-21 (Europe/London).

## Current checkpoint

- Milestone: **M0 — Planning, references and build foundation**.
- Completed task: **0.1 — Write plan and progress files**.
- Next task: **0.2 — Establish Git metadata and ignore rules**.
- State: stopped at the user's request after writing the documentation. Await
  the user's GitHub repository setup and Skia/Vulkan build locations before
  continuing. No application milestone has passed.
- Next milestone commit: `M0: establish project plan and build foundation`.
- Commits/tags made by the agent: none. The user controls commits.

## Completed work

- Inspected the empty destination and source clone.
- Verified FreeShow version 1.6.7 and reference commit
  `436d4b634878ba723da1d1373637384ec700ce16`.
- Authored `PLAN.md` with architecture, 32 numbered tasks, milestone commit
  checkpoints, acceptance checks and resume rules.
- Authored this handoff file and `.gitignore` for generated files and personal
  media. The original clone is unchanged. No application code was created.

## Verification and environment

- `git -C /home/jim/FreeShow rev-parse HEAD`: matches the reference above.
- `git status --short` in this workspace: fails; not a Git repository.
- `findmnt -T .git`: `.git` is an empty, read-only tmpfs mount. Do not delete or
  replace this protected mount or create an undocumented alternate repository.
- Fedora 46 KDE prerelease. CMake, Ninja, GCC, Qt runtime tooling, Vulkan headers
  and FFmpeg executable found.
- Earlier pkg-config checks did not find Qt6Core, Qt6Quick, SDL3 or libavcodec
  development packages. A reproducible full build is not verified.
- Upstream has no installed node_modules; runtime UI captures are not available
  yet. Source-based references can be documented independently.
- Task 0.1 acceptance: both Markdown files were successfully written; `PLAN.md`
  contains the accepted architecture, all M0–M6 tasks and acceptance criteria;
  this file records current status, blockers and exact resume actions.

## Decisions

- Follow the accepted architecture in `PLAN.md`; specifically the worker must
  remain Qt-free, and existing FreeShow document import is deferred.
- Save/resume progress in these tracked Markdown files rather than chat memory.
- Do not mark authored scaffolding as a verified graphics implementation.
- User instruction: stop after writing plan/progress. The user will set up the
  GitHub repository and provide Skia/Vulkan builds. Do not initialize Git,
  install dependencies or begin application implementation in this session.

## Blockers and remaining work

- Task 0.2 needs working writable Git metadata.
- Task 0.4 needs Qt6/SDL3/FFmpeg development dependencies and a Skia build.
- Runtime screenshots and graphics/media acceptance need a display-capable session.

## Exact next action

1. Wait for the user to provide the GitHub repository setup and Skia/Vulkan build
   locations. No further implementation is authorized until work is resumed.
2. When the user resumes, inspect Git status/history and the supplied builds.
3. Continue task 0.2, then M0 reference/build preparation using those builds.

## Resume checklist

1. Read `PLAN.md` and this file.
2. Inspect Git status/history (or the recorded Git blocker) and existing changes.
3. Continue the current task; preserve unrelated work and the source clone.
4. Before stopping, update this file with commands/results and the next action.
