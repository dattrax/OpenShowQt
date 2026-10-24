#include "media/VideoFrameSource.h"

#include <cstdio>

#include "media/FFmpeg.h"

namespace openshow::media {

namespace {
std::string ffError(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// get_format hook for the VA-API hwaccel: take surface format if the
// decoder offers it, otherwise let FFmpeg pick as usual (software). The
// owner rides in AVCodecContext.opaque so the outcome is logged once —
// "hardware configured" only truly means hardware once a frame negotiates.
enum AVPixelFormat negotiatePixelFormat(AVCodecContext* avctx,
                                       const enum AVPixelFormat* formats) {
    const char* decoderName = avctx->codec ? avctx->codec->name : "";
    for (const enum AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_VAAPI) {
            if (auto* self = static_cast<VideoFrameSource*>(avctx->opaque)) {
                self->noteFormatNegotiated(decoderName, true);
            }
            return *p;
        }
    }
    if (auto* self = static_cast<VideoFrameSource*>(avctx->opaque)) {
        self->noteFormatNegotiated(decoderName, false);
    }
    return avcodec_default_get_format(avctx, formats);
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
    if (fHardwareDecodeEnabled && codec) {
        // VA-API hwaccels live on FFmpeg's native decoders; external
        // wrapper decoders (this build resolves AV1 to libdav1d) ignore the
        // hwaccel mechanism entirely and decode in software. Do what the
        // ffmpeg CLI does for -hwaccel vaapi: prefer the decoder registered
        // under the canonical codec name, which is the native one. Without
        // hardware decoding the default stays as-is (libdav1d beats the
        // native AV1 decoder on the CPU).
        if (const AVCodec* native =
                avcodec_find_decoder_by_name(avcodec_get_name(stream->codecpar->codec_id))) {
            codec = native;
        }
    }

    // Task 1.5h: prefer hardware decode. VA-API registers no standalone
    // decoders (its "*_vaapi" codecs are encode-only), so GPU decode is a
    // hwaccel attached to the normal decoder: an AV_HWDEVICE_TYPE_VAAPI
    // device context plus a get_format hook that picks AV_PIX_FMT_VAAPI
    // when the decoder offers it. Frames then come back as GPU surfaces
    // the decode thread copies to system memory before the ring — the win
    // is the decode work itself leaving the CPU. A machine without a VA
    // driver (or a stream the driver cannot accelerate) decodes in
    // software exactly as before; the effective path is logged once, at
    // open when known and otherwise when the first frame negotiates.
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    bool hardwareConfigured = false;
    std::string hardwareNote;
    if (!decoder || avcodec_parameters_to_context(decoder, stream->codecpar) < 0) {
        if (decoder) {
            avcodec_free_context(&decoder);
        }
        avformat_close_input(&format);
        return fail("cannot open the " + best->codec + " decoder");
    }
    if (fHardwareDecodeEnabled) {
        AVBufferRef* hwDevice = nullptr;
        const int err = av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_VAAPI, nullptr,
                                               nullptr, 0);
        if (err < 0) {
            hardwareNote = "VA-API device unavailable: " + ffError(err);
        } else {
            fHwDeviceCtx = hwDevice;  // held for the source's lifetime too
            decoder->hw_device_ctx = av_buffer_ref(hwDevice);
            decoder->opaque = this;
            decoder->get_format = &negotiatePixelFormat;
            hardwareConfigured = true;
        }
    }
    if (avcodec_open2(decoder, codec, nullptr) < 0) {
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return fail("cannot open the " + best->codec + " decoder");
    }
    if (hardwareConfigured) {
        fDecodeDescription = best->codec + " (VA-API hardware decode)";  // definitive once frames flow
    } else {
        fDecodeDescription =
            best->codec + " (software decode" + (hardwareNote.empty() ? "" : "; " + hardwareNote) +
            ")";
        std::fprintf(stderr, "openshow-worker: video decode: %s\n", fDecodeDescription.c_str());
    }

    // The ring lives at the decoder's visible size; mid-stream resolution
    // changes are out of scope for 1.5 (the source reports, it does not
    // adapt). Any pixel format — yuv420p direct, another software format,
    // or NV12/P010 transferred back from a VAAPI surface — is converted to
    // I420 by a swscale context the decode thread builds lazily from the
    // frames as they arrive, because only then is the real format known.
    if (!fRing.create(ringDepth, decoder->width, decoder->height, error)) {
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return false;
    }

    fPath = path;
    fStreamInfo = *best;
    fVideoStreamIndex = best->index;
    fFormatContext = format;
    fCodecContext = decoder;
    fNegotiatedLogged = false;
    fTimeBaseSeconds = av_q2d(stream->time_base);
    fSyntheticStepSeconds = 1.0 / (best->frameRate > 0.0 ? best->frameRate : 25.0);
    fSyntheticPtsSeconds = 0.0;
    return true;
}

void VideoFrameSource::close() {
    if (fCodecContext) {
        AVCodecContext* decoder = static_cast<AVCodecContext*>(fCodecContext);
        avcodec_free_context(&decoder);
        fCodecContext = nullptr;
    }
    if (fHwDeviceCtx) {
        AVBufferRef* device = static_cast<AVBufferRef*>(fHwDeviceCtx);
        av_buffer_unref(&device);
        fHwDeviceCtx = nullptr;
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
    fDecodeDescription.clear();
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

void VideoFrameSource::noteFormatNegotiated(const char* decoderName, bool hardware) {
    // Called from the get_format hook on the decode thread, once at the
    // start of decode (and again if a mid-stream reinitialisation ever
    // negotiates afresh; the log stays one-shot either way).
    if (fNegotiatedLogged.exchange(true)) {
        return;
    }
    fDecodeDescription =
        std::string(decoderName) +
        (hardware ? " (VA-API hardware decode)" : " (software decode; no VAAPI format offered)");
    std::fprintf(stderr, "openshow-worker: video decode: %s\n", fDecodeDescription.c_str());
}

void VideoFrameSource::run() {
    AVFormatContext* format = static_cast<AVFormatContext*>(fFormatContext);
    AVCodecContext* decoder = static_cast<AVCodecContext*>(fCodecContext);
    AVFrame* frame = av_frame_alloc();
    // Scratch frame for av_hwframe_transfer_data(): brings a VAAPI surface
    // back to system memory as NV12 (8-bit streams) or P010 (10-bit).
    AVFrame* transfer = av_frame_alloc();
    // The I420 converter, built lazily because only arriving frames reveal
    // the real format (hardware negotiation, or a non-yuv420p software
    // stream); rebuilt once should the format ever change mid-stream.
    SwsContext* convert = nullptr;
    AVPixelFormat convertFrom = AV_PIX_FMT_NONE;

    auto bump = [&](auto&& update) {
        std::lock_guard lock(fStatsMutex);
        update(fStats);
    };

    // Returns false only when the ring refused a frame (stop) — every other
    // problem is counted and decoding continues.
    auto emitFrame = [&](AVFrame* decoded) -> bool {
        // A VAAPI surface must be copied back to system memory before the
        // ring can hold it; the PTS stays with the original frame because
        // the transfer copies pixels only.
        AVFrame* pixels = decoded;
        if (decoded->format == AV_PIX_FMT_VAAPI) {
            const int err = av_hwframe_transfer_data(transfer, decoded, 0);
            if (err < 0) {
                av_frame_unref(transfer);
                bump([](Stats& s) { s.decodeErrors++; });
                return true;
            }
            pixels = transfer;
        }
        VideoFrame out;
        if (!fRing.claim(&out)) {
            av_frame_unref(transfer);
            return false;
        }
        const auto pixelFormat = static_cast<AVPixelFormat>(pixels->format);
        if (pixelFormat == AV_PIX_FMT_YUV420P) {
            av_image_copy(out.data, out.linesize, const_cast<const uint8_t**>(pixels->data),
                          pixels->linesize, AV_PIX_FMT_YUV420P, out.width, out.height);
        } else {
            if (convert && convertFrom != pixelFormat) {
                sws_freeContext(convert);
                convert = nullptr;
            }
            if (!convert) {
                convert = sws_getContext(pixels->width, pixels->height, pixelFormat, out.width,
                                         out.height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                                         nullptr, nullptr);
                convertFrom = pixelFormat;
                if (convert) {
                    std::fprintf(stderr, "openshow-worker: video decode: converting %s to I420\n",
                                 av_get_pix_fmt_name(pixelFormat));
                }
            }
            if (!convert) {
                fRing.abortClaim(out.slot);
                bump([](Stats& s) { s.decodeErrors++; });
                av_frame_unref(transfer);
                return true;
            }
            sws_scale(convert, const_cast<const uint8_t**>(pixels->data), pixels->linesize, 0,
                      pixels->height, out.data, out.linesize);
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
        av_frame_unref(transfer);
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
    av_frame_free(&transfer);
    if (convert) {
        sws_freeContext(convert);
    }
    // Whatever the exit reason, a stopped ring lets the consumer drain the
    // remaining frames and then see poll() return false.
    fRing.stop();
}

}  // namespace openshow::media
