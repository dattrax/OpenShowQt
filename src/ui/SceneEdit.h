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
#include <QStringList>
#include <QVariantMap>

namespace openshow::ui {

struct SceneContent {
    bool words = false;
    bool background = false;
    // The acknowledged scene carries the show's audio layer (DR.5). Audio is
    // its own layer: it never makes `background` true, and a cue that is only
    // audible is still output activity.
    bool audio = false;
};

// Content flags of a scene JSON, read through the SAME typed contract the
// worker validates with (review D1), so the indicator and the audience cannot
// disagree: a camera background reports a background because the codec
// reinstates its picture, and a payload the codec refuses reports nothing.
// The background counts when it carries an image, a non-black colour, or a
// gradient (6.21e) — a black fill with nothing second to it is what "no
// background" looks like, and transparency suppresses it.
SceneContent sceneContent(const QString& sceneJson);

// The scene with its words removed (background untouched) or its background
// removed (words untouched); "" when the input is not a scene this build can
// present. The background clear is a complete transformation through the typed
// contract (review D1): picture, video, camera and gradient all go, so the
// result validates and the camera is really retired. Everything else —
// provenance, style, the words, the show's audio layer — carries over.
QString withRetainedBackground(const QString& sceneJson, const QString& previousJson);
QString withWordsCleared(const QString& sceneJson);
QString withBackgroundCleared(const QString& sceneJson);

// Every asset reference a scene payload carries, in wire order: the
// background's `assetPath`, the show's audio layer's, and each image item's.
// ONE traversal, shared by the read and the rewrite below (DR.2), because a
// reference this list misses is a stale path the running session keeps after
// a save — which is exactly the bug D2 named. An unreadable payload answers
// nothing.
QStringList sceneAssetPaths(const QString& sceneJson);

// The same payload with every asset reference whose OLD path is a key in
// `pathMap` rewritten to the new one. Returns the input UNCHANGED when no
// reference moved (so a caller can tell "nothing to do" from "rewritten"),
// and "" when the input is not a scene object. A save's adoption feeds every
// retained payload through this: the revision mirror, pending prepares, the
// Restore snapshot and the desired presentation.
QString withAdoptedAssetPaths(const QString& sceneJson, const QVariantMap& pathMap);

// scene.clear-all in the same shape: the scene library's cleared scene,
// serialized (black, no items).
QString clearedSceneJson();

}  // namespace openshow::ui
