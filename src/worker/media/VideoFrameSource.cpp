#include "media/VideoFrameSource.h"

#include "media/FFmpeg.h"

namespace openshow::media {

namespace {
std::string ffError(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}
}  // namespace

VideoFrameSource::~VideoFrameSource() {
    requestStop();
    join();
    close();
}

bool VideoFrameSource::open(const std::string& path, unsigned ringDepth, std::string* error) {
    auto fail = [&](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        close();
        return false;
    };

    MediaInfo info;
    if (probeMedia(path, &info, error) != ProbeStatus::kOk) {
        return false;
    }
    const VideoStreamInfo* best = nullptr;
    for (const auto& video : info.videoStreams) {
        if (!video.decoderAvailable) {
            continue;
        }
        const long long area = static_cast<long long>(video.width) * video.height;
        const long long bestArea = best ? static_cast<long long>(best->width) * best->height : -1;
        if (area > bestArea) {
            best = &video;
        }
    }
    if (!best) {
        return fail("no decodable video stream in '" + path + "'");
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
    if (static_cast<unsigned>(best->index) >= format->nb_streams ||
        format->streams[best->index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        avformat_close_input(&format);
        return fail("video stream changed between probe and open");
    }

    const AVStream* stream = format->streams[best->index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    if (!decoder || avcodec_parameters_to_context(decoder, stream->codecpar) < 0 ||
        avcodec_open2(decoder, codec, nullptr) < 0) {
        if (decoder) {
            avcodec_free_context(&decoder);
        }
        avformat_close_input(&format);
        return fail("cannot open the " + best->codec + " decoder");
    }

    // The ring lives at the decoder's visible size; mid-stream resolution
    // changes are out of scope for 1.5 (the source reports, it does not
    // adapt). I420 in, I420 out — conversion only when the stream differs.
    if (!fRing.create(ringDepth, decoder->width, decoder->height, error)) {
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return false;
    }
    if (decoder->pix_fmt != AV_PIX_FMT_YUV420P) {
        fSwsContext = sws_getContext(decoder->width, decoder->height, decoder->pix_fmt,
                                     decoder->width, decoder->height, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!fSwsContext) {
            avcodec_free_context(&decoder);
            avformat_close_input(&format);
            return fail("cannot create the conversion to I420");
        }
    }

    fPath = path;
    fStreamInfo = *best;
    fVideoStreamIndex = best->index;
    fFormatContext = format;
    fCodecContext = decoder;
    fTimeBaseSeconds = av_q2d(stream->time_base);
    fSyntheticStepSeconds = 1.0 / (best->frameRate > 0.0 ? best->frameRate : 25.0);
    fSyntheticPtsSeconds = 0.0;
    return true;
}

void VideoFrameSource::close() {
    if (fSwsContext) {
        sws_freeContext(static_cast<SwsContext*>(fSwsContext));
        fSwsContext = nullptr;
    }
    if (fCodecContext) {
        AVCodecContext* decoder = static_cast<AVCodecContext*>(fCodecContext);
        avcodec_free_context(&decoder);
        fCodecContext = nullptr;
    }
    if (fFormatContext) {
        AVFormatContext* format = static_cast<AVFormatContext*>(fFormatContext);
        avformat_close_input(&format);
        fFormatContext = nullptr;
    }
    fRing.destroy();
    fVideoStreamIndex = -1;
    fStreamInfo = {};
    fPath.clear();
    std::lock_guard lock(fStatsMutex);
    fStats = {};
    fThreadWanted = false;
    fLoop = false;
}

bool VideoFrameSource::startDecode(bool loop, std::string* error) {
    if (!fFormatContext || !fCodecContext) {
        if (error) {
            *error = "VideoFrameSource is not open";
        }
        return false;
    }
    if (fThread.joinable()) {
        if (error) {
            *error = "VideoFrameSource already decoding (reopen to restart)";
        }
        return false;
    }
    fLoop = loop;
    fThreadWanted = true;
    fThread = std::thread(&VideoFrameSource::run, this);
    return true;
}

void VideoFrameSource::requestStop() {
    fThreadWanted = false;
    // Wakes a decode thread parked on a full ring, and lets a consumer's
    // poll() drain what is visible and then fail fast.
    fRing.stop();
}

void VideoFrameSource::join() {
    if (fThread.joinable()) {
        fThread.join();
    }
}

VideoFrameSource::Stats VideoFrameSource::stats() const {
    std::lock_guard lock(fStatsMutex);
    return fStats;
}

void VideoFrameSource::run() {
    AVFormatContext* format = static_cast<AVFormatContext*>(fFormatContext);
    AVCodecContext* decoder = static_cast<AVCodecContext*>(fCodecContext);
    SwsContext* sws = static_cast<SwsContext*>(fSwsContext);
    AVFrame* frame = av_frame_alloc();

    auto bump = [&](auto&& update) {
        std::lock_guard lock(fStatsMutex);
        update(fStats);
    };

    // Returns false only when the ring refused a frame (stop) — every other
    // problem is counted and decoding continues.
    auto emitFrame = [&](AVFrame* decoded) -> bool {
        VideoFrame out;
        if (!fRing.claim(&out)) {
            return false;
        }
        if (decoded->format == AV_PIX_FMT_YUV420P) {
            av_image_copy(out.data, out.linesize,
                          const_cast<const uint8_t**>(decoded->data), decoded->linesize,
                          AV_PIX_FMT_YUV420P, out.width, out.height);
        } else if (sws) {
            sws_scale(sws, const_cast<const uint8_t**>(decoded->data), decoded->linesize, 0,
                      out.height, out.data, out.linesize);
        } else {
            fRing.abortClaim(out.slot);
            bump([](Stats& s) { s.decodeErrors++; });
            return true;
        }
        const int64_t pts = decoded->best_effort_timestamp != AV_NOPTS_VALUE
                                ? decoded->best_effort_timestamp
                                : decoded->pts;
        out.ptsSeconds =
            pts != AV_NOPTS_VALUE ? static_cast<double>(pts) * fTimeBaseSeconds
                                  : fSyntheticPtsSeconds;
        fSyntheticPtsSeconds = out.ptsSeconds + fSyntheticStepSeconds;
        fRing.publish(out);
        bump([](Stats& s) { s.framesProduced++; });
        return true;
    };

    auto drain = [&](bool flushDecoder) -> bool {
        if (flushDecoder) {
            avcodec_send_packet(decoder, nullptr);
        }
        for (;;) {
            const int r = avcodec_receive_frame(decoder, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                return true;
            }
            if (r < 0) {
                bump([](Stats& s) { s.decodeErrors++; });
                return true;
            }
            const bool keepGoing = emitFrame(frame);
            av_frame_unref(frame);
            if (!keepGoing) {
                return false;
            }
        }
    };

    while (fThreadWanted.load()) {
        AVPacket* packet = av_packet_alloc();
        const int readResult = av_read_frame(format, packet);
        if (readResult >= 0) {
            bump([&](Stats& s) { s.packetsRead++; });
            if (packet->stream_index == fVideoStreamIndex) {
                const int sendResult = avcodec_send_packet(decoder, packet);
                av_packet_free(&packet);
                if (sendResult == 0) {
                    if (!drain(/*flushDecoder=*/false)) {
                        break;
                    }
                } else {
                    bump([](Stats& s) { s.decodeErrors++; });
                }
            } else {
                av_packet_free(&packet);
            }
            continue;
        }

        av_packet_free(&packet);
        if (readResult == AVERROR_EOF) {
            if (!drain(/*flushDecoder=*/true)) {
                break;
            }
            if (fLoop.load()) {
                if (av_seek_frame(format, fVideoStreamIndex, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
                    avcodec_flush_buffers(decoder);
                    fSyntheticPtsSeconds = 0.0;
                    bump([](Stats& s) { s.loops++; });
                    continue;
                }
            }
            bump([](Stats& s) { s.eof = true; });
            break;
        }
        bump([](Stats& s) { s.decodeErrors++; });
        break;  // a read failure is terminal; the caller sees decodeErrors
    }

    av_frame_free(&frame);
    // Whatever the exit reason, a stopped ring lets the consumer drain the
    // remaining frames and then see poll() return false.
    fRing.stop();
}

}  // namespace openshow::media
