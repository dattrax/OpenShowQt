// Task 5.3 acceptance: PipeWire output and stereo mixing — video and
// standalone audio have independent volume and mute. The mixer is tested
// through its real shape: bounded SPSC input rings fed by source threads
// (and, in the last scenario, real AudioFileFeeders decoding generated
// wav files), one mix thread, and the injected write callback pointed at
// a fake output device — an AudioRing drained by a thread at nominal
// rate, zero-padding the shortfall, exactly the discipline the PipeWire
// process callback applies. The mixer itself knows nothing about
// PipeWire; the av-bench rerun with --second-audio covers the live sink.
// The wav fixtures are written here (PCM s16 sine with exact amplitudes)
// so the expected RMS values are analytic rather than whatever the CLI's
// sine source happens to emit on a given build; decoding them still runs
// the real FFmpeg seam.
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

#include "media/AudioFeeder.h"
#include "media/AudioMixer.h"
#include "media/AudioRing.h"

namespace {

using openshow::media::AudioFileFeeder;
using openshow::media::AudioMixer;
using openshow::media::AudioRing;

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

// The fake output device: the mixer's write callback feeds this ring, a
// drain thread pulls nominal-rate blocks (10 ms every 10 ms), zero-pads
// the shortfall (counted as underrun frames) and records everything.
// Its capacity bounds the mixer's delivery rate, which is exactly how
// the sink paces the mixer in production.
struct FakeDevice {
    AudioRing ring;
    std::thread drain;
    std::atomic<bool> stop{false};
    mutable std::mutex mutex;
    std::vector<float> captured;  // interleaved stereo
    uint64_t underrunFrames = 0;

    bool start(uint32_t rate) {
        if (!ring.create(rate / 10)) {  // 100 ms: small, latency-bounding
            return false;
        }
        drain = std::thread([this, rate] {
            std::vector<float> block(rate / 100 * 2, 0.0f);
            const size_t frames = block.size() / 2;
            // Nominal-rate pacing: one block per 10 ms of wall time,
            // anchored so the sleeps do not drift the stream slower.
            auto next = std::chrono::steady_clock::now();
            while (!stop.load()) {
                const size_t got = ring.read(block.data(), frames);
                if (got < frames) {
                    std::fill(block.begin() + static_cast<ptrdiff_t>(got) * 2, block.end(),
                              0.0f);
                    std::lock_guard<std::mutex> lock(mutex);
                    underrunFrames += frames - got;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    captured.insert(captured.end(), block.begin(), block.end());
                }
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
            }
        });
        return true;
    }
    void resetUnderruns() {
        std::lock_guard<std::mutex> lock(mutex);
        underrunFrames = 0;
    }
    void stopNow() {
        stop.store(true);
        if (drain.joinable()) {
            drain.join();
        }
    }
    size_t frames() const {
        std::lock_guard<std::mutex> lock(mutex);
        return captured.size() / 2;
    }
    std::vector<float> window(size_t fromFrame, size_t frameCount) const {
        std::lock_guard<std::mutex> lock(mutex);
        fromFrame = std::min(fromFrame, captured.size() / 2);
        const size_t to = std::min(fromFrame + frameCount, captured.size() / 2);
        return std::vector<float>(captured.begin() + static_cast<ptrdiff_t>(fromFrame) * 2,
                                  captured.begin() + static_cast<ptrdiff_t>(to) * 2);
    }
};

// A continuous DC source thread for one mixer input.
struct DcSource {
    std::thread thread;
    std::atomic<bool> stop{false};

    void start(AudioMixer& mixer, int input, float left, float right, uint32_t rate) {
        thread = std::thread([this, &mixer, input, left, right, rate] {
            const size_t blockFrames = rate / 100;
            std::vector<float> block(blockFrames * 2);
            for (size_t f = 0; f < blockFrames; ++f) {
                block[f * 2] = left;
                block[f * 2 + 1] = right;
            }
            while (!stop.load()) {
                if (mixer.write(input, block.data(), blockFrames) == 0) {
                    sleepMs(2);  // input ring full: the pacing signal
                }
            }
        });
    }
    void stopNow() {
        stop.store(true);
        if (thread.joinable()) {
            thread.join();
        }
    }
};

constexpr uint32_t kRate = 48000;

// Minimal stereo PCM s16 WAV writer: whole-cycle sines at exact amplitudes,
// so the per-channel RMS at the mixer output is analytic (amplitude /
// sqrt(2)) and the feeder's loop cannot restart at a broken phase. Stereo
// in, stereo out keeps the feeder's resample a plain format conversion —
// some swresample builds attenuate a mono upmix by 1/sqrt(2), and the
// fixture must not depend on that.
bool writeSineWav(const std::string& path, double frequency, double amplitude) {
    constexpr double kTwoPi = 6.283185307179586;
    const size_t samples = 96000;  // 2 s at 48 k: 880 cycles at 440 Hz, 2000 at 1000 Hz
    std::vector<int16_t> pcm(samples * 2);
    for (size_t i = 0; i < samples; ++i) {
        const double v = amplitude * std::sin(kTwoPi * frequency * static_cast<double>(i) /
                                              static_cast<double>(kRate));
        const int16_t sample = static_cast<int16_t>(std::lround(v * 32767.0));
        pcm[i * 2] = sample;
        pcm[i * 2 + 1] = sample;
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
    put16(1);               // PCM
    put16(2);               // stereo
    put32(kRate);
    put32(kRate * 4);       // byte rate
    put16(4);               // block align
    put16(16);              // bits
    std::fwrite("data", 1, 4, file);
    put32(dataBytes);
    const bool wrote = std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), file) == pcm.size();
    std::fclose(file);
    return wrote;
}

// --- scenario: two DC inputs, exact mix against volume/mute combos ---

bool dcMix() {
    bool ok = true;
    FakeDevice device;
    if (!check(device.start(kRate), "fake device drains at nominal rate")) {
        return false;
    }
    AudioMixer mixer;
    const int a = mixer.addInput();
    const int b = mixer.addInput();
    ok = check(a == 0 && b == 1, "two inputs registered") && ok;

    // Asymmetric channels pin the interleaving: L and R must stay apart
    // through gain and summing.
    DcSource srcA, srcB;
    srcA.start(mixer, a, 0.30f, 0.10f, kRate);
    srcB.start(mixer, b, -0.20f, 0.40f, kRate);
    std::string error;
    ok = check(mixer.start([&device](const float* frames, size_t count) {
                   return device.ring.write(frames, count);
               },
               kRate, &error),
               "mixer starts against the fake device") && ok;
    sleepMs(300);  // let the device ring fill; underruns before this are startup only
    device.resetUnderruns();

    struct Combo {
        float volumeA;
        bool muteA;
        float volumeB;
        bool muteB;
        const char* label;
    };
    const Combo combos[] = {
        {1.0f, false, 1.0f, false, "both live at unity"},
        {0.5f, false, 1.0f, false, "input A at half volume"},
        {1.0f, true, 0.25f, false, "A muted while B plays at a quarter"},
        {0.75f, false, 1.0f, true, "B muted while A plays"},
        {1.0f, true, 1.0f, true, "both muted falls to silence"},
    };
    // Latency bound: the mixer's gain applies at consumption, and the
    // device ring (100 ms) sits between delivery and capture; 400 ms of
    // settling puts every captured sample after the mark past the change.
    for (const Combo& combo : combos) {
        mixer.setVolume(a, combo.volumeA);
        mixer.setMute(a, combo.muteA);
        mixer.setVolume(b, combo.volumeB);
        mixer.setMute(b, combo.muteB);
        sleepMs(400);
        const size_t mark = device.frames();
        const bool grew = waitFor([&] { return device.frames() >= mark + 4800; }, 2000);
        ok = check(grew, "the device keeps capturing through the change") && ok;
        const auto samples = device.window(mark, 4800);
        const float gainA = combo.muteA ? 0.0f : combo.volumeA;
        const float gainB = combo.muteB ? 0.0f : combo.volumeB;
        const float wantL = gainA * 0.30f + gainB * -0.20f;
        const float wantR = gainA * 0.10f + gainB * 0.40f;
        bool exact = samples.size() == 4800 * 2;
        for (size_t f = 0; f + 1 < samples.size(); f += 2) {
            exact = exact && std::fabs(samples[f] - wantL) < 1e-4f &&
                    std::fabs(samples[f + 1] - wantR) < 1e-4f;
        }
        ok = check(exact, combo.label) && ok;
        if (!exact) {
            std::fprintf(stderr, "  combo %s: want L=%.4f R=%.4f, got L=%.4f R=%.4f\n",
                         combo.label, wantL, wantR, samples.empty() ? 0.0f : samples[0],
                         samples.size() > 1 ? samples[1] : 0.0f);
        }
    }
    const auto stats = mixer.stats();
    ok = check(stats.input[a].framesWritten > stats.input[a].framesMixed &&
                   stats.input[b].framesMixed > 0,
               "per-input stats count what each side did") && ok;

    srcA.stopNow();
    srcB.stopNow();
    mixer.requestStop();
    mixer.join();
    const uint64_t underruns = device.underrunFrames;
    device.stopNow();
    ok = check(underruns == 0, "a fed mixer never starved the fake device") && ok;
    return ok;
}

// --- scenario: volume 0 -> 1 live produces no discontinuity ---

// The signal is DC so the only sample-to-sample jumps come from the
// gain ramp itself; the largest per-frame gain step is
// (block/ramp-seconds)/rate = 0.2/480 ≈ 0.0004, far under 0.05.
bool clicklessRamp() {
    bool ok = true;
    FakeDevice device;
    device.start(kRate);
    AudioMixer mixer;
    const int in = mixer.addInput();
    mixer.setVolume(in, 0.0f);
    DcSource src;
    src.start(mixer, in, 1.0f, 1.0f, kRate);
    std::string error;
    ok = check(mixer.start([&device](const float* frames, size_t count) {
                   return device.ring.write(frames, count);
               },
               kRate, &error),
               "mixer starts muted by volume") && ok;
    sleepMs(150);
    mixer.setVolume(in, 1.0f);
    sleepMs(600);
    const auto samples = device.window(0, device.frames());
    float maxJump = 0.0f;
    for (size_t i = 2; i < samples.size(); ++i) {
        maxJump = std::max(maxJump, std::fabs(samples[i] - samples[i - 1]));
    }
    ok = check(maxJump < 0.05f, "volume 0->1 ramps: no sample jump approaches a click") && ok;
    std::printf("ok: max consecutive-sample jump %.6f over %zu frames\n", maxJump,
                samples.size() / 2);
    // And the ramp actually lands at full level.
    const size_t frames = device.frames();
    const auto tail = device.window(frames > 960 ? frames - 960 : 0, 960);
    double sum = 0.0;
    for (size_t f = 0; f + 1 < tail.size(); f += 2) {
        sum += tail[f];
    }
    const double tailMean = tail.empty() ? 0.0 : sum / (tail.size() / 2);
    ok = check(std::fabs(tailMean - 1.0) < 1e-3, "the ramp settles at the set volume") && ok;

    src.stopNow();
    mixer.requestStop();
    mixer.join();
    device.stopNow();
    return ok;
}

// --- scenario: a starved input keeps the output alive, starts later ---

bool starvedInput() {
    bool ok = true;
    FakeDevice device;
    device.start(kRate);
    AudioMixer mixer;
    const int a = mixer.addInput();
    const int b = mixer.addInput();
    DcSource srcA;
    srcA.start(mixer, a, 0.5f, -0.5f, kRate);  // input B has no source yet
    std::string error;
    ok = check(mixer.start([&device](const float* frames, size_t count) {
                   return device.ring.write(frames, count);
               },
               kRate, &error),
               "mixer runs with one input, one silent") && ok;
    sleepMs(300);
    device.resetUnderruns();  // anything counted before now is startup only
    const uint64_t starvedB = mixer.stats().input[b].starvedBlocks;
    ok = check(starvedB >= 20, "the silent input's starvation is counted per block") && ok;

    size_t mark = device.frames();
    const bool flowing = waitFor([&] { return device.frames() >= mark + 2400; }, 2000);
    const auto before = device.window(mark, 2400);
    ok = check(flowing, "the device keeps capturing while only A feeds") && ok;
    bool onlyA = before.size() == 2400 * 2;
    for (size_t f = 0; f + 1 < before.size(); f += 2) {
        onlyA = onlyA && std::fabs(before[f] - 0.5f) < 1e-4f &&
                std::fabs(before[f + 1] + 0.5f) < 1e-4f;
    }
    ok = check(onlyA, "the starved input contributes silence, the live one passes through") && ok;

    DcSource srcB;
    srcB.start(mixer, b, 0.25f, 0.25f, kRate);  // B joins while the mixer runs
    sleepMs(400);
    mark = device.frames();
    const bool grew = waitFor([&] { return device.frames() >= mark + 2400; }, 2000);
    ok = check(grew, "the output never stopped while B was joining") && ok;
    const auto after = device.window(mark, 2400);
    bool mixed = after.size() == 2400 * 2;
    for (size_t f = 0; f + 1 < after.size(); f += 2) {
        mixed = mixed && std::fabs(after[f] - 0.75f) < 1e-4f &&
                std::fabs(after[f + 1] + 0.25f) < 1e-4f;
    }
    ok = check(mixed, "B flows the moment it starts feeding (no permanent penalty)") && ok;

    srcA.stopNow();
    srcB.stopNow();
    mixer.requestStop();
    mixer.join();
    ok = check(device.underrunFrames == 0,
               "the mixer fed the device steadily despite total input starvation") && ok;
    device.stopNow();
    return ok;
}

// --- scenario: real feeders, independent mute at the real decode seam ---

// Input 0 plays a full-scale 440 Hz sine (RMS 1/sqrt(2) ≈ 0.707), input 1
// a 1000 Hz sine at 0.424 amplitude (RMS ≈ 0.3). Both live the sum's RMS
// is sqrt(0.5 + 0.09) ≈ 0.768; muting input 0 must leave ≈ 0.3. Both
// files hold whole sine cycles, so the feeders' loops are seamless.
double rms(const std::vector<float>& samples, int channel) {
    double sum = 0.0;
    size_t counted = 0;
    for (size_t f = static_cast<size_t>(channel); f < samples.size(); f += 2) {
        sum += static_cast<double>(samples[f]) * samples[f];
        ++counted;
    }
    return counted == 0 ? -1.0 : std::sqrt(sum / static_cast<double>(counted));
}

bool feedersThroughMixer(const std::string& wavA, const std::string& wavB) {
    bool ok = true;
    FakeDevice device;
    device.start(kRate);
    AudioMixer mixer;
    const int inA = mixer.addInput();
    const int inB = mixer.addInput();
    std::string error;
    ok = check(mixer.start([&device](const float* frames, size_t count) {
                   return device.ring.write(frames, count);
               },
               kRate, &error),
               "mixer starts for the feeder scenario") && ok;

    AudioFileFeeder feederA, feederB;
    ok = check(feederA.open(wavA, kRate, &error), "feeder A opens the 440 Hz wav") && ok;
    ok = check(feederB.open(wavB, kRate, &error), "feeder B opens the 1000 Hz wav") && ok;
    ok = check(feederA.start(
                   [&](const float* frames, size_t count) {
                       return mixer.write(inA, frames, count);
                   },
                   true, &error),
               "feeder A decodes into mixer input 0") && ok;
    ok = check(feederB.start(
                   [&](const float* frames, size_t count) {
                       return mixer.write(inB, frames, count);
                   },
                   true, &error),
               "feeder B decodes into mixer input 1") && ok;

    sleepMs(500);  // past the feeders' fill and any ramp
    device.resetUnderruns();
    size_t mark = device.frames();
    const bool flowing = waitFor([&] { return device.frames() >= mark + 9600; }, 3000);
    ok = check(flowing, "both decoded streams flow to the device") && ok;
    const auto both = device.window(mark, 9600);  // 200 ms
    const double rmsBothL = rms(both, 0);
    const double rmsBothR = rms(both, 1);
    ok = check(std::fabs(rmsBothL - 0.768) < 0.05 && std::fabs(rmsBothR - 0.768) < 0.05,
               "both live: mixed RMS ≈ 0.77 (0.707^2 + 0.3^2 summed)") && ok;
    std::printf("ok: both-live RMS L=%.3f R=%.3f\n", rmsBothL, rmsBothR);

    mixer.setMute(inA, true);
    const uint64_t fedAAtMute = feederA.stats().framesFed;
    sleepMs(400);
    mark = device.frames();
    const bool kept = waitFor([&] { return device.frames() >= mark + 9600; }, 3000);
    ok = check(kept, "muting one input never stopped the output") && ok;
    const auto muted = device.window(mark, 9600);
    const double rmsMuted = rms(muted, 0);
    ok = check(std::fabs(rmsMuted - 0.3) < 0.05,
               "A muted: what remains is B alone (RMS ≈ 0.3)") && ok;
    std::printf("ok: A-muted RMS %.3f\n", rmsMuted);
    const uint64_t fedALater = waitFor([&] { return feederA.stats().framesFed > fedAAtMute; },
                                       2000)
                                  ? feederA.stats().framesFed
                                  : fedAAtMute;
    ok = check(fedALater > fedAAtMute,
               "the muted input keeps draining — no backlog for unmute") && ok;

    mixer.setMute(inA, false);
    sleepMs(400);
    mark = device.frames();
    const bool back = waitFor([&] { return device.frames() >= mark + 9600; }, 3000);
    ok = check(back && std::fabs(rms(device.window(mark, 9600), 0) - 0.768) < 0.05,
               "unmute restores the full mix live") && ok;

    feederA.requestStop();
    feederB.requestStop();
    feederA.join();
    feederB.join();
    mixer.requestStop();
    mixer.join();
    device.stopNow();
    ok = check(device.underrunFrames == 0, "no device underrun across the feeder run") && ok;
    return ok;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = "audio-mix-fixtures";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string wavA = (dir / "mix-440.wav").string();
    const std::string wavB = (dir / "mix-1000-quiet.wav").string();
    if (!writeSineWav(wavA, 440.0, 1.0) || !writeSineWav(wavB, 1000.0, 0.424)) {
        std::fprintf(stderr, "FAIL: the sine wav fixtures could not be written\n");
        return 1;
    }

    bool ok = true;
    ok = dcMix() && ok;
    ok = clicklessRamp() && ok;
    ok = starvedInput() && ok;
    ok = feedersThroughMixer(wavA, wavB) && ok;
    return ok ? 0 : 1;
}
