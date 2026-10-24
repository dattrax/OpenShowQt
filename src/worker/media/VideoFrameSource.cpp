#include "media/VideoFrameSource.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

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
int directBuffer(AVCodecContext* codec, AVFrame* frame, int) {
    try {
        return static_cast<VideoFrameSource*>(codec->opaque)->allocateDecoderBuffer(codec, frame);
    } catch (...) {
        return AVERROR(ENOMEM); // no C++ exception may escape into FFmpeg
    }
}
AVPixelFormat softwareFormat(AVCodecContext*, const AVPixelFormat* formats) {
    for (const auto* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const auto* desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}
}  // namespace

VideoFrameSource::~VideoFrameSource() {
    close();
}

namespace {
// Compatibility output follows the probed depth. Software decoders often
// return planar 10-bit, which still needs conversion to P010 in that mode.
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
    fDirectDecode = false; // CPU-only convenience API explicitly uses copying storage
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
    fBacking = backing;
    if (fDirectDecode && !backing) {
        if (error) *error = "direct decoding requires a GPU frame backing";
        return false;
    }
    if (fDirectDecode && !primeDirect(error)) return false;
    return fRing.create(ringDepth, fFrameWidth, fFrameHeight, fRingFormat, backing, error,
                        fDirectDecode);
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
    if (!fDirectDecode && fHardwareDecodeEnabled && codec) {
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

    // Compatibility mode may prefer hardware decode. VA-API registers no standalone
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
    if (!fDirectDecode && fHardwareDecodeEnabled) {
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
    if (fDirectDecode) {
        const AVPixelFormat native = av_get_pix_fmt(best->pixelFormat.c_str());
        if (!codec || !(codec->capabilities & AV_CODEC_CAP_DR1) ||
            (native != AV_PIX_FMT_YUV420P && native != AV_PIX_FMT_NV12 &&
             native != AV_PIX_FMT_P010LE)) {
            avcodec_free_context(&decoder);
            avformat_close_input(&format);
            return fail("direct video decode requires a get_buffer2-capable software decoder "
                        "and native yuv420p, nv12 or p010le (stream: " + best->codec + " " +
                        best->pixelFormat + "); use --allow-video-copy for compatibility");
        }
        fNativePixelFormat = native;
        decoder->thread_count = 4;
        decoder->opaque = this;
        decoder->get_buffer2 = &directBuffer;
        decoder->get_format = &softwareFormat;
    }
    if (avcodec_open2(decoder, codec, nullptr) < 0) {
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return fail("cannot open the " + best->codec + " decoder");
    }
    if (fDirectDecode) {
        fDecodeDescription = std::string(codec->name) + " (direct software decode into Vulkan linear images)";
        std::fprintf(stderr, "openshow-worker: video decode: %s\n", fDecodeDescription.c_str());
    } else if (hardwareConfigured) {
        fDecodeDescription = best->codec + " (VA-API hardware decode)";  // definitive once frames flow
    } else {
        fDecodeDescription =
            best->codec + " (software decode" + (hardwareNote.empty() ? "" : "; " + hardwareNote) +
            ")";
        std::fprintf(stderr, "openshow-worker: video decode: %s\n", fDecodeDescription.c_str());
    }

    // Direct mode preserves the native byte layout; compatibility uses
    // NV12/P010 staging and may convert into that layout.
    fFrameWidth = decoder->width;
    fFrameHeight = decoder->height;
    fRingFormat = fDirectDecode && fNativePixelFormat == AV_PIX_FMT_YUV420P
        ? FrameFormat::kYUV420P : deriveRingFormat(best->pixelFormat);

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
    requestStop();
    join();
    if (fPrimedFrame) {
        auto* frame = static_cast<AVFrame*>(fPrimedFrame);
        av_frame_free(&frame);
        fPrimedFrame = nullptr;
    }
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
    fBacking = nullptr;
    fVideoStreamIndex = -1;
    fStreamInfo = {};
    fPath.clear();
    fDecodeDescription.clear();
    fFrameWidth = fFrameHeight = 0;
    fRingFormat = FrameFormat::kNV12;
    std::lock_guard lock(fStatsMutex);
    fStats = {};
    fAllocationError.clear();
    fDecodeAllocations.clear();
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
    if (!fRing.slotCount()) {
        if (error) *error = "createRing must succeed before startDecode";
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
    if (fBacking) fBacking->requestShutdown();
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

int VideoFrameSource::allocateDecoderBuffer(void* codecPtr, void* framePtr) {
    auto* codec = static_cast<AVCodecContext*>(codecPtr);
    auto* frame = static_cast<AVFrame*>(framePtr);
    auto fail = [&](const std::string& message) {
        std::lock_guard lock(fStatsMutex);
        if (fAllocationError.empty()) fAllocationError = message;
        return AVERROR(EINVAL);
    };
    // FFmpeg requests coded dimensions here (e.g. 144x112 for visible
    // 130x98 H.264), then applies codec cropping to the returned AVFrame.
    if (!fBacking || frame->format != fNativePixelFormat ||
        codec->width != fFrameWidth || codec->height != fFrameHeight ||
        frame->width < fFrameWidth || frame->height < fFrameHeight)
        return fail("decoder requested a different native format or resolution");

    FrameAllocation request;
    request.format = fRingFormat;
    request.width = frame->width;
    request.height = frame->height;
    avcodec_align_dimensions2(codec, &request.width, &request.height, request.alignment);
    // Reserve SIMD overread/edge padding inside every plane, including the
    // last chroma plane. These extra rows are never copied to presentation.
    request.height += 64;
    for (auto& alignment : request.alignment)
        alignment = std::max(alignment, static_cast<int>(av_cpu_max_align()));
    std::string error;
    auto storage = fBacking->allocateDecodeFrame(request, &error);
    if (!storage) return fail(error.empty() ? "direct frame allocation failed" : error);
    if (!storage->base || !storage->size || storage->format != fRingFormat ||
        storage->width < request.width || storage->height < request.height)
        return fail("backing returned an invalid decoder allocation");
    const int planes = fRingFormat == FrameFormat::kYUV420P ? 3 : 2;
    for (int p = 0; p < planes; ++p) {
        const auto base = reinterpret_cast<uintptr_t>(storage->base);
        const auto address = reinterpret_cast<uintptr_t>(storage->data[p]);
        const size_t rows = p ? request.height / 2 : request.height;
        const int rowBytes = (p && planes == 3 ? request.width / 2 : request.width) *
                            (fRingFormat == FrameFormat::kP010 ? 2 : 1);
        if (address < base || address - base >= storage->size ||
            storage->linesize[p] < rowBytes || address % request.alignment[p] ||
            storage->linesize[p] % request.alignment[p] ||
            rows * static_cast<size_t>(storage->linesize[p]) > storage->size - (address - base))
            return fail("mapped plane does not satisfy FFmpeg alignment or padding");
    }
    auto* owner = new (std::nothrow) std::shared_ptr<FrameStorage>(storage);
    if (!owner) return AVERROR(ENOMEM);
    frame->buf[0] = av_buffer_create(storage->base, storage->size,
        [](void* opaque, uint8_t*) { delete static_cast<std::shared_ptr<FrameStorage>*>(opaque); },
        owner, 0);
    if (!frame->buf[0]) { delete owner; return AVERROR(ENOMEM); }
    std::copy(std::begin(storage->data), std::end(storage->data), frame->data);
    std::copy(std::begin(storage->linesize), std::end(storage->linesize), frame->linesize);
    frame->extended_data = frame->data;
    {
        std::lock_guard lock(fStatsMutex);
        fDecodeAllocations.erase(std::remove_if(fDecodeAllocations.begin(), fDecodeAllocations.end(),
            [](const auto& ref) { return ref.expired(); }), fDecodeAllocations.end());
        const bool known = std::any_of(fDecodeAllocations.begin(), fDecodeAllocations.end(),
            [&](const auto& ref) { return ref.lock() == storage; });
        if (!known) fDecodeAllocations.push_back(storage);
    }
    return 0;
}

std::shared_ptr<FrameStorage> VideoFrameSource::decoderStorage(void* framePtr) const {
    const auto* frame = static_cast<const AVFrame*>(framePtr);
    if (frame->format != fNativePixelFormat || frame->width != fFrameWidth ||
        frame->height != fFrameHeight || !frame->buf[0]) return {};
    std::lock_guard lock(fStatsMutex);
    for (const auto& ref : fDecodeAllocations) {
        auto storage = ref.lock();
        if (!storage || frame->buf[0]->data != storage->base ||
            frame->buf[0]->size != storage->size) continue;
        const int planes = fRingFormat == FrameFormat::kYUV420P ? 3 : 2;
        bool matches = true;
        for (int p = 0; p < planes; ++p)
            matches &= frame->data[p] == storage->data[p] &&
                       frame->linesize[p] == storage->linesize[p];
        if (matches) return storage;
    }
    return {};
}

void VideoFrameSource::failDecode(const std::string& error) {
    std::lock_guard lock(fStatsMutex);
    if (fStats.fatalError.empty()) {
        fStats.fatalError = fAllocationError.empty() ? error : fAllocationError;
        ++fStats.decodeErrors;
        std::fprintf(stderr, "openshow-worker: video decode failed: %s\n", fStats.fatalError.c_str());
    }
}

bool VideoFrameSource::primeDirect(std::string* error) {
    auto* decoder = static_cast<AVCodecContext*>(fCodecContext);
    auto* format = static_cast<AVFormatContext*>(fFormatContext);
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    bool flushed = false;
    int result = frame && packet ? 0 : AVERROR(ENOMEM);
    while (result >= 0) {
        result = avcodec_receive_frame(decoder, frame);
        if (result == 0) {
            if (decoderStorage(frame)) {
                fPrimedFrame = frame;
                av_packet_free(&packet);
                return true;
            }
            failDecode("decoder bypassed the mapped Vulkan allocator or changed format");
            break;
        }
        if (result != AVERROR(EAGAIN) || flushed) break;
        do {
            result = av_read_frame(format, packet);
            if (result >= 0 && packet->stream_index != fVideoStreamIndex) av_packet_unref(packet);
            else break;
        } while (true);
        if (result == AVERROR_EOF) {
            flushed = true;
            result = avcodec_send_packet(decoder, nullptr);
        } else if (result >= 0) {
            result = avcodec_send_packet(decoder, packet);
            av_packet_unref(packet);
        }
    }
    failDecode("cannot prime direct decoder: " + ffError(result));
    if (error) *error = stats().fatalError;
    av_frame_free(&frame);
    av_packet_free(&packet);
    return false;
}

void VideoFrameSource::run() {
    AVFormatContext* format = static_cast<AVFormatContext*>(fFormatContext);
    AVCodecContext* decoder = static_cast<AVCodecContext*>(fCodecContext);
    AVFrame* frame = av_frame_alloc();
    // Only compatibility mode uses a transfer frame. Its temporary buffer
    // reference describes the actual mapped destination, not a dummy ticket.
    AVFrame* transfer = av_frame_alloc();
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

    int receivedFormat = AV_PIX_FMT_NONE;
    // Direct-path and format/size failures are terminal; never silently stage.
    auto emitFrame = [&](AVFrame* decoded) -> bool {
        // Direct mode attaches the existing decoder allocation to a token;
        // compatibility writes the token's separate staging buffers.
        VideoFrame out;
        if (!fRing.claim(&out)) {
            return false;
        }
        bool pixelsReady = false;
        if (receivedFormat != AV_PIX_FMT_NONE && receivedFormat != decoded->format) {
            fRing.abortClaim(out.slot);
            failDecode("midstream video pixel format changes are unsupported");
            return false;
        }
        receivedFormat = decoded->format;
        if (decoded->width != fFrameWidth || decoded->height != fFrameHeight) {
            fRing.abortClaim(out.slot);
            failDecode("midstream video resolution changes are unsupported");
            return false;
        }
        if (fDirectDecode) {
            out.storage = decoderStorage(decoded);
            if (!out.storage) {
                fRing.abortClaim(out.slot);
                failDecode("decoder changed format or bypassed the mapped Vulkan allocator");
                return false;
            }
            pixelsReady = true;
        } else if (decoded->format == AV_PIX_FMT_VAAPI) {
            // A VAAPI surface must be copied back to system memory; the
            // PTS stays with the original frame because the transfer
            // copies pixels only. When the download format is the ring
            // format (the norm — both follow the stream's depth) the
            // transfer writes the slot's mapped planes directly.
            bump([](Stats& s) { ++s.downloadedFrames; });
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
                const auto y = reinterpret_cast<uintptr_t>(out.data[0]);
                const auto uv = reinterpret_cast<uintptr_t>(out.data[1]);
                const auto start = std::min(y, uv);
                const auto end = std::max(y + static_cast<size_t>(out.linesize[0]) * out.height,
                    uv + static_cast<size_t>(out.linesize[1]) * (out.height / 2));
                transfer->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(start), end - start,
                    [](void*, uint8_t*) {}, nullptr, 0);
                if (!transfer->buf[0]) { fRing.abortClaim(out.slot); failDecode("transfer buffer allocation failed"); return false; }
                const int err = av_hwframe_transfer_data(transfer, decoded, 0);
                av_frame_unref(transfer);  // drops the temporary mapping reference
                pixelsReady = err >= 0;
            } else {
                // Probe and download format disagree (rare): allocate
                // FFmpeg's own scratch and convert through swscale.
                av_frame_unref(transfer);
                const int err = av_hwframe_transfer_data(transfer, decoded, 0);
                if (err >= 0) {
                    bump([](Stats& s) { ++s.convertedFrames; });
                    pixelsReady = toRingWithSws(transfer, out);
                }
                av_frame_unref(transfer);
            }
        } else if (decoded->format == AV_PIX_FMT_YUV420P && ringFormat == AV_PIX_FMT_NV12) {
            // Planar 8-bit is the software decoder's native output: luma
            // rows copy verbatim, U and V interleave into the slot's
            // chroma plane on the way (the old uvScratch fold, now landing
            // directly in the destination).
            bump([](Stats& s) { ++s.copiedFrames; });
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
            // Already the compatibility slot layout: plain plane copies.
            // Chroma row = width/2 pairs of
            // two components = width * componentBytes bytes.
            const int rowBytes = out.width * componentBytes;
            bump([](Stats& s) { ++s.copiedFrames; });
            copyPlane(out.data[0], out.linesize[0], decoded->data[0], decoded->linesize[0],
                      rowBytes, out.height);
            copyPlane(out.data[1], out.linesize[1], decoded->data[1], decoded->linesize[1],
                      rowBytes, out.height / 2);
            pixelsReady = true;
        } else {
            bump([](Stats& s) { ++s.convertedFrames; });
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
                failDecode("Vulkan frame copy failed");
            }
            return false;
        }
        bump([&](Stats& s) { ++s.framesProduced; if (fDirectDecode) ++s.directFrames; });
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
            if (r < 0 && fDirectDecode) {
                failDecode("direct decoder receive failed: " + ffError(r));
                return false;
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

    if (fPrimedFrame) {
        auto* primed = static_cast<AVFrame*>(fPrimedFrame);
        if (!emitFrame(primed)) fThreadWanted = false;
        av_frame_free(&primed);
        fPrimedFrame = nullptr;
        if (fThreadWanted && !drain(false)) fThreadWanted = false;
    }
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
                    if (fDirectDecode) { failDecode("direct decoder send failed: " + ffError(sendResult)); break; }
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
        failDecode("video demux failed: " + ffError(readResult));
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
