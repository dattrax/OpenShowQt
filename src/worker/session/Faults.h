/*
 * Test-only fault-injection knobs (task 6.4, review R4), shared by the
 * session's frame loop and the preview producer (task 6.18a2) because the
 * fixtures must force two windows rather than race for a millisecond — a
 * command landing while the render thread is mid scene-apply (transport
 * opens really do take hundreds of ms, so this models reality), present
 * failures that must not be acknowledged, and a preview target whose cost
 * is real milliseconds paid inside the measured region of the producer's
 * fill, so the audience-budget policy is tested against a cost rather than
 * a microsecond (task 6.11a, review R13).
 *
 * Failure arming belongs to a real scene change so a worker's startup
 * frames are never the ones eaten: OPENSHOW_FAULT_SCENE_PRESENT_FAILS
 * re-arms on every applied scene switch and the next that-many presents
 * fail. Absent the environment every knob is off and production pays
 * nothing but a few never-taken branches per apply / present / preview
 * fill.
 *
 * The instance is render-thread only (mutated by frame()/
 * applyPendingSceneChange alone); the counters it holds that change at run
 * time (armedPresents) are touched by that thread only.
 */

#pragma once

#include <cstdlib>

namespace openshow::worker {

struct Faults {
    int applyDelayMs = 0;         // OPENSHOW_FAULT_SCENE_APPLY_MS
    int scenePresentFails = 0;    // OPENSHOW_FAULT_SCENE_PRESENT_FAILS
    int armedPresents = 0;        // runtime: scene-change failures left to burn
    int holdMs = 0;               // OPENSHOW_FAULT_HOLD_MS: sleep after a failure
    // Task 6.11a (review R13): the cost of an expensive preview target,
    // paid inside the measured region of the preview fill.
    int previewDelayMs = 0;       // OPENSHOW_FAULT_PREVIEW_MS
    Faults() {
        const auto read = [](const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr ? std::atoi(value) : 0;
        };
        applyDelayMs = read("OPENSHOW_FAULT_SCENE_APPLY_MS");
        scenePresentFails = read("OPENSHOW_FAULT_SCENE_PRESENT_FAILS");
        holdMs = read("OPENSHOW_FAULT_HOLD_MS");
        previewDelayMs = read("OPENSHOW_FAULT_PREVIEW_MS");
    }
};

inline Faults& faults() {
    static Faults instance;
    return instance;
}

}  // namespace openshow::worker
