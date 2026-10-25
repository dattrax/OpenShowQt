/*
 * The video presentation seam (task 1.5e), lifted out of OutputSession by
 * task 5.1 so the production playback pipeline can implement and feed it
 * without this header's caller pulling in the session's window machinery.
 * The session decides *when* a submitted slot is drawn (against the video
 * clock); the presenter owns *what* the token is and its GPU lifetime.
 */

#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "include/core/SkImage.h"

namespace openshow::worker {

// A decoded frame offered to the session: presenter-side slot identity,
// the pts in video-clock seconds, and the decoder's monotone sequence.
struct VideoSubmission {
    unsigned token = 0;       // presenter-side slot identity
    double ptsSeconds = 0.0;  // in video-clock seconds
    uint64_t sequence = 0;
};

class VideoPresenter {
public:
    virtual ~VideoPresenter() = default;
    // Render thread: the image to sample for this token. Wrapping
    // belongs here — every Skia call stays on the render thread.
    // Returning null drops the submission.
    virtual SkImage* acquireImage(unsigned token) = 0;
    // Render thread, once the frame has been submitted and any preview
    // readback completed: doneVk (freshly created here, ownership now
    // the presenter's) signals when the GPU finished sampling.
    virtual void onPresented(unsigned token, VkSemaphore doneVk) = 0;
    // The submission was replaced before ever being drawn. This runs on
    // the render thread, or on the thread that called submitVideo when
    // it replaced an undrawn pending submission — implementations
    // carrying counters around it need a lock.
    virtual void onDropped(unsigned token) = 0;
};

}  // namespace openshow::worker
