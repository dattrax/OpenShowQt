#include "media/VideoFrameSource.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

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

namespace {
// The ring's two-plane format follows the stream's bit depth (task 1.5i):
// a 10-bit-or-deeper stream downloads as P010 from VA-API and decodes as
// p010le in software, so both paths write the slot layout without any
// colour conversion; everything else takes NV12. Format names the probe
// cannot map take the 8-bit path — the decode thread's swscale fallback
// still converts odd arriving formats correctly into it.
FrameFormat deriveRingFormat(const std::string& pixelFormat) {
    const AVPixelFormat probed = av_get_pix_fmt(pixelFormat.c_str());
    if (probed != AV_PIX_FMT_NONE) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(probed);
        if (desc && desc->comp[0].depth >= 10) {
            return FrameFormat::kP010;
        }
    }
    return FrameFormat::kNV12;
}
}  // namespace

bool VideoFrameSource::open(const std::string& path, unsigned ringDepth, std::string* error) {
    if (!openInternal(path, error)) {
        return false;
    }
    if (!createRing(ringDepth, nullptr, error)) {
        close();
        return false;
    }
    return true;
}

bool VideoFrameSource::open(const std::string& path, std::string* error) {
    return openInternal(path, error);
}

bool VideoFrameSource::createRing(unsigned ringDepth, FrameBacking* backing,
                                  std::string* error) {
    if (!fFormatContext || !fCodecContext) {
        if (error) {
            *error = "VideoFrameSource is not open";
        }
        return false;
    }
    // The ring lives at the decoder's coded size; mid-stream resolution
    // changes are out of scope for 1.5 (the source reports, it does not
    // adapt). The arriving pixel format is either the slot's NV12/P010
    // layout already (hardware download, yuv420p/p010le software) or is
    // converted into it by a swscale context the decode thread builds
    // lazily — only arriving frames reveal the real format.
    return fRing.create(ringDepth, fFrameWidth, fFrameHeight, fRingFormat, backing, error);
}

bool VideoFrameSource::openInternal(const std::string& path, std::string* error) {
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

    // Task 1.5i: record the coded size and the ring's two-plane format here
    // so the (optionally deferred) createRing() and a GPU backing's create()
    // agree without any FFmpeg types crossing the header. Any arriving pixel
    // format — yuv420p or p010le direct, another software format, or
    // NV12/P010 transferred back from a VAAPI surface — lands in this layout,
    // converted only when it differs.
    fFrameWidth = decoder->width;
    fFrameHeight = decoder->height;
    fRingFormat = deriveRingFormat(best->pixelFormat);

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
    fFrameWidth = fFrameHeight = 0;
    fRingFormat = FrameFormat::kNV12;
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
    // Destination for av_hwframe_transfer_data(). Task 1.5i: the frame is
    // normally filled with the claimed ring slot's plane pointers so the
    // hardware download lands directly in the slot's (GPU-visible) memory —
    // FFmpeg writes through caller buffers whenever dst->buf[0] is set, and
    // the little buffer below is that non-owning ticket (the ring/backing
    // owns the pixels). When a stream's download format disagrees with the
    // ring format the frame stays clean, FFmpeg allocates its own scratch,
    // and the swscale fallback converts into the slot.
    AVFrame* transfer = av_frame_alloc();
    AVBufferRef* bufferTicket = av_buffer_alloc(1);
    // The slot-format converter, built lazily because only arriving frames
    // reveal the real format (hardware negotiation, or a stream whose
    // software output is neither yuv420p/p010le nor the slot layout);
    // rebuilt once should the format ever change mid-stream.
    SwsContext* convert = nullptr;
    AVPixelFormat convertFrom = AV_PIX_FMT_NONE;

    const AVPixelFormat ringFormat =
        fRingFormat == FrameFormat::kP010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    const int componentBytes = fRingFormat == FrameFormat::kP010 ? 2 : 1;

    auto bump = [&](auto&& update) {
        std::lock_guard lock(fStatsMutex);
        update(fStats);
    };

    auto copyPlane = [](uint8_t* dst, int dstLinesize, const uint8_t* src, int srcLinesize,
                        int rowBytes, int rows) {
        for (int row = 0; row < rows; ++row) {
            std::memcpy(dst + static_cast<size_t>(row) * dstLinesize,
                        src + static_cast<size_t>(row) * srcLinesize,
                        static_cast<size_t>(rowBytes));
        }
    };

    // The general path: whatever arrived becomes the ring's two-plane
    // layout through a lazily built swscale context.
    auto toRingWithSws = [&](const AVFrame* pixels, const VideoFrame& out) -> bool {
        const auto pixelFormat = static_cast<AVPixelFormat>(pixels->format);
        if (convert && convertFrom != pixelFormat) {
            sws_freeContext(convert);
            convert = nullptr;
        }
        if (!convert) {
            convert = sws_getContext(pixels->width, pixels->height, pixelFormat, out.width,
                                     out.height, ringFormat, SWS_BILINEAR, nullptr, nullptr,
                                     nullptr);
            convertFrom = pixelFormat;
            if (convert) {
                std::fprintf(stderr, "openshow-worker: video decode: converting %s to %s\n",
                             av_get_pix_fmt_name(pixelFormat), av_get_pix_fmt_name(ringFormat));
            }
        }
        return convert != nullptr &&
               sws_scale(convert, const_cast<const uint8_t**>(pixels->data), pixels->linesize, 0,
                         pixels->height, out.data, out.linesize) > 0;
    };

    // Returns false only when the ring refused a frame (stop) — every other
    // problem is counted and decoding continues.
    auto emitFrame = [&](AVFrame* decoded) -> bool {
        // Claim first: with the slot's planes as the destination there is
        // no intermediate frame between the decoder and the ring.
        VideoFrame out;
        if (!fRing.claim(&out)) {
            return false;
        }
        bool pixelsReady = false;
        if (decoded->format == AV_PIX_FMT_VAAPI) {
            // A VAAPI surface must be copied back to system memory; the
            // PTS stays with the original frame because the transfer
            // copies pixels only. When the download format is the ring
            // format (the norm — both follow the stream's depth) the
            // transfer writes the slot's mapped planes directly.
            const AVHWFramesContext* surfaces =
                decoded->hw_frames_ctx
                    ? reinterpret_cast<const AVHWFramesContext*>(decoded->hw_frames_ctx->data)
                    : nullptr;
            if (surfaces && surfaces->sw_format == ringFormat) {
                av_frame_unref(transfer);
                transfer->format = ringFormat;
                transfer->width = out.width;
                transfer->height = out.height;
                std::copy(std::begin(out.data), std::end(out.data), transfer->data);
                std::copy(std::begin(out.linesize), std::end(out.linesize), transfer->linesize);
                transfer->buf[0] = av_buffer_ref(bufferTicket);
                const int err = av_hwframe_transfer_data(transfer, decoded, 0);
                av_frame_unref(transfer);  // drops the alias and the ticket ref
                pixelsReady = err >= 0;
            } else {
                // Probe and download format disagree (rare): allocate
                // FFmpeg's own scratch and convert through swscale.
                av_frame_unref(transfer);
                const int err = av_hwframe_transfer_data(transfer, decoded, 0);
                if (err >= 0) {
                    pixelsReady = toRingWithSws(transfer, out);
                }
                av_frame_unref(transfer);
            }
        } else if (decoded->format == AV_PIX_FMT_YUV420P && ringFormat == AV_PIX_FMT_NV12) {
            // Planar 8-bit is the software decoder's native output: luma
            // rows copy verbatim, U and V interleave into the slot's
            // chroma plane on the way (the old uvScratch fold, now landing
            // directly in the destination).
            copyPlane(out.data[0], out.linesize[0], decoded->data[0], decoded->linesize[0],
                      out.width, out.height);
            for (int row = 0; row < out.height / 2; ++row) {
                const uint8_t* uRow =
                    decoded->data[1] + static_cast<size_t>(row) * decoded->linesize[1];
                const uint8_t* vRow =
                    decoded->data[2] + static_cast<size_t>(row) * decoded->linesize[2];
                uint8_t* uvRow = out.data[1] + static_cast<size_t>(row) * out.linesize[1];
                for (int col = 0; col < out.width / 2; ++col) {
                    uvRow[col * 2] = uRow[col];
                    uvRow[col * 2 + 1] = vRow[col];
                }
            }
            pixelsReady = true;
        } else if ((decoded->format == AV_PIX_FMT_NV12 && ringFormat == AV_PIX_FMT_NV12) ||
                   (decoded->format == AV_PIX_FMT_P010LE && ringFormat == AV_PIX_FMT_P010LE)) {
            // Already the slot's layout (10-bit software decode lands
            // here): plain plane copies, chroma row = width/2 pairs of
            // two components = width * componentBytes bytes.
            const int rowBytes = out.width * componentBytes;
            copyPlane(out.data[0], out.linesize[0], decoded->data[0], decoded->linesize[0],
                      rowBytes, out.height);
            copyPlane(out.data[1], out.linesize[1], decoded->data[1], decoded->linesize[1],
                      rowBytes, out.height / 2);
            pixelsReady = true;
        } else {
            pixelsReady = toRingWithSws(decoded, out);
        }
        if (!pixelsReady) {
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
        if (!fRing.publish(out)) {
            // The backing's commit refused the slot. Teardown releases a
            // parked commit through requestShutdown() — that abort is the
            // producer unwinding, not a decode error; only a GPU-side
            // failure while the ring still runs counts.
            if (!fRing.stopped()) {
                bump([](Stats& s) { s.decodeErrors++; });
            }
            return true;
        }
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
    av_frame_free(&transfer);
    av_buffer_unref(&bufferTicket);
    if (convert) {
        sws_freeContext(convert);
    }
    // Whatever the exit reason, a stopped ring lets the consumer drain the
    // remaining frames and then see poll() return false.
    fRing.stop();
}

}  // namespace openshow::media
