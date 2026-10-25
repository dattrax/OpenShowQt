#include "media/AudioMixer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace openshow::media {

namespace {
// Gain changes interpolate over this long: audible clicks need a step,
// and the largest per-sample jump this ramp allows is block-fraction
// small.
constexpr double kRampSeconds = 0.05;
}  // namespace

AudioMixer::AudioMixer(size_t inputCapacityFrames) : fInputCapacity(inputCapacityFrames) {}

AudioMixer::~AudioMixer() {
    requestStop();
    join();
}

int AudioMixer::addInput() {
    if (fStarted.load() || fInputCount >= kMaxInputs) {
        return kNoInput;
    }
    auto input = std::make_unique<Input>();
    if (!input->ring.create(fInputCapacity)) {
        return kNoInput;
    }
    fInputs.push_back(std::move(input));
    return static_cast<int>(fInputCount++);
}

void AudioMixer::setVolume(int input, float volume) {
    if (input < 0 || input >= static_cast<int>(fInputCount)) {
        return;
    }
    fInputs[input]->volume.store(std::clamp(volume, 0.0f, 1.0f));
}

float AudioMixer::volume(int input) const {
    if (input < 0 || input >= static_cast<int>(fInputCount)) {
        return 0.0f;
    }
    return fInputs[input]->volume.load();
}

void AudioMixer::setMute(int input, bool muted) {
    if (input < 0 || input >= static_cast<int>(fInputCount)) {
        return;
    }
    fInputs[input]->mute.store(muted);
}

bool AudioMixer::muted(int input) const {
    if (input < 0 || input >= static_cast<int>(fInputCount)) {
        return true;
    }
    return fInputs[input]->mute.load();
}

size_t AudioMixer::write(int input, const float* interleavedStereo, size_t frames) {
    if (input < 0 || input >= static_cast<int>(fInputCount)) {
        return 0;
    }
    const size_t taken = fInputs[input]->ring.write(interleavedStereo, frames);
    if (taken != 0) {
        fInputs[input]->framesWritten.fetch_add(taken, std::memory_order_relaxed);
    }
    return taken;
}

bool AudioMixer::start(WriteFn out, uint32_t rate, std::string* error) {
    if (!out) {
        if (error) *error = "AudioMixer needs a write callback";
        return false;
    }
    if (rate == 0) {
        if (error) *error = "AudioMixer needs a sample rate";
        return false;
    }
    bool expected = false;
    if (!fStarted.compare_exchange_strong(expected, true)) {
        if (error) *error = "AudioMixer is already running";
        return false;
    }
    fOut = std::move(out);
    fRate = rate;
    fStop.store(false);
    fThread = std::thread([this] { run(); });
    return true;
}

void AudioMixer::requestStop() {
    fStop.store(true);
}

void AudioMixer::join() {
    if (fThread.joinable()) {
        fThread.join();
    }
}

void AudioMixer::run() {
    // One 10 ms block per iteration at the negotiated rate; the scratch
    // buffers and the per-input ramp state live only here — the mix
    // thread owns them for the whole run.
    const size_t blockFrames = std::max<size_t>(1, fRate / 100);
    std::vector<float> readScratch(blockFrames * 2);
    std::vector<float> outScratch(blockFrames * 2);
    std::vector<float> gain(fInputs.size(), -1.0f);  // -1: not yet settled
    const double maxStepPerBlock =
        static_cast<double>(blockFrames) / (kRampSeconds * static_cast<double>(fRate));

    while (!fStop.load()) {
        std::memset(outScratch.data(), 0, outScratch.size() * sizeof(float));
        for (size_t i = 0; i < fInputs.size(); ++i) {
            Input& input = *fInputs[i];
            const float target = input.mute.load(std::memory_order_relaxed)
                                     ? 0.0f
                                     : input.volume.load(std::memory_order_relaxed);
            if (gain[i] < 0.0f) {
                gain[i] = target;  // first block: start at the set level
            }
            const size_t filled = input.ring.read(readScratch.data(), blockFrames);
            if (filled < blockFrames) {
                std::memset(readScratch.data() + filled * 2, 0,
                            (blockFrames - filled) * 2 * sizeof(float));
                input.starvedBlocks.fetch_add(1, std::memory_order_relaxed);
            }
            input.framesMixed.fetch_add(filled, std::memory_order_relaxed);

            // Ramp at most maxStepPerBlock toward the target this block,
            // interpolating per frame so block boundaries stay smooth.
            const float delta = std::clamp(target - gain[i], -static_cast<float>(maxStepPerBlock),
                                           static_cast<float>(maxStepPerBlock));
            if (delta == 0.0f) {
                if (gain[i] == 1.0f) {
                    for (size_t f = 0; f < blockFrames; ++f) {
                        outScratch[f * 2] += readScratch[f * 2];
                        outScratch[f * 2 + 1] += readScratch[f * 2 + 1];
                    }
                } else if (gain[i] != 0.0f) {
                    const float g = gain[i];
                    for (size_t f = 0; f < blockFrames; ++f) {
                        outScratch[f * 2] += g * readScratch[f * 2];
                        outScratch[f * 2 + 1] += g * readScratch[f * 2 + 1];
                    }
                }
            } else {
                const float from = gain[i];
                const float to = gain[i] + delta;
                for (size_t f = 0; f < blockFrames; ++f) {
                    const float g = from + (to - from) * (static_cast<float>(f) + 1.0f) /
                                                static_cast<float>(blockFrames);
                    outScratch[f * 2] += g * readScratch[f * 2];
                    outScratch[f * 2 + 1] += g * readScratch[f * 2 + 1];
                }
                gain[i] = to;
            }
        }

        // Deliver the whole block; a full output ring is the mixer's
        // pacing (same retry discipline as the feeder), and stop cuts
        // the retries short.
        size_t written = 0;
        while (!fStop.load() && written < blockFrames) {
            const size_t taken = fOut(outScratch.data() + written * 2, blockFrames - written);
            written += taken;
            if (taken == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (fStop.load()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(fStatsMutex);
            ++fBlocks;
            fDelivered += written;
            if (written < blockFrames) {
                ++fWriteRetries;
            }
        }
    }
}

AudioMixer::Stats AudioMixer::stats() const {
    Stats out;
    out.inputs = fInputCount;
    for (unsigned i = 0; i < fInputCount; ++i) {
        out.input[i].framesWritten = fInputs[i]->framesWritten.load(std::memory_order_relaxed);
        out.input[i].framesMixed = fInputs[i]->framesMixed.load(std::memory_order_relaxed);
        out.input[i].starvedBlocks = fInputs[i]->starvedBlocks.load(std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(fStatsMutex);
    out.blocks = fBlocks;
    out.deliveredFrames = fDelivered;
    out.writeRetries = fWriteRetries;
    return out;
}

}  // namespace openshow::media
