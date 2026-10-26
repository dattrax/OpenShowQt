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

// A decoded frame offered to the session: presenter-side slot identity, the
// decoder's monotone sequence, and the frame's TWO times (task 6.7d, review
// R19) — they are not the same quantity and were treated as one:
//
//   * ptsSeconds is the MEDIA position: loop-continuous pts, the number a
//     lyric service reads and status reports. It restarts when the operator
//     seeks, and it climbs past the clip length when the clip loops.
//   * deadlineSeconds is the PRESENT TIME in the transport clock's own
//     domain: when the pacer decided this frame is due. It is what a clock
//     reading is compared against, and after a pause, a seek or a loop wrap
//     the two domains differ — by a whole clip length in the case R19 found
//     (a backward seek after a wrap), where comparing pts against the clock
//     held the audience's picture for seconds while the transport played on.
struct VideoSubmission {
    unsigned token = 0;             // presenter-side slot identity
    double ptsSeconds = 0.0;        // media position, in media seconds
    double deadlineSeconds = 0.0;   // present time, in transport-clock seconds
    uint64_t sequence = 0;
};

// The session's present gate for one submission, in one place so pacing and
// the session cannot drift into using different deadlines again (task 6.7d):
// a frame is due when the transport clock reaches the deadline the pacer
// computed for it. Never its media pts.
inline bool videoSubmissionDue(const VideoSubmission& submission, double clockSeconds) {
    return clockSeconds >= submission.deadlineSeconds;
}

// The token's GPU lifetime is a LEASE (task 6.6a, review R6): a
// submission leases its slot from the moment it is submitted until the
// session stops sampling it. While leased, the presenter never hands the
// slot back to the producer — the held frame survives every dirty redraw
// and preview publish of the same image. Two obligations are separate:
//
//   * onPresented() registers one GPU-completion obligation (doneVk) per
//     sampling flush. It does NOT end the lease; the redraw that produced
//     the semaphore may be the frame after the token first went up.
//   * onReplaced()/onDropped() end the lease, and they must prove which
//     generation they own: the sequence they name has to match the lease,
//     or nothing is released (the task-6.5 release-proof rule, applied
//     to GPU slots).
//
// acquireImage() validates the same way: an image is returned only while
// the token's current lease carries exactly the sequence the submission
// was made with, so a stale held submission can never sample a
// later-generation image of the same slot.
class VideoPresenter {
public:
    virtual ~VideoPresenter() = default;
    // Render thread: the image to sample for this token, drawn under the
    // graphics-queue lock. Wrapping belongs here — every Skia call stays
    // on the render thread. Returning null (bad token, failed wrap, or a
    // generation that no longer leases the slot) drops the submission;
    // the caller then relinquishes it with onReplaced().
    virtual SkImage* acquireImage(unsigned token, uint64_t sequence) = 0;
    // Render thread, once the frame has been submitted and any preview
    // readback completed: doneVk (freshly created here, ownership now
    // the presenter's) signals when the GPU finished sampling THIS draw.
    // The slot stays leased — a held frame is redrawn, and each redraw
    // carries its own doneVk.
    virtual void onPresented(unsigned token, VkSemaphore doneVk) = 0;
    // Render thread: the displayed frame is replaced or forgotten; the
    // session will not sample this token's image again. Releases the
    // lease only when sequence names the generation that is leased.
    virtual void onReplaced(unsigned token, uint64_t sequence) = 0;
    // The submission was replaced before ever being drawn — leased, but
    // nothing sampled it, so it goes back with no GPU obligation. Same
    // release proof as onReplaced. This runs on the render thread, or on
    // the thread that called submitVideo when it replaced an undrawn
    // pending submission — implementations carrying counters around it
    // need a lock.
    virtual void onDropped(unsigned token, uint64_t sequence) = 0;
};

}  // namespace openshow::worker
