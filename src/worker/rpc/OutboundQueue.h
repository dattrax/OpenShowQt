/*
 * OutboundQueue: the bytes an RPC server owes its frontend, bounded and
 * coalescing (task 6.8b, review R11).
 *
 * A server that writes to a socket synchronously is at the mercy of whoever
 * reads it: one peer that stops, and the server thread is parked in the kernel
 * with no command handling, no disconnect detection and no prompt shutdown.
 * The alternative is a queue, and a queue is only safe if it has a shape — so
 * this owns the two rules that make buffering a stalled frontend survivable:
 *
 *   A REPLY (empty key) answers a request and is never dropped, reordered, or
 *   allowed to be pushed out by anything else. Losing one is how a client ends
 *   up waiting forever for an answer to a command whose outcome it cannot
 *   determine, which is worse than a refused connection.
 *
 *   A NOTIFICATION carries state its producer has already reduced to "the
 *   newest of each", so it is identified by a key and queueing a second one for
 *   the same key REPLACES the first. That is what the consumer wants — it takes
 *   the current state, not a transcript of how it got here — and it is what
 *   stops a chatty notification source from filling the queue and starving the
 *   replies behind it. A notification that will not fit is dropped, since the
 *   producer will report the state again anyway.
 *
 * A frame that has already started going out can never be replaced or given up:
 * its bytes are in the peer's stream, and abandoning or rewriting it would
 * misalign every message behind it. Those bytes stay until they are written.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <string>
#include <utility>

#include "openshow/protocol/protocol.h"

namespace openshow::worker {

// The most the server will hold for one frontend. One message budget,
// deliberately: the transport refuses a single frame larger than this, so
// refusing to HOLD more is the same rule from the other end — and it is far
// more than a real exchange needs, since the largest reply the worker makes is
// output.status and a notification is a hundred bytes. A backlog past one frame
// is not a frontend that is slow, it is one that has stopped, and buffering
// that is how a wedged client turns into a server that grows. This IS the
// transport's own limit rather than a number next to it, so the two cannot
// drift apart.
constexpr std::size_t kMaxQueuedBytes = openshow::protocol::kMaxMessageBytes;
// The entry count is bounded separately because the coalescing scan and the
// per-entry bookkeeping scale with frames rather than bytes. This is a backstop
// for many small frames; kMaxQueuedBytes is the ceiling for big ones.
constexpr std::size_t kMaxQueuedFrames = 1024;

class OutboundQueue {
public:
    // Reported on the log line that ends a connection, so "why did the server
    // hang up" is answerable from the log alone.
    std::size_t coalesced = 0;
    std::size_t droppedNotifications = 0;
    // Set by the queue when a REPLY would not fit: the caller's only honest
    // option left is to say so and close.
    bool replyRefused = false;

    bool empty() const { return fEntries.empty(); }
    std::size_t bytes() const { return fUnwritten; }
    std::size_t frames() const { return fEntries.size(); }

    // Takes the frame. `key` empty means "this is a reply": it is queued, or the
    // queue refuses and sets replyRefused. A non-empty key means "the newest of
    // this kind", and such a frame is replaced or dropped, never refused.
    bool push(std::string key, std::string frame) {
        if (!key.empty()) {
            for (Entry& queued : fEntries) {
                if (queued.key != key) {
                    continue;
                }
                if (queued.offset != 0) {
                    // Part-way down the wire: its bytes are already in the
                    // peer's stream. Not a loss — the producer holds the newest
                    // state and offers it again once this frame is gone.
                    ++droppedNotifications;
                    return true;
                }
                fUnwritten -= std::min(fUnwritten, queued.bytes.size());
                queued.bytes = std::move(frame);
                fUnwritten += queued.bytes.size();
                ++coalesced;
                return true;
            }
        }
        if (!admits(frame.size())) {
            if (!key.empty()) {
                ++droppedNotifications;  // advisory yields; a command does not
                return true;
            }
            replyRefused = true;
            return false;
        }
        fUnwritten += frame.size();
        fEntries.push_back(Entry{std::move(key), std::move(frame), 0});
        return true;
    }

    // The head of the queue: the only frame that may be written, always front to
    // back, so the peer's stream stays aligned.
    const char* headData() const { return fEntries.front().bytes.data(); }
    std::size_t headRemaining() const {
        return fEntries.front().bytes.size() - fEntries.front().offset;
    }

    void didWrite(std::size_t written) {
        fUnwritten -= std::min(written, fUnwritten);
        Entry& head = fEntries.front();
        head.offset += written;
        if (head.offset >= head.bytes.size()) {
            fEntries.pop_front();
        }
    }

private:
    struct Entry {
        std::string key;
        std::string bytes;
        std::size_t offset = 0;
    };

    bool fits(std::size_t incoming) const {
        return fUnwritten + incoming <= kMaxQueuedBytes && fEntries.size() < kMaxQueuedFrames;
    }

    // Room for a reply is worth making: queued notifications are advisory and
    // the same state is reported again soon, while a lost reply costs the
    // operator a command whose outcome they cannot determine. Only frames that
    // have not started going out can be given up.
    bool admits(std::size_t incoming) {
        if (fits(incoming)) {
            return true;
        }
        auto entry = fEntries.begin();
        while (entry != fEntries.end() && !fits(incoming)) {
            if (entry->key.empty() || entry->offset != 0) {
                ++entry;
                continue;
            }
            fUnwritten -= std::min(fUnwritten, entry->bytes.size());
            entry = fEntries.erase(entry);
            ++droppedNotifications;
        }
        return fits(incoming);
    }

    std::deque<Entry> fEntries;
    std::size_t fUnwritten = 0;
};

}  // namespace openshow::worker
