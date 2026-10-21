# OpenShowQt progress and session handoff

Updated: 2026-10-21 (Europe/London).

## Current checkpoint

- Milestone: **M0 — Planning, references and build foundation**.
- Completed tasks: **0.1** plan and progress files, **0.2** Git metadata and
  ignore rules, **0.5** recorded dependency versions and licences.
- Next task: **0.4 — Establish CMake targets and dependencies**.
- State: the workspace is a working Git repository tracking
  `git@github.com:dattrax/OpenShowQt.git`. Documentation is committed and
  pushed. No application code exists and no application milestone has passed.
- Next milestone commit: `M0: establish project plan and build foundation`.
- Commits made by the agent: documentation only. The user controls tags and
  further commits.

## Completed work

- Renamed the documentation from the working title FreeShowQt to OpenShowQt.
  References to the upstream FreeShow application are untouched.
- Rewrote `README.md` as a short description of the frontend and worker split,
  the first release scope and the current state.
- Checked the Git setup: remote, reachability and ignore rules (task 0.2).
- Inspected `/home/jim/sloppy-ui` and replaced SDL3 in the plan with GLFW plus
  vk-bootstrap for the output window, and native PipeWire for audio.
- Recorded the dependency versions, licences and gaps in `PLAN.md` (task 0.5).

## Verification and environment

- `git remote -v` gives origin `git@github.com:dattrax/OpenShowQt.git`;
  `git ls-remote --heads origin` returns `refs/heads/main`; `git status --short`
  is clean apart from these documentation edits. `.gitignore` excludes build
  output, fetched dependencies, editor settings and personal media.
- FreeShow 1.6.7 reference clone `/home/jim/FreeShow` at
  `436d4b634878ba723da1d1373637384ec700ce16`, left unchanged.
- `rpm -q glfw-devel` gives `glfw-devel-3.4-8.fc45`; `pkg-config --modversion
  glfw3` gives `3.4.0`. Vulkan loader `1.4.357.0` and `/usr/include/vulkan` are
  present. `cmake`, `ninja` and `g++` are present; `glslc` is absent.
- `/home/jim/skia` is at `a94df1cdab` with `out/Release/libskia.a` and also
  `libskparagraph.a`, `libskshaper.a`, `libskunicode_core.a` and
  `libskunicode_icu.a`, so SkParagraph needs no Skia rebuild.
- Absent and blocking task 0.4: `qt6-qtbase-devel`, `qt6-qtdeclarative-devel`,
  `pipewire-devel`, `ffmpeg-devel`, `asio-devel`, and a shader compiler
  (`glslang-devel` or `shaderc-devel`). PipeWire runtime `1.6.9` is installed.
- `/home/jim/sloppy-ui` builds windowed against that Skia tree with GLFW; its
  `DIRECT_TO_DISPLAY` mode needs an input plugin that is not on this machine.
- Fedora 46 KDE with some fc45 packages. Display and media acceptance still
  require a desktop session, which headless checks cannot replace.

## Decisions

- The worker stays Qt-free and gains no SDL dependency. GLFW provides the
  output window and Vulkan surface, vk-bootstrap the instance and device,
  following the pattern proven in `/home/jim/sloppy-ui`.
- Audio is native PipeWire: one stream per worker, software mix of embedded
  video audio with one standalone track, per-source volume and mute, the
  process callback as clock.
- Text layout stays in Skia with SkParagraph.
- No-compositor output through the Vulkan display plane is deferred.
- Keep progress in these tracked Markdown files rather than chat memory, and
  never mark authored scaffolding as verified graphics work.

## Blockers and remaining work

- Task 0.3 needs FreeShow running in a desktop session to capture screens,
  controls and shortcuts.
- Task 0.4 needs the missing development packages; installing them requires the
  user's approval for `sudo dnf install`.
- The plan still names standalone Asio for the worker socket. A plain `poll()`
  loop on a Unix socket would drop that dependency; settle this before writing
  any RPC code.

## Exact next action

1. Ask, then install `qt6-qtbase-devel qt6-qtdeclarative-devel pipewire-devel
   ffmpeg-devel` and a shader compiler with `sudo dnf install`.
2. Create the CMake skeleton: Qt frontend, Qt-free worker, and a
   Qt-independent domain and protocol library, with the worker building against
   `/home/jim/skia/out/Release`, GLFW, Vulkan and libpipewire.
3. Record the configure and build commands that worked here, run the M0 checks
   and propose the checkpoint commit.

## Resume checklist

1. Read `PLAN.md` and this file.
2. Inspect Git status/history and any existing changes.
3. Continue the current task; preserve unrelated work and the source clone.
4. Before stopping, update this file with commands, results and the next action.
