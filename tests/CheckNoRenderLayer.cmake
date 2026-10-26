# Fails when the production composition pass reaches for an intermediate
# render target (task 6.11b, review R13).
#
# The output rule, stated in src/worker/render/Layer.h, is one pass with no
# render-to-texture: every enabled layer draws straight into the target
# canvas, so a low-end PC pays for one render target. A FADE is where that
# rule gets tested, because a fade wants a group alpha — the blend applies to
# what the layers composited, not to each draw inside them — and the reflex
# answer is `saveLayer()`, which allocates a second target for every frame of
# every transition. Both fades have a target-free form now (the background
# dissolves through one draw whose shader is `picture over fill`; the words
# through a ParagraphPainter that scales the alpha on each glyph draw), so
# the reflex is no longer an excuse, and this scan is what stops it coming
# back the next time a composite looks awkward.
#
# Two scans, because the two ways to break the rule are different:
#   1. saveLayer/saveLayerAlpha anywhere under SRC_DIRS, and
#   2. a render target created inside the layer stack (LAYER_DIRS).
# Comments are not code — inline comments are stripped and a line left with
# nothing but comment text is skipped — so the rule can be EXPLAINED in the
# sources it guards without the guard objecting. A call merely standing behind
# a comment on the same line is still a call, and still flagged.
#
# Usage:
#   cmake -DROOT=/path/to/repo -DSRC_DIRS="src" \
#         -DLAYER_DIRS="src/worker/render" -P CheckNoRenderLayer.cmake

if(NOT ROOT OR NOT SRC_DIRS)
  message(FATAL_ERROR
    "CheckNoRenderLayer.cmake needs -DROOT=<repo> -DSRC_DIRS=<dirs, relative to ROOT>"
    " [-DLAYER_DIRS=<dirs>]")
endif()

# Nothing may join either list without a PLAN.md entry and a reason.
#
# EXEMPT_GROUP_ALPHA: files allowed to call saveLayer. EMPTY — production has
# no group-alpha layer anywhere, which is the whole point of the task.
set(EXEMPT_GROUP_ALPHA)

# EXEMPT_TARGET_FILES: files under LAYER_DIRS allowed to create a surface.
# These two are not the live composition pass: renderSceneSnapshot() renders
# one frame to a raster canvas for the CLI selftest and the pixel fixtures
# (that is how a fixture sees a fade at all), and TestScene.cpp is the
# development harness's own window. Neither runs on the audience's frame loop.
set(EXEMPT_TARGET_FILES
  "src/worker/render/SceneText.cpp"
  "src/worker/render/TestScene.cpp")

string(REGEX REPLACE "[ \t\"]" "" srcDirs "${SRC_DIRS}")
if(LAYER_DIRS)
  string(REGEX REPLACE "[ \t\"]" "" layerDirs "${LAYER_DIRS}")
else()
  set(layerDirs)
endif()

set(violations "")

# Scans one directory (relative to ROOT) for a forbidden call pattern,
# recording hits in the caller's `violations`. Comments are not code, but a
# comment must not be a HIDING PLACE either: inline comments are stripped
# first, so `/* never */ canvas->saveLayer(...)` is still a violation, and only
# a line left with nothing but comment text (including a line inside a block
# comment, which still begins with * or /*) is skipped. Lines in
# tests/fixtures/ are this guard's own negative test data.
function(scan_dir dir pattern exemption_list label)
  file(GLOB_RECURSE found RELATIVE "${ROOT}"
    "${ROOT}/${dir}/*.cpp" "${ROOT}/${dir}/*.h" "${ROOT}/${dir}/*.hpp"
    "${ROOT}/${dir}/*.cc")
  foreach(source ${found})
    if(source IN_LIST ${exemption_list})
      continue()
    endif()
    if(source MATCHES "/fixtures/")
      continue()
    endif()
    file(STRINGS "${ROOT}/${source}" lines)
    set(lineno 0)
    foreach(line ${lines})
      math(EXPR lineno "${lineno} + 1")
      set(code "${line}")
      string(REGEX REPLACE "/\\*.*\\*/" "" code "${code}")
      string(REGEX REPLACE "//.*" "" code "${code}")
      if(code MATCHES "^[ \t]*$" OR code MATCHES "^[ \t]*(\\*|/\\*)")
        continue()
      endif()
      if(code MATCHES ${pattern})
        string(APPEND violations "\n  [${label}] ${source}:${lineno}: ${line}")
      endif()
    endforeach()
  endforeach()
  set(violations "${violations}" PARENT_SCOPE)
endfunction()

# 1. saveLayer is not the way to fade anything, anywhere in production.
foreach(dir ${srcDirs})
  scan_dir("${dir}" "(->|\\.)saveLayer(Alpha)?[ \t]*\\(" EXEMPT_GROUP_ALPHA
           "group-alpha layer")
endforeach()

# 2. The layer stack creates no render target of any kind.
foreach(dir ${layerDirs})
  scan_dir("${dir}" "(->|\\.)makeSurface[ \t]*\\(|SkSurfaces::|SkSurface::Make"
           EXEMPT_TARGET_FILES "render target")
endforeach()

if(violations)
  message(FATAL_ERROR
    "the composition pass must not create an intermediate render target "
    "(one pass, no render-to-texture — see src/worker/render/Layer.h and "
    "PLAN.md task 6.11b, review R13). A fade's group alpha belongs in the "
    "paint or the shader, not in a layer:"
    "${violations}")
endif()
message(STATUS "no intermediate render targets in the composition pass")
