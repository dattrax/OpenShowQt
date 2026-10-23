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

    // Output scratch for swr_convert: interleaved stereo F32.
    std::vector<uint8_t> outBytes;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    // Pump one batch of resampled frames through the write callback,
    // retrying (not blocking) while the sink ring is full.
    auto emit = [&](const uint8_t* data, size_t frames) {
        size_t offset = 0;
        size_t fed = 0;
        while (offset < frames && !fStopRequested) {
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
                const uint8_t** data = const_cast<const uint8_t**>(frame->extended_data);
                resample(data, frame->nb_samples, static_cast<size_t>(frame->nb_samples) + 256);
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
            // Headroom from the rate ratio plus the resampler's internal
            // delay window; +256 covers rounding on extreme ratios.
            const size_t capacity =
                static_cast<size_t>(av_rescale_q(frame->nb_samples, AVRational{1, codec->sample_rate},
                                                 AVRational{1, static_cast<int>(fTargetRate)})) +
                256;
            const uint8_t** data = const_cast<const uint8_t**>(frame->extended_data);
            resample(data, frame->nb_samples, capacity);
            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
}

}  // namespace openshow::media
