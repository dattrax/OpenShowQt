/*
 * TestScene: the development harness for the production compositor
 * (SceneRenderer), the descendant of task 1.1's picture.
 *
 * Task 6.1b (review R1: "production output still uses the development
 * scene") finished the split this file's old class never got: the layers,
 * the transition state machine and the output-rule composition now live in
 * render/SceneRenderer.{h,cpp}, which a --socket session composes from.
 * Nothing here is production code. What remains is the development side of
 * that compositor:
 *
 *   - demoBuilder(): the 1.x picture as a SceneRenderer::DemoDecoration —
 *     the backdrop gradient, a procedural photo encoded to PNG and decoded
 *     through SkCodec (proving the real image path), and the SkParagraph
 *     title shaped through harfbuzz with the fontconfig manager. Attaching
 *     it is exactly what the old kDemo profile was: the decoration draws
 *     only while nothing is on air, and brings the clock layer up as the
 *     live-frame indicator.
 *   - renderTestSnapshot(): the --snapshot picture on the CPU raster.
 *   - verifyLayerComposition / verifySceneTransition / verifyProduction
 *     Compositor(): the pixel assertion harnesses behind --check-layers,
 *     --check-transition and --check-production. They drive a real
 *     SceneRenderer — dev-decorated where the 1.x picture is the subject,
 *     bare where the audience composition is — so the checks always test
 *     what the window actually composes.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/SkImage.h"

#include "render/Layer.h"
#include "render/SceneRenderer.h"

namespace openshow::worker {

// Builds the 1.x development picture as a decoration for SceneRenderer:
// the gradient backdrop, the raster -> PNG -> SkCodec demo photo, and the
// SkParagraph demo title (its paint closure owns the laid-out paragraph
// and draws it through the words tab). Pass it to create() to get the old
// TestScene picture; the production session passes nothing.
SceneRenderer::DemoBuilder demoBuilder();

// Renders the decorated development scene on the CPU and writes a PNG. No
// display, Vulkan or windowing needed; used by the test suite and for
// offline review.
bool renderTestSnapshot(const std::string& path, int width, int height,
                        const std::vector<std::pair<LayerId, bool>>& layerOverrides,
                        std::string* error);

// Composites frames on the CPU with different layer toggles and checks the
// model: everything off is pure black, each layer's pixels appear only in
// its region, and toggles round-trip to identical frames. Reports the first
// failed expectation in error. Backs --check-layers.
bool verifyLayerComposition(std::string* error);

// Drives the scene transition state machine (task 3.2) on the CPU: pops
// in the first presentation, times both crossfade tracks (media 800ms,
// text 500ms) from drawn frames, completes them cleanly, and proves a cut
// swaps in one frame — including cancelling a fade in flight. Pixel
// assertions through the same layers the audience window uses.
bool verifySceneTransition(std::string* error);

// Task 6.1a (review R1): the production composition's rules on the CPU,
// against a bare SceneRenderer. It starts with clock and aux off and draws
// a uniformly black frame with nothing presented — every pixel black, the
// footer/clock region included; a presented scene composes pixel-identical
// production and development (clock-off) frames, so the audience scene is
// the proven layer path, not a demo-flavoured one; and the demo picture
// still paints in full when a decoration is attached. Backs
// --check-production.
bool verifyProductionCompositor(std::string* error);

}  // namespace openshow::worker
