/*
 * SceneEdit: the clear controls' transforms on the scene wire form
 * (task 3.4).
 *
 * The reference's per-layer clears are independent (docs/reference.md §5:
 * background, slide, ... each on its own key, "independent per-layer
 * clearing made visible" by buttons that disable while their layer is
 * empty), and clearing is presentation, not a latch: the next go brings
 * the layer back. Since presenting is what this project's output does —
 * clearing was already "just an ordinary cleared scene" in task 3.1 — a
 * clear here compiles a *derived scene*: the scene on air with one part
 * removed. That keeps every promise of the model intact (revisions are
 * immutable, the worker acknowledges what is on air, restore is
 * presenting the remembered revision), and it is why the transforms work
 * on the JSON the frontend sent rather than on any worker state.
 *
 * What "cleared" means follows the reference's settled question §3:
 * cleared output is plain black with nothing left behind — words clear
 * to no items, background clears to the black fill with no image.
 *
 * The same functions answer the indicator question: `sceneContent` says
 * whether a scene's wire form has words and a background, which is what
 * enables a clear button and lights its live state — always derived from
 * the acknowledged revision, never from a hopeful click.
 */

#pragma once

#include <QString>

namespace openshow::ui {

struct SceneContent {
    bool words = false;
    bool background = false;
};

// Content flags of a scene JSON; {} for anything unparseable. The
// background counts when it carries an image or a non-black colour —
// a black fill is what "no background" looks like.
SceneContent sceneContent(const QString& sceneJson);

// The scene with its words removed (background untouched) or its
// background removed (words untouched); "" when the input is not a scene
// object. Everything else — provenance, style, the other part — carries
// over verbatim.
QString withWordsCleared(const QString& sceneJson);
QString withBackgroundCleared(const QString& sceneJson);

// scene.clear-all in the same shape: the scene library's cleared scene,
// serialized (black, no items).
QString clearedSceneJson();

}  // namespace openshow::ui
