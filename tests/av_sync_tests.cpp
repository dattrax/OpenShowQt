// Task 5.4 acceptance: playback clocks and synchronization. Four layers of
// evidence:
//
//  1. AudioClock transport primitives (no threads, no daemon): the media
//     clock advances with delivered frames, pause() holds the position
//     exactly, resume() continues it without a jump, rebase() lands a
//     seek target, anchor() clears the transport.
//  2. A seek through the REAL production chain (AudioFileFeeder ->
//     AudioMixer -> paced fake device) over an internally written pattern
//     wav (loud / silence / quiet regions, separated by RMS): after
//     requestSeek + waitSeekApplied, the captured output leaves the old
//     region — the stale queue dropped by the seeked callback's input
//     flush is never heard — and lands in the target region. Plus the
//     no-thread wait discipline (returns false, never hangs) and the
//     loop-default mirror.
//  3. AvPlayer without a daemon (enableSink=false): the orchestrator runs
//     the same chain on the monotonic fallback clock; the seek fan-out
//     reaches the feeder, the loop toggle and the volume passthrough
//     land on the mixer.
//  4. AvPlayer with the live PipeWire sink: the server consumes (callbacks
//     and clock advance), pause() freezes the clock at the position and
//     deactivates the stream, play() continues it seamlessly (the queue
//     IS the continuation — flushing would punch a hole), seekTo()
//     rebases the clock to the target and the sink's flush counter moves.
//     Without a daemon this scenario reports an honest skip.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/AudioClock.h"
#include "media/AudioFeeder.h"
#include "media/AudioMixer.h"
#include "media/PipeWireSink.h"
#include "playback/AvPlayer.h"

namespace {

using openshow::media::AudioClock;
using openshow::media::AudioFileFeeder;
using openshow::media::AudioMixer;
using openshow::media::AudioRing;
using openshow::media::PipeWireSink;
using openshow::worker::AvPlayer;

bool check(bool ok, const char* message) {
    std::fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "ok" : "FAIL", message);
    return ok;
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 10) {
        if (predicate()) {
            return true;
        }
        sleepMs(10);
    }
    return predicate();
}

// Fake output device with the sink process callback's discipline: the
// mixer writes into a bounded ring, a drain thread pulls nominal-rate
// 10 ms blocks (zero-padding the shortfall) and records everything.
// The capture side is deliberately LOCK-FREE (preallocated buffer plus a
// published frame count, the same SPSC discipline as the production
// rings): this box's glibc futex path was measured stalling a waiter for
// seconds behind a 10 ms holder cycle, which must not leak into test
// timing — and no production data path needs a mutex either.
struct FakeDevice {
    static constexpr size_t kCapacityFrames = 48000u * 60;  // 60 s of capture

    AudioRing ring;
    std::thread drain;
    std::atomic<bool> stop{false};
    std::vector<float> buffer;  // interleaved; only the drain writes
    std::atomic<size_t> framesCaptured{0};

    bool start(uint32_t rate) {
        buffer.assign(kCapacityFrames * 2, 0.0f);
        if (!ring.create(rate / 10)) {  // 100 ms
            return false;
        }
        drain = std::thread([this, rate] {
            std::vector<float> block(rate / 100 * 2, 0.0f);
            const size_t frames = block.size() / 2;
            size_t pos = 0;
            auto next = std::chrono::steady_clock::now();
            while (!stop.load()) {
                const size_t got = ring.read(block.data(), frames);
                if (got < frames) {
                    std::fill(block.begin() + static_cast<ptrdiff_t>(got) * 2, block.end(), 0.0f);
                }
                if (pos + frames <= kCapacityFrames) {
                    std::copy(block.begin(), block.end(),
                              buffer.begin() + static_cast<ptrdiff_t>(pos) * 2);
                    // Publish content-then-count: readers only touch
                    // frames below the published watermark.
                    framesCaptured.store(pos + frames, std::memory_order_release);
                }
                pos += frames;
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
            }
        });
        return true;
    }
    void stopNow() {
        stop.store(true);
        if (drain.joinable()) {
            drain.join();
        }
    }
    size_t frames() const { return framesCaptured.load(std::memory_order_acquire); }
    std::vector<float> window(size_t fromFrame, size_t frameCount) const {
        const size_t total = frames();
        fromFrame = std::min(fromFrame, total);
        const size_t to = std::min(fromFrame + frameCount, total);
        return std::vector<float>(buffer.begin() + static_cast<ptrdiff_t>(fromFrame) * 2,
                                  buffer.begin() + static_cast<ptrdiff_t>(to) * 2);
    }
};

double rms(const std::vector<float>& samples, int channel) {
    double sum = 0.0;
    size_t counted = 0;
    for (size_t f = static_cast<size_t>(channel); f < samples.size(); f += 2) {
        sum += static_cast<double>(samples[f]) * samples[f];
        ++counted;
    }
    return counted == 0 ? -1.0 : std::sqrt(sum / static_cast<double>(counted));
}

constexpr uint32_t kRate = 48000;

// 8 s stereo s16 pattern: [0,2) 440 Hz amplitude 1.0 (RMS ~0.707),
// [2,5) silence (RMS 0), [5,8) 1000 Hz amplitude 0.5 (RMS ~0.354). RMS
// alone separates the three regions; whole cycles keep phases honest.
bool writePatternWav(const std::string& path) {
    constexpr size_t kSamples = kRate * 8;
    std::vector<int16_t> pcm(kSamples * 2, 0);
    for (size_t i = 0; i < kSamples; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kRate);
        double v = 0.0;
        if (t < 2.0) {
            v = std::sin(2.0 * 3.14159265358979 * 440.0 * t);
        } else if (t >= 5.0) {
            v = 0.5 * std::sin(2.0 * 3.14159265358979 * 1000.0 * (t - 5.0));
        }
        pcm[i * 2] = static_cast<int16_t>(std::lround(v * 32767.0));
        pcm[i * 2 + 1] = pcm[i * 2];
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    const auto put32 = [file](uint32_t v) {
        const uint8_t bytes[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                                  static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
        std::fwrite(bytes, 1, 4, file);
    };
    const auto put16 = [file](uint16_t v) {
        const uint8_t bytes[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
        std::fwrite(bytes, 1, 2, file);
    };
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    std::fwrite("RIFF", 1, 4, file);
    put32(36 + dataBytes);
    std::fwrite("WAVE", 1, 4, file);
    std::fwrite("fmt ", 1, 4, file);
    put32(16);
    put16(1);  // PCM
    put16(2);  // stereo
    put32(kRate);
    put32(kRate * 4);
    put16(4);
    put16(16);
    std::fwrite("data", 1, 4, file);
    put32(dataBytes);
    const bool wrote = std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), file) == pcm.size();
    std::fclose(file);
    return wrote;
}

// --- 1. clock transport primitives ---

bool clockTransport() {
    bool ok = true;
    AudioClock clock;
    clock.setRate(kRate);
    clock.anchor();
    ok = check(clock.clockSeconds() < 0.001, "a fresh anchor reads ~0") && ok;
    clock.addDelivered(4800);  // 100 ms
    const double at100 = clock.clockSeconds();
    ok = check(std::fabs(at100 - 0.1) < 1e-6, "delivered frames advance the media clock") && ok;

    clock.pause();
    ok = check(clock.paused(), "pause reports paused") && ok;
    const double held = clock.clockSeconds();
    clock.addDelivered(4800);  // the reader must not move...
    sleepMs(200);              // ...and neither may wall time
    const double afterHold = clock.clockSeconds();
    ok = check(std::fabs(afterHold - held) < 1e-9,
               "while paused the position is frozen (delivered and wall ignored)") && ok;

    clock.resume();
    ok = check(!clock.paused(), "resume clears paused") && ok;
    const double resumed = clock.clockSeconds();
    ok = check(std::fabs(resumed - held) < 0.02, "resume continues from the hold without a jump") &&
         ok;
    clock.addDelivered(4800);
    ok = check(clock.clockSeconds() > resumed + 0.05, "the clock advances again after resume") &&
         ok;

    clock.rebase(10.0);
    ok = check(std::fabs(clock.clockSeconds() - 10.0) < 0.001, "rebase lands the seek target") &&
         ok;
    clock.pause();
    ok = check(std::fabs(clock.clockSeconds() - 10.0) < 0.001,
               "a paused clock holds the rebased target") && ok;
    clock.resume();
    clock.anchor();
    ok = check(std::fabs(clock.clockSeconds()) < 0.001 && !clock.paused(),
               "anchor clears offsets and the paused state") && ok;

    // The stream-time path must obey the same transport semantics: hold
    // at the reported position, continue on resume, land a rebase.
    AudioClock streamClock;
    streamClock.setRate(kRate);
    streamClock.anchor();
    streamClock.setStreamTime(0, AudioClock::monotonicNs(), static_cast<double>(kRate));
    sleepMs(200);
    streamClock.pause();
    const double streamHeld = streamClock.clockSeconds();
    ok = check(streamHeld > 0.1 && streamHeld < 0.5,
               "stream time drives the clock between callbacks") && ok;
    sleepMs(250);  // callbacks stopped arriving (the server is inactive);
                   // the extrapolation must NOT carry the position
    ok = check(std::fabs(streamClock.clockSeconds() - streamHeld) < 1e-9,
               "a paused stream clock does not extrapolate past its hold") && ok;
    streamClock.resume();
    ok = check(std::fabs(streamClock.clockSeconds() - streamHeld) < 0.05,
               "resuming a stream clock continues from the hold") && ok;
    streamClock.rebase(3.0);
    ok = check(std::fabs(streamClock.clockSeconds() - 3.0) < 0.05,
               "rebase works on the stream-time path too") && ok;
    return ok;
}

// --- 2. feeder seek through the real mixer chain ---

bool feederSeekToTarget(const std::string& wav) {
    bool ok = true;
    FakeDevice device;
    device.start(kRate);
    AudioMixer mixer;
    const int in = mixer.addInput();
    std::string error;
    ok = check(mixer.start(
                   [&device](const float* frames, size_t count) {
                       return device.ring.write(frames, count);
                   },
                   kRate, &error),
               "mixer starts for the seek scenario") && ok;

    AudioFileFeeder feeder;
    ok = check(feeder.open(wav, kRate, &error), "feeder opens the pattern wav") && ok;
    // The production flush seam: exactly what AvPlayer wires — the
    // callback runs while the feeder is quiet, so dropping the input's
    // queue here discards only pre-jump content.
    feeder.setSeekedCallback([&mixer, in](double) { mixer.requestInputFlush(in); });
    // loop=true keeps the decode thread alive for the later seeks (with
    // loop off it would finish the 8 s file and end).
    ok = check(feeder.start(
                   [&mixer, in](const float* frames, size_t count) {
                       return mixer.write(in, frames, count);
                   },
                   true, &error),
               "feeder decodes the pattern wav into the mixer") && ok;
    ok = check(feeder.loopEnabled(), "loopEnabled mirrors the start value") && ok;

    sleepMs(600);  // past the fill: the loud region is flowing to the device
    {
        // Diagnostic scan: per-100 ms RMS and zero-crossings across the
        // whole capture so the heard region layout is printed, not
        // inferred (440 Hz ~88 crossings/100 ms, 1 kHz ~200, silence ~0).
        const size_t total = device.frames();
        std::printf("   capture scan after the 600 ms warm-up:\n");
        for (size_t start = 0; start + 4800 <= total; start += 4800) {
            const std::vector<float> w = device.window(start, 4800);
            int crossings = 0;
            for (size_t f = 1; f < w.size() / 2; ++f) {
                if ((w[(f - 1) * 2] < 0.0f) != (w[f * 2] < 0.0f)) {
                    ++crossings;
                }
            }
            std::printf("   [%5.2f..%5.2fs] rms=%.4f zc=%d\n", start / 48000.0,
                        (start + 4800) / 48000.0, rms(w, 0), crossings);
        }
        const auto ms = mixer.stats();
        std::printf("   mixer written=%llu mixed=%llu starved=%llu device=%zu frames\n",
                    static_cast<unsigned long long>(ms.input[in].framesWritten),
                    static_cast<unsigned long long>(ms.input[in].framesMixed),
                    static_cast<unsigned long long>(ms.input[in].starvedBlocks), total);
    }
    const double loudRms = rms(device.window(device.frames() - 4800, 4800), 0);
    ok = check(loudRms > 0.6, "the loud region reaches the device first") && ok;

    // --- backward-into-content seek: 3.0 s is the silence region. The
    // input ring holds ~2 s of loud pre-jump audio; only a working
    // flush makes the device go silent quickly.
    const uint64_t seeksBefore = feeder.stats().seeks;
    feeder.requestSeek(3.0);
    const bool applied1 = feeder.waitSeekApplied(1000);
    ok = check(applied1, "the silence-region seek applied") && ok;
    ok = check(feeder.stats().seeks == seeksBefore + 1, "the applied seek is counted") && ok;
    ok = check(std::fabs(feeder.lastSeekSeconds() - 3.0) < 1e-6,
               "lastSeekSeconds reports the target") && ok;
    sleepMs(350);  // device ring (100 ms) plus settle
    const size_t afterSeek1 = device.frames();
    const double silenceRms = rms(device.window(afterSeek1 - 4800, 4800), 0);
    std::printf("   post-seek(3.0 s) heard RMS %.4f (want silence)\n", silenceRms);
    ok = check(silenceRms < 0.06,
               "the stale queue was flushed — the device is in the silence region") && ok;
    ok = check(mixer.stats().input[in].flushes >= 1, "the input flush is counted") && ok;

    // --- forward seek into the quiet tone region ---
    feeder.requestSeek(6.5);
    ok = check(feeder.waitSeekApplied(1000), "the tone-region seek applied") && ok;
    sleepMs(350);
    const size_t afterSeek2 = device.frames();
    const double quietRms = rms(device.window(afterSeek2 - 4800, 4800), 0);
    std::printf("   post-seek(6.5 s) heard RMS %.4f (want ~0.354)\n", quietRms);
    ok = check(quietRms > 0.25 && quietRms < 0.46, "the device lands in the target region") && ok;
    ok = check(feeder.stats().seeks == 2, "both seeks counted") && ok;

    // The wait discipline without a decode thread: false, quickly, no hang.
    feeder.requestStop();
    feeder.join();
    feeder.requestSeek(1.0);
    const auto waitStart = std::chrono::steady_clock::now();
    const bool appliedDead = feeder.waitSeekApplied(300);
    const double waitedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart)
            .count();
    ok = check(!appliedDead && waitedMs < 250.0,
               "waitSeekApplied with no thread returns false instead of hanging") && ok;

    mixer.requestStop();
    mixer.join();
    device.stopNow();
    return ok;
}

// --- 3. AvPlayer on the monotonic fallback (no daemon needed) ---

bool avPlayerFallback(const std::string& wav) {
    bool ok = true;
    AvPlayer player;
    AvPlayer::Config config;
    config.videoAudioPath = wav;
    config.loop = true;
    config.enableSink = false;  // exercise the daemon-less shape on purpose
    std::string error;
    ok = check(player.start(config, &error), "the player starts without a sink") && ok;
    ok = check(!player.audioLive(), "audioLive is false without a sink") && ok;
    ok = check(!player.sinkActive(), "the sink never activated") && ok;

    sleepMs(300);
    const double clock = player.clockSeconds();
    ok = check(clock > 0.15 && clock < 0.6,
               "the monotonic fallback clock advances with wall time") && ok;

    player.setVolume(AvPlayer::kVideoInput, 0.5f);
    player.setMute(AvPlayer::kSecondInput, true);
    ok = check(player.volume(AvPlayer::kVideoInput) == 0.5f,
               "setVolume passes through to the mixer") && ok;
    ok = check(!player.muted(AvPlayer::kVideoInput),
               "muting an unregistered input leaves the real ones alone") && ok;

    player.seekTo(4.0);  // no sink: the fan-out still reaches the feeder
    ok = check(player.feederStats(false).seeks == 1, "the transport seek reaches the feeder") &&
         ok;
    ok = check(std::fabs(player.clockSeconds() - 4.0) > 0.5,
               "without audio the monotonic clock is untouched by a seek") && ok;

    // The sink-less chain free-runs (the mixer output discards), so with
    // the rewind off the feeder finishes the file and reports eof — the
    // toggle's proof is eof arriving, not the feeder staying alive.
    player.setLoop(false);
    ok = check(!player.loopEnabled(), "setLoop mirrors the toggle off") && ok;
    ok = check(waitFor([&] { return player.feederStats(false).eof; }, 10000),
               "with loop off the free-running feeder reaches eof") && ok;

    player.stop();
    player.stop();  // idempotent
    ok = check(!player.audioLive(), "stop leaves the player silent and down") && ok;
    return ok;
}

// --- 4. AvPlayer with the live PipeWire sink ---

bool avPlayerLive(const std::string& wav) {
    // Probe first so a missing daemon is a clean skip, not a failed run.
    PipeWireSink probe;
    std::string probeError;
    if (!probe.start(kRate, &probeError)) {
        std::printf("   skipped the live transport scenario (no PipeWire daemon: %s)\n",
                    probeError.c_str());
        return true;
    }
    probe.stop();

    bool ok = true;
    AvPlayer player;
    AvPlayer::Config config;
    config.videoAudioPath = wav;
    config.loop = true;
    std::string error;
    ok = check(player.start(config, &error), "the player starts with the live sink") && ok;
    ok = check(player.audioLive(), "audioLive is true with the daemon") && ok;
    ok = check(player.sinkActive(), "the sink stream is active after start") && ok;

    sleepMs(600);
    const auto liveStats = player.sinkStats();
    // Slice cadence: with the 500 ms sink ring the server pulls large
    // slices (hundreds of ms each), so callbacks arrive a few times per
    // second, not at the graph quantum rate. Two in 600 ms plus a
    // advancing clock is the honest floor.
    ok = check(liveStats.callbacks >= 2, "the server is pulling callbacks") && ok;
    const double heard = player.clockSeconds();
    ok = check(heard > 0.3, "the media clock advanced with server consumption") && ok;
    std::printf("   live clock %.3fs callbacks=%llu underrun-frames=%llu flushes=%llu\n", heard,
                static_cast<unsigned long long>(liveStats.callbacks),
                static_cast<unsigned long long>(liveStats.underrunFrames),
                static_cast<unsigned long long>(liveStats.flushes));
    ok = check(liveStats.underrunFrames < kRate / 2,
               "the pre-roll+ring feeding kept underruns well under 0.5 s") && ok;

    // pause: the clock freezes at the position, the stream deactivates.
    player.pause();
    ok = check(!player.sinkActive(), "pause deactivated the stream") && ok;
    const double held = player.clockSeconds();
    sleepMs(300);
    const double stillHeld = player.clockSeconds();
    ok = check(std::fabs(stillHeld - held) < 0.002,
               "the live clock held its position across the pause (±2 ms)") && ok;

    // play: seamless continuation (the queue is the continuation).
    player.play();
    ok = check(player.sinkActive(), "play reactivated the stream") && ok;
    const double resumed = player.clockSeconds();
    ok = check(std::fabs(resumed - stillHeld) < 0.05,
               "resume continues from the hold without a jump") && ok;
    sleepMs(300);
    ok = check(player.clockSeconds() > resumed + 0.15, "the clock advances again after resume") &&
         ok;

    // seek: the clock rebases to the target and the sink flush is answered.
    const uint64_t flushesBefore = player.sinkStats().flushes;
    player.seekTo(6.5);
    ok = check(player.feederStats(false).seeks == 1, "the fan-out seek reached the feeder") && ok;
    const double atTarget = player.clockSeconds();
    ok = check(std::fabs(atTarget - 6.5) < 0.15, "the live clock rebased to the seek target") &&
         ok;
    const bool flushed = waitFor(
        [&] { return player.sinkStats().flushes >= flushesBefore + 1; }, 2000);
    ok = check(flushed, "the server-side flush was answered") && ok;

    // A paused seek parks the held position on the target (a parked
    // transport shows the target, and the flush lands on resume).
    player.pause();
    player.seekTo(2.0);
    ok = check(std::fabs(player.clockSeconds() - 2.0) < 0.15,
               "a paused seek lands the held position on the target") && ok;
    player.play();
    // The flush is answered by the first post-resume process callback —
    // one slice period away, so wait generously.
    const bool pausedFlushed = waitFor(
        [&] { return player.sinkStats().flushes >= flushesBefore + 2; }, 2000);
    ok = check(pausedFlushed, "the paused-seek flush was answered on resume") && ok;

    player.stop();
    const auto finalStats = player.sinkStats();
    std::printf("   live run done: clock=%.3fs underrun-frames=%llu flushes=%llu\n",
                finalStats.clockSeconds,
                static_cast<unsigned long long>(finalStats.underrunFrames),
                static_cast<unsigned long long>(finalStats.flushes));
    return ok;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = "av-sync-fixtures";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string wav = (dir / "pattern-8s.wav").string();
    if (!writePatternWav(wav)) {
        std::fprintf(stderr, "FAIL: the pattern wav could not be written\n");
        return 1;
    }

    bool ok = true;
    ok = clockTransport() && ok;
    ok = feederSeekToTarget(wav) && ok;
    ok = avPlayerFallback(wav) && ok;
    ok = avPlayerLive(wav) && ok;
    return ok ? 0 : 1;
}
