/*
 * Task 6.8b (review R11): the shape of the queue an RPC server keeps for a
 * frontend that is not reading. The live-worker fixture proves the transport as
 * a whole drops a stalled peer in bounded time; this pins the rules that make
 * buffering survivable, which are easy to get wrong and hard to see through a
 * socket:
 *
 *   a reply is never dropped and never displaced by a notification, because a
 *   client that never learns the outcome of a command is worse off than one
 *   whose connection was closed;
 *   a notification with the same key as one still queued is REPLACED, so a
 *   chatty producer cannot fill the queue with state nobody asked for twice;
 *   the queue says NO at a fixed budget rather than growing;
 *   and a frame that has already started going out is never rewritten or
 *   abandoned, because the bytes already sent are in the peer's stream and
 *   anything that follows them must stay aligned.
 *
 * The coalescing rules are HERE rather than in the live-worker fixture on
 * purpose: through a real worker the notification stream is gated by the
 * render thread's own per-slot pending flag, so a socket-driven fixture cannot
 * queue enough of them to observe a replacement, and a rule that cannot be
 * observed must not be claimed to be tested.
 */

#include <cstddef>
#include <cstdio>
#include <string>

#include "openshow/protocol/protocol.h"
#include "rpc/OutboundQueue.h"

namespace {

// The bounds in ABSOLUTE units, deliberately not `kMaxQueuedBytes` and friends.
// A fixture that expects whatever budget the code under test declares passes
// when that budget is raised: this file's first mutation run lifted the byte
// budget to 512 MiB and the frame cap to a million and every check still held,
// because each was measured against the mutated constant. The numbers below are
// written out, and the one check that ties them to the code says so itself.
constexpr std::size_t kBudget = 1024u * 1024u;
constexpr std::size_t kFrameCap = 1024;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

using openshow::worker::OutboundQueue;

std::string frameOf(std::size_t bytes) { return std::string(bytes, 'x'); }

// Walks the queue the way the socket loop does: writes the head in `chunk`-byte
// pieces, and reports what the peer would have received in order.
std::string drainInChunks(OutboundQueue* queue, std::size_t chunk) {
    std::string received;
    while (!queue->empty()) {
        const std::size_t taking = std::min(chunk, queue->headRemaining());
        received.append(queue->headData(), taking);
        queue->didWrite(taking);
    }
    return received;
}

void testTheBoundsAreTheOnesTheCodeClaims() {
    // The queue's byte budget IS the transport's frame budget: the transport
    // refuses to send a single frame larger than that, so refusing to HOLD more
    // than it is the same rule seen from the other end, and the two numbers
    // cannot drift into "the queue buffers what the wire would never carry".
    check(openshow::worker::kMaxQueuedBytes == openshow::protocol::kMaxMessageBytes &&
              openshow::worker::kMaxQueuedBytes == kBudget,
          "the queue's byte budget is one transport frame, 1 MiB, and not a number next to it");
    // The entry cap is a backstop for many tiny frames, and it is only a backstop
    // if it is small: the coalescing scan and the room-making walk are both
    // linear in it, so a million entries is a million-entry scan per frame.
    check(openshow::worker::kMaxQueuedFrames == kFrameCap,
          "the queue caps its entry count at 1024, which is what keeps the per-frame scan cheap");
}

void testOrderingAndCoalescing() {
    OutboundQueue queue;
    check(queue.empty() && queue.bytes() == 0, "a fresh queue owes nothing");

    check(queue.push("", "A") && queue.push("note:1", "N1") && queue.push("", "B") &&
              queue.push("note:2", "N2"),
          "replies and notifications are all accepted while there is room");
    check(queue.frames() == 4 && queue.bytes() == 6,
          "the queue accounts for every frame and byte it holds");

    // The newest of each kind replaces the queued one, in place: the order of
    // what is left is the order the peer must see it in.
    check(queue.push("note:1", "N1-new") && queue.push("note:2", "N2-new"),
          "a notification for a key already queued is accepted");
    check(queue.frames() == 4,
          "replacing a queued notification does not add a frame — this is what keeps a busy "
          "preview from lengthening the queue");
    check(queue.coalesced == 2, "the replacement is counted, for the log that ends a peer");
    check(drainInChunks(&queue, 64) == "AN1-newBN2-new",
          "the peer receives each kind's newest value, in the original order, with the replies "
          "where they were queued");
}

void testAReplyIsNeverLostWhileNotificationsCan() {
    OutboundQueue queue;
    // Fill it with advisory frames: they are the ones that have to give way.
    // Eight of these are the entire budget, and few enough that the entry-count
    // bound is nowhere near being reached, so what binds here is bytes.
    constexpr std::size_t kNote = 128 * 1024;
    for (int i = 0; i < 8; ++i) {
        check(queue.push("note" + std::to_string(i), frameOf(kNote)),
              "a notification fits while the queue is empty of replies");
    }
    check(queue.bytes() == kBudget && queue.frames() == 8,
          "the queue fills to exactly 1 MiB of its budget with advisory frames");

    check(queue.push("", "the answer"),
          "a reply is still admitted at a full queue: the notifications give way");
    check(queue.droppedNotifications > 0 && queue.droppedNotifications <= 8,
          "and the queue says plainly how many notifications it gave up to make room");
    check(queue.frames() <= 8, "making room removed frames, it did not add up to more");

    // A second reply, and one that alone is the whole budget: the notifications
    // are already gone, so this one has nowhere to go. It must be REFUSED rather
    // than make room by evicting the reply already queued — a queue that trades
    // one unanswered command for another has not solved anything.
    check(!queue.push("", frameOf(kBudget)),
          "a reply that will not fit even after every notification is dropped is REFUSED, and "
          "the queue reports it rather than growing");
    check(queue.replyRefused, "the refusal is recorded for the caller to act on");
    check(queue.bytes() <= kBudget, "refusal never grew the queue past its budget");

    // And the reply that WAS admitted is still there, byte for byte, having been
    // bought with notifications rather than with itself.
    check(!queue.empty() && drainInChunks(&queue, 64) == "the answer",
          "the queued reply survives the whole exercise intact, so what the room-making path "
          "gave up was never a command");
}

void testAFrameInTheWireIsSacred() {
    OutboundQueue queue;
    // A notification three quarters sent: its first bytes are already the
    // peer's, so nothing may rewrite or remove it.
    check(queue.push("note", frameOf(4000)), "the frame is queued");
    queue.didWrite(3000);
    check(!queue.empty() && queue.headRemaining() == 1000,
          "a part-written frame stays at the head until its last byte");

    check(queue.push("note", frameOf(10)),
          "a newer notification of the same key is accepted while the old one is in the wire");
    check(queue.coalesced == 0 && queue.droppedNotifications == 1,
          "the newer one is dropped rather than rewriting bytes mid-frame — the producer "
          "reports the state again once this frame is gone");
    check(queue.headRemaining() == 1000,
          "and the head is still the ORIGINAL frame's remaining bytes, not the replacement's");
    check(3000 + drainInChunks(&queue, 64).size() == 4000,
          "and the peer still receives the original frame whole: what was sent plus what was "
          "left, with nothing spliced into the middle of it");

    // The same rule protects the room-making path: with the queue at its budget
    // and the head part-written, the head must survive even though it is a
    // notification.
    OutboundQueue full;
    check(full.push("note", frameOf(kBudget)), "one notification can occupy the whole budget");
    full.didWrite(1024);
    check(!full.push("", frameOf(4096)),
          "a reply cannot evict the notification that is mid-frame, however badly it wants "
          "the room: evicting it would misalign everything behind it");
    check(full.droppedNotifications == 0, "nothing was evicted, because nothing could be");
}

void testTheFrameCountIsBoundedToo() {
    OutboundQueue queue;
    // Every key distinct, so nothing coalesces and every accepted frame is one
    // more the queue really holds. These are tiny, so the entry count binds
    // first — the byte bound binds for big frames, and both have to hold.
    // A notification that does not fit is DROPPED rather than refused: push
    // still succeeds, because there is nothing to answer and the producer will
    // report the state again. So the count of what was lost is the queue's own.
    for (std::size_t i = 0; i < kFrameCap + 50; ++i) {
        queue.push("note:" + std::to_string(i), "x");
    }
    check(queue.frames() == kFrameCap,
          "the entry count stops at 1024 whatever the frames' size — a million tiny frames is "
          "a million-entry scan per frame, which is a stall of a different shape");
    check(queue.droppedNotifications == 50,
          "the frames past the bound were dropped, not queued: the queue grew by nothing");
}

}  // namespace

int main() {
    testTheBoundsAreTheOnesTheCodeClaims();
    testOrderingAndCoalescing();
    testAReplyIsNeverLostWhileNotificationsCan();
    testAFrameInTheWireIsSacred();
    testTheFrameCountIsBoundedToo();

    if (gFailures != 0) {
        std::fprintf(stderr, "%d outbound queue check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("ok: the outbound queue bounds bytes and frames, keeps replies, and never "
                "touches a frame already in the wire\n");
    return 0;
}
