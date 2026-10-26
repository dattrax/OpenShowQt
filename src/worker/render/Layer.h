/*
 * Output layer model (PLAN.md "Output layer model").
 *
 * The audience output composites each frame from ordered layers, bottom to
 * top: background, images, words, aux and clock, mirroring FreeShow's output
 * tabs. Audio is deliberately absent: it is a playback source owned by the
 * AV subsystem, never a composition item dispatched to Skia.
 *
 * Compositing is one pass with no render-to-texture: every enabled layer
 * draws straight into the target canvas (the current swapchain surface) in
 * z order, so a low-end PC pays for one render target only. A disabled layer
 * is functionally skipped: its draw() is never called, recording no Skia
 * work at all. Layers are toggled from the frontend over RPC (task 1.2+)
 * and take effect on the next frame without recreating GPU resources.
 * The one exception (task 3.2) is the fade transition's words dissolve,
 * which wraps the block in a group-alpha saveLayer — Skia's own way of
 * putting the transparency in the paint; the layer is word-bounds sized and
 * lives only while a fade is in flight.
 */

#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string_view>
#include <vector>

#include "openshow/scene/scene.h"

class SkCanvas;
class SkImage;

namespace openshow::worker {

// The presentation currently on air, shared with whatever draws it. Null
// means nothing has been presented: production draws nothing (the black
// clear stands), a decorated development renderer shows its demo.
using ActiveScene = std::shared_ptr<const openshow::scene::Scene>;

// One composed frame, captured once and drawn read-only to every target
// (task 6.11a, review R13).
//
// The worker draws the same moment to two targets — the audience panel and
// the operator's preview — and both must show the SAME scene at the SAME
// blend under the SAME revision tag. Reading the renderer's live state at
// draw time cannot promise that: a preview tick that asked for the blend
// "as of now" advanced the live crossfade with it, so a preview could run
// a fade to its end on a clock the audience never saw, and leave the
// audience stranded on whatever blend it had last been shown. So the frame
// loop captures this snapshot once, per audience frame, and every target
// draws these values; nothing a target does during its draw can change
// another target's picture or the next frame's.
struct SceneFrame {
    ActiveScene scene;           // the composition on air (null: nothing shown)
    ActiveScene fadeFrom;        // the outgoing scene, null when not fading
    float mediaBlend = 1.0f;     // 0 = all fadeFrom, 1 = all scene
    float textBlend = 1.0f;
    double timeSeconds = 0.0;    // steady clock since output started
    std::time_t wallClock = 0;   // fixed by selftests for deterministic text
    std::uint64_t revision = 0;  // the command revision these pixels carry
};

enum class LayerId : uint8_t {
    kBackground = 0,  // solid colour, gradient, image or video behind everything
    kImages,          // slide images
    kWords,           // lyrics and text boxes
    kAux,             // auxiliary overlays (placeholder; off by default)
    kClock,           // audience clock and diagnostics footer
    kCount,
};

constexpr uint32_t layerBit(LayerId id) { return 1u << static_cast<uint32_t>(id); }

// Stable names used by the development CLI today and by the RPC layer toggle
// later: "background", "images", "words", "aux", "clock".
std::string_view layerName(LayerId id);
bool layerIdFromName(std::string_view name, LayerId* out);

// Everything a layer may need about the frame being composed. The canvas is
// already transformed by the output rule (authored 1920x1080 scaled and
// centred), so layers draw in authored coordinates.
struct FrameContext {
    SkCanvas* canvas = nullptr;
    // The frame to draw (task 6.11a, review R13): scene, fade blends, clock
    // and revision in one captured snapshot. Layers read their content here
    // and nowhere else, which is what makes drawing to a target a read-only
    // operation — the scene-aware layers used to point at the renderer's
    // live state, so the second target of a frame saw whatever the first
    // target's drawing had left behind. Never null in a real dispatch.
    const SceneFrame* frame = nullptr;
    const char* statusLine = "";
    // The video frame the session's scheduler decided to draw this frame
    // (task 5.5): null when no video is on offer. The background layer
    // consumes it when the on-air scene's background names a video asset;
    // whoever offered the frame decides everything else about it (timing,
    // lifetime), the layers only sample it.
    const SkImage* backgroundVideo = nullptr;
};

class RenderLayer {
public:
    virtual ~RenderLayer() = default;
    virtual LayerId id() const = 0;
    // Draws into ctx.canvas. Implementations must not create offscreen
    // surfaces; transparency goes straight into the paint.
    virtual void draw(const FrameContext& ctx) = 0;
};

// Ordered collection with per-layer enable bits. Layers are added bottom to
// top; drawing walks that order and skips disabled layers entirely.
class LayerStack {
public:
    // Adds in z order (first added is bottom-most) and starts enabled.
    void add(std::unique_ptr<RenderLayer> layer);

    void setEnabled(LayerId id, bool enabled);
    bool isEnabled(LayerId id) const;

    // Development/RPC-facing name toggle; returns false for unknown names.
    bool applyOverride(std::string_view name, bool enabled);

    void draw(const FrameContext& ctx) const;

private:
    std::vector<std::unique_ptr<RenderLayer>> fLayers;  // bottom -> top
    uint32_t fEnabledMask = 0;
};

}  // namespace openshow::worker
