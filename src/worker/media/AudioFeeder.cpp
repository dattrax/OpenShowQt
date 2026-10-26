#include "media/AudioFeeder.h"

#include <chrono>
#include <vector>

#include "media/FFmpeg.h"

namespace openshow::media {

namespace {
std::string ffError(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}
}  // namespace

AudioFileFeeder::~AudioFileFeeder() {
    requestStop();
    join();
    close();
}

bool AudioFileFeeder::open(const std::string& path, uint32_t targetRate, std::string* error) {
    auto fail = [&](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        close();
        return false;
    };
    if (targetRate == 0) {
        return fail("AudioFileFeeder needs a target sample rate");
    }

    AVFormatContext* format = nullptr;
    int err = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (err < 0) {
        return fail("cannot open '" + path + "': " + ffError(err));
    }
    err = avformat_find_stream_info(format, nullptr);
    if (err < 0) {
        avformat_close_input(&format);
        return fail("cannot read stream info from '" + path + "': " + ffError(err));
    }
    const int streamIndex =
        av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        avformat_close_input(&format);
        return fail("no audio stream in '" + path + "'");
    }
    const AVCodec* decoder =
        avcodec_find_decoder(format->streams[streamIndex]->codecpar->codec_id);
    if (decoder == nullptr) {
        avformat_close_input(&format);
        return fail("no decoder for the audio stream of '" + path + "'");
    }
    AVCodecContext* codec = avcodec_alloc_context3(decoder);
    err = avcodec_parameters_to_context(codec, format->streams[streamIndex]->codecpar);
    if (err < 0) {
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return fail("cannot copy audio codec parameters: " + ffError(err));
    }
    err = avcodec_open2(codec, decoder, nullptr);
    if (err < 0) {
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return fail("cannot open audio decoder: " + ffError(err));
    }

    fFormatContext = format;
    fCodecContext = codec;
    fAudioStreamIndex = streamIndex;
    fTargetRate = targetRate;
    {
        std::lock_guard<std::mutex> lock(fStatsMutex);
        fStats = Stats{};
    }
    fStopRequested = false;
    return true;
}

void AudioFileFeeder::close() {
    if (fCodecContext != nullptr) {
        AVCodecContext* codec = static_cast<AVCodecContext*>(fCodecContext);
        avcodec_free_context(&codec);
        fCodecContext = nullptr;
    }
    if (fFormatContext != nullptr) {
        AVFormatContext* format = static_cast<AVFormatContext*>(fFormatContext);
        avformat_close_input(&format);
        fFormatContext = nullptr;
    }
    fAudioStreamIndex = -1;
    fSeek.reset();
    fSeekError.clear();
    fLastSeekSeconds = 0.0;
    fSkipUntilSeconds = 0.0;
    fSeekedCallback = nullptr;
}

bool AudioFileFeeder::start(WriteFn write, bool loop, std::string* error) {
    if (fFormatContext == nullptr || fThreadWanted) {
        if (error) {
            *error = fFormatContext == nullptr ? "feeder is not open"
                                               : "feeder thread already started";
        }
        return false;
    }
    fWrite = std::move(write);
    fLoop = loop;
    fStopRequested = false;
    fThreadWanted = true;
    fThread = std::thread([this] { run(); });
    (void)error;
    return true;
}

void AudioFileFeeder::requestStop() {
    fStopRequested = true;
}

void AudioFileFeeder::requestSeek(double targetSeconds) {
    fSeek.request(targetSeconds);
}

bool AudioFileFeeder::waitSeekApplied(int timeoutMs) {
    // Waits for the newest request to reach a TERMINAL state and reports
    // which one (task 6.7d, review R19): a jump the container refused settles
    // as refused, so the caller learns the truth in milliseconds instead of
    // the whole timeout — and instead of the old "applied", which the
    // transport acted on by flushing queues and re-anchoring the clock on a
    // position the media never reached. The stop flag ends the wait as a
    // failure too: with the decode thread gone nothing can settle a request.
    return fSeek.waitAppliedLatest(timeoutMs, &fStopRequested);
}

bool AudioFileFeeder::applyPendingSeek(double targetSeconds) {
    auto* format = static_cast<AVFormatContext*>(fFormatContext);
    auto* codec = static_cast<AVCodecContext*>(fCodecContext);
    AVStream* stream = format->streams[fAudioStreamIndex];
    const auto pts = static_cast<int64_t>(
        targetSeconds * static_cast<double>(stream->time_base.den) /
        static_cast<double>(stream->time_base.num));
    // The 5.2 video pattern: BACKWARD lands on or before the target, so
    // decode time never starts ahead of it. The injected failure (set by the
    // worker from OPENSHOW_FAULT_AUDIO_SEEK_FAIL, task 6.7d) stands in for a
    // container that refuses the jump, which no well-formed fixture file can
    // produce on command.
    const bool seeked = !fFaultSeekFailure.load() &&
                        av_seek_frame(format, fAudioStreamIndex, pts, AVSEEK_FLAG_BACKWARD) >= 0;
    if (!seeked) {
        // Nothing moved, so NOTHING is flushed: the decoder, the resampler
        // and the downstream queues still describe the position the audience
        // is hearing, and dropping any of them would punch a hole of exactly
        // the jump that did not happen. No skip window, no seeked callback,
        // no last-seek stamp — what the transport reports as its position
        // stays true. Task 6.7d (review R19) is this branch's existence: the
        // caller must be able to see the refusal.
        std::lock_guard<std::mutex> lock(fStatsMutex);
        ++fStats.seekFailures;
        fSeekError = fFaultSeekFailure.load()
                         ? "injected audio seek failure (OPENSHOW_FAULT_AUDIO_SEEK_FAIL)"
                         : "the container refused the seek";
        // A skip window left over from an EARLIER jump would silence from
        // that old target onward, which is content the audience should hear:
        // no jump happened, so nothing is short of any target.
        fSkipUntilSeconds.store(0.0, std::memory_order_relaxed);
        return false;
    }
    avcodec_flush_buffers(codec);
    // The resampler's delay window belongs to the old position; rebuild it.
    swr_close(static_cast<SwrContext*>(fSwrContext));
    swr_init(static_cast<SwrContext*>(fSwrContext));
    // Only after the container actually moved does the skip window open.
    fSkipUntilSeconds.store(targetSeconds, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(fStatsMutex);
        ++fStats.seeks;
        fSeekError.clear();
    }
    fLastSeekSeconds.store(targetSeconds, std::memory_order_relaxed);
    // Race-free flush seam: the decode thread is quiet at this instant. Only
    // a jump that happened may flush what comes downstream.
    if (fSeekedCallback) {
        fSeekedCallback(targetSeconds);
    }
    return true;
}

std::string AudioFileFeeder::lastSeekError() const {
    std::lock_guard<std::mutex> lock(fStatsMutex);
    return fSeekError;
}

void AudioFileFeeder::join() {
    if (fThreadWanted && fThread.joinable()) {
        fThread.join();
    }
    fThreadWanted = false;
}

AudioFileFeeder::Stats AudioFileFeeder::stats() const {
    std::lock_guard<std::mutex> lock(fStatsMutex);
    return fStats;
}

bool AudioFileFeeder::rewindForLoop() {
    auto* format = static_cast<AVFormatContext*>(fFormatContext);
    auto* codec = static_cast<AVCodecContext*>(fCodecContext);
    if (avformat_seek_file(format, -1, INT64_MIN, 0, INT64_MAX, AVSEEK_FLAG_BACKWARD) < 0 &&
        av_seek_frame(format, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }
    avcodec_flush_buffers(codec);
    std::lock_guard<std::mutex> lock(fStatsMutex);
    ++fStats.loops;
    return true;
}

void AudioFileFeeder::run() {
    auto* format = static_cast<AVFormatContext*>(fFormatContext);
    auto* codec = static_cast<AVCodecContext*>(fCodecContext);
    auto noteError = [&]() {
        std::lock_guard<std::mutex> lock(fStatsMutex);
        ++fStats.decodeErrors;
    };

    SwrContext* swr = nullptr;
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, static_cast<int>(fTargetRate),
                            &codec->ch_layout,
                            static_cast<AVSampleFormat>(codec->sample_fmt), codec->sample_rate,
                            0, nullptr) < 0 ||
        swr_init(swr) < 0) {
        if (swr != nullptr) {
            swr_free(&swr);
        }
        std::fprintf(stderr, "openshow-worker: audio resampler setup failed\n");
        noteError();
        return;
    }
    fSwrContext = swr;

    // Output scratch for swr_convert: interleaved stereo F32.
    std::vector<uint8_t> outBytes;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    // Frame times are stream time_base; seek targets are media seconds.
    auto* aStream = format->streams[fAudioStreamIndex];
    const double timeBaseSeconds =
        static_cast<double>(aStream->time_base.den) > 0.0
            ? static_cast<double>(aStream->time_base.num) /
                  static_cast<double>(aStream->time_base.den)
            : 0.0;

    // Newest-request-wins seek application (the transport thread may have
    // moved on while the decode thread was mid-frame), paired with the
    // generation it belongs to (task 6.7d, review R19): only the generation
    // whose target this call actually applied is credited, a request that
    // arrived in the meantime stays pending for the next pass, and a refused
    // jump settles as REFUSED — its waiter stops waiting and learns the
    // truth, and the "a seek is queued" rule below releases either way,
    // because silence must never be the price of a jump that failed.
    auto seekIfPending = [&]() -> double {
        uint64_t generation = 0;
        double target = 0.0;
        if (!fSeek.pending(&generation, &target)) {
            return -1.0;
        }
        if (!applyPendingSeek(target)) {
            fSeek.refuse(generation);
            return -1.0;
        }
        fSeek.apply(generation);
        return target;
    };

    // Frames entirely at or before the seek target are resampled (to keep
    // swr's phase honest) but silenced instead of emitted. A frame with no
    // pts cannot be placed, so it plays.
    auto endsBeforeTarget = [&](const AVFrame* decoded) {
        const double until = fSkipUntilSeconds.load(std::memory_order_relaxed);
        if (until <= 0.0 || decoded->pts == AV_NOPTS_VALUE || timeBaseSeconds <= 0.0) {
            return false;
        }
        const double frameEnd =
            (static_cast<double>(decoded->pts) + static_cast<double>(decoded->nb_samples)) *
            timeBaseSeconds;
        return frameEnd <= until - 1e-4;
    };
    auto skipFrame = [&](const uint8_t** data, int samples) {
        outBytes.resize(static_cast<size_t>(samples) * 2u * sizeof(float));
        uint8_t* out = outBytes.data();
        swr_convert(swr, &out, samples, data, samples);  // keep the resampler in step
        std::lock_guard<std::mutex> lock(fStatsMutex);
        ++fStats.seekDiscards;
    };

    // Pump one batch of resampled frames through the write callback,
    // retrying (not blocking) while the sink ring is full. A pending seek
    // aborts the batch: without that, a full ring (the normal state of a
    // sprinting feeder) would delay the seek by however long the mixer
    // takes to drain it — seconds. Discarding a half-written batch here
    // is exactly right, because the seek's flush drops the queue anyway.
    auto seekPending = [&]() {
        // "Some request has not settled yet" — applied or refused, either
        // terminal state releases the emit retry (see seekIfPending).
        return fSeek.settledGeneration() < fSeek.requested();
    };
    auto emit = [&](const uint8_t* data, size_t frames) {
        size_t offset = 0;
        size_t fed = 0;
        while (offset < frames && !fStopRequested && !seekPending()) {
            const size_t taken = fWrite(reinterpret_cast<const float*>(data) + offset * 2u,
                                        frames - offset);
            if (taken == 0) {
                {
                    std::lock_guard<std::mutex> lock(fStatsMutex);
                    ++fStats.writeRetries;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            offset += taken;
            fed += taken;
        }
        if (fed != 0) {
            std::lock_guard<std::mutex> lock(fStatsMutex);
            fStats.framesFed += fed;
        }
    };

    // Resample one decoded frame (or drain, when inData == nullptr).
    auto resample = [&](const uint8_t** inData, int inSamples, size_t outCapacity) {
        outBytes.resize(outCapacity * 2u * sizeof(float));
        uint8_t* out = outBytes.data();
        const int got = swr_convert(swr, &out, static_cast<int>(outCapacity), inData, inSamples);
        if (got > 0) {
            emit(outBytes.data(), static_cast<size_t>(got));
        }
    };

    while (!fStopRequested) {
        const int err = av_read_frame(format, packet);
        if (err == AVERROR_EOF) {
            // Let the decoder empty first, then the resampler, then either
            // rewind (loop) or finish.
            avcodec_send_packet(codec, nullptr);
            while (!fStopRequested && avcodec_receive_frame(codec, frame) == 0) {
                seekIfPending();
                const uint8_t** data = const_cast<const uint8_t**>(frame->extended_data);
                if (endsBeforeTarget(frame)) {
                    skipFrame(data, frame->nb_samples);
                } else {
                    resample(data, frame->nb_samples,
                             static_cast<size_t>(frame->nb_samples) + 256);
                }
                av_frame_unref(frame);
            }
            if (!fStopRequested) {
                resample(nullptr, 0, 4096);
            }
            if (fLoop && !fStopRequested && rewindForLoop()) {
                continue;
            }
            std::lock_guard<std::mutex> lock(fStatsMutex);
            fStats.eof = true;
            break;
        }
        if (err < 0) {
            std::fprintf(stderr, "openshow-worker: audio read failed: %s\n", ffError(err).c_str());
            noteError();
            break;
        }
        if (packet->stream_index != fAudioStreamIndex) {
            av_packet_unref(packet);
            continue;
        }
        const int sendErr = avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
        if (sendErr < 0) {
            noteError();
            continue;
        }
        while (!fStopRequested && avcodec_receive_frame(codec, frame) == 0) {
            seekIfPending();
            // Headroom from the rate ratio plus the resampler's internal
            // delay window; +256 covers rounding on extreme ratios.
            const size_t capacity =
                static_cast<size_t>(av_rescale_q(frame->nb_samples, AVRational{1, codec->sample_rate},
                                                 AVRational{1, static_cast<int>(fTargetRate)})) +
                256;
            const uint8_t** data = const_cast<const uint8_t**>(frame->extended_data);
            if (endsBeforeTarget(frame)) {
                skipFrame(data, frame->nb_samples);
            } else {
                resample(data, frame->nb_samples, capacity);
            }
            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    fSwrContext = nullptr;
}

}  // namespace openshow::media
