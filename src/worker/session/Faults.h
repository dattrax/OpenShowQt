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
    // Task 6.9b (review R10): a prepare that really reads the disk for
    // milliseconds, so the prepare queue's bound and the RPC's honesty
    // against it are tested against a COST, not a race window. The sleep
    // checks the executor's cooperative flag, so a retired job stops
    // early exactly the way real cancellable file work does.
    int prepareDelayMs = 0;       // OPENSHOW_FAULT_PREPARE_MS
    // Task 6.9b (review R10): a video open that really waits. The scenario
    // names the clip (substring) and how long its transport's bring-up
    // sleeps first — the fixture must not depend on a codec being slow on
    // some machine and instant on another, and R10's sentence ("slow video
    // open cannot freeze active output") deserves to be tested against a
    // SLOW open, not against a guess.
    int videoOpenDelayMs = 0;     // OPENSHOW_FAULT_VIDEO_OPEN_MS
    std::string videoOpenMatch;   // OPENSHOW_FAULT_VIDEO_OPEN_MATCH (path substring)
    // Task 6.21d (review F5): a cold THUMBNAIL decode that really takes
    // milliseconds, so the fixture proves the RPC pump answers status,
    // release and shutdown WHILE the file is being read, not after a
    // decode that happens to be quick on this machine. Named by path
    // substring like the video-open knob: the slow file is the subject,
    // every other picture is the yardstick.
    int thumbnailDecodeDelayMs = 0;  // OPENSHOW_FAULT_THUMBNAIL_DECODE_MS
    std::string thumbnailDecodeMatch;  // OPENSHOW_FAULT_THUMBNAIL_DECODE_MATCH
    // Task 6.22e1 (review A5): a standalone duration probe that really
    // waits, so the fixture measures WHERE the probe is paid rather than
    // hoping containers are quick. Named by path substring like every other
    // knob: the slow file is the subject, everything else the yardstick.
    int standaloneProbeDelayMs = 0;  // OPENSHOW_FAULT_STANDALONE_PROBE_MS
    std::string standaloneProbeMatch;  // OPENSHOW_FAULT_STANDALONE_PROBE_MATCH
    Faults() {
        const auto read = [](const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr ? std::atoi(value) : 0;
        };
        applyDelayMs = read("OPENSHOW_FAULT_SCENE_APPLY_MS");
        scenePresentFails = read("OPENSHOW_FAULT_SCENE_PRESENT_FAILS");
        holdMs = read("OPENSHOW_FAULT_HOLD_MS");
        previewDelayMs = read("OPENSHOW_FAULT_PREVIEW_MS");
        prepareDelayMs = read("OPENSHOW_FAULT_PREPARE_MS");
        videoOpenDelayMs = read("OPENSHOW_FAULT_VIDEO_OPEN_MS");
        if (const char* match = std::getenv("OPENSHOW_FAULT_VIDEO_OPEN_MATCH")) {
            videoOpenMatch = match;
        }
        thumbnailDecodeDelayMs = read("OPENSHOW_FAULT_THUMBNAIL_DECODE_MS");
        if (const char* match = std::getenv("OPENSHOW_FAULT_THUMBNAIL_DECODE_MATCH")) {
            thumbnailDecodeMatch = match;
        }
        standaloneProbeDelayMs = read("OPENSHOW_FAULT_STANDALONE_PROBE_MS");
        if (const char* match = std::getenv("OPENSHOW_FAULT_STANDALONE_PROBE_MATCH")) {
            standaloneProbeMatch = match;
        }
    }

    // True when THIS video's bring-up owes the injected wait.
    bool delaysVideoOpen(const std::string& path) const {
        return videoOpenDelayMs > 0 && !videoOpenMatch.empty() &&
               path.find(videoOpenMatch) != std::string::npos;
    }

    // True when THIS thumbnail's decode owes the injected wait (task 6.21d).
    bool delaysThumbnailDecode(const std::string& path) const {
        return thumbnailDecodeDelayMs > 0 && !thumbnailDecodeMatch.empty() &&
               path.find(thumbnailDecodeMatch) != std::string::npos;
    }

    // True when THIS standalone track's duration probe owes the injected
    // wait (task 6.22e1).
    bool delaysStandaloneProbe(const std::string& path) const {
        return standaloneProbeDelayMs > 0 && !standaloneProbeMatch.empty() &&
               path.find(standaloneProbeMatch) != std::string::npos;
    }
};

inline Faults& faults() {
    static Faults instance;
    return instance;
}

}  // namespace openshow::worker
