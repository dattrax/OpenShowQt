#include "media/VideoThumbnail.h"

#include "media/FFmpeg.h"
#include "media/SwsScale.h"

#include <png.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace openshow::media {
namespace {

std::string base64Encode(const std::uint8_t* data, std::size_t size) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < size) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back(kTable[n & 0x3F]);
        i += 3;
    }
    if (i < size) {
        const std::uint32_t n =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (i + 1 < size ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < size ? kTable[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

struct PngWriter {
    std::string bytes;
    static void write(png_structp png, png_bytep data, png_size_t length) {
        PngWriter* writer = static_cast<PngWriter*>(png_get_io_ptr(png));
        writer->bytes.append(reinterpret_cast<const char*>(data), length);
    }
    static void flush(png_structp) {}
};

bool encodePng(const std::uint8_t* rgb, int width, int height, std::string* out,
               std::string* error) {
    PngWriter* writer = new PngWriter;
    png_structp png =
        png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) {
        delete writer;
        *error = "png: create_write_struct failed";
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        delete writer;
        *error = "png: create_info_struct failed";
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        delete writer;
        *error = "png: encoding failed";
        return false;
    }
    png_set_write_fn(png, writer, PngWriter::write, PngWriter::flush);
    png_set_IHDR(png, info, static_cast<png_uint_32>(width),
                 static_cast<png_uint_32>(height), 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < height; ++y) {
        png_write_row(png, const_cast<png_bytep>(
                               rgb + static_cast<std::size_t>(y) *
                                         static_cast<std::size_t>(width) * 3));
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    *out = base64Encode(reinterpret_cast<const std::uint8_t*>(writer->bytes.data()),
                        writer->bytes.size());
    delete writer;
    return true;
}

}  // namespace

bool videoThumbnailBase64(const std::string& path, int maxWidth, int maxHeight,
                          std::string* base64Png, int* width, int* height,
                          std::string* error) {
    if (base64Png == nullptr || width == nullptr || height == nullptr) {
        if (error != nullptr) *error = "video thumbnail: null output";
        return false;
    }
    const int boxWidth = std::clamp(maxWidth, 16, 1024);
    const int boxHeight = std::clamp(maxHeight, 16, 1024);

    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    bool ok = false;

    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) {
        if (error != nullptr) *error = "video thumbnail: cannot open " + path;
        return false;
    }
    if (avformat_find_stream_info(format, nullptr) < 0) {
        if (error != nullptr) *error = "video thumbnail: no stream info in " + path;
        goto cleanup;
    }
    {
        const int streamIndex =
            av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex < 0) {
            if (error != nullptr) *error = "video thumbnail: no video stream in " + path;
            goto cleanup;
        }
        AVStream* stream = format->streams[streamIndex];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            if (error != nullptr) *error = "video thumbnail: no decoder for " + path;
            goto cleanup;
        }
        decoder = avcodec_alloc_context3(codec);
        if (decoder == nullptr ||
            avcodec_parameters_to_context(decoder, stream->codecpar) < 0 ||
            avcodec_open2(decoder, codec, nullptr) < 0) {
            if (error != nullptr) *error = "video thumbnail: cannot open decoder for " + path;
            goto cleanup;
        }
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (frame == nullptr || packet == nullptr) {
            if (error != nullptr) *error = "video thumbnail: allocation failed";
            goto cleanup;
        }
        bool got = false;
        int packets = 0;
        while (!got && packets < 128 && av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == streamIndex) {
                ++packets;
                if (avcodec_send_packet(decoder, packet) < 0) {
                    av_packet_unref(packet);
                    break;
                }
                const int result = avcodec_receive_frame(decoder, frame);
                if (result == 0) {
                    got = true;
                } else if (result != AVERROR(EAGAIN)) {
                    av_packet_unref(packet);
                    break;
                }
            }
            av_packet_unref(packet);
        }
        if (!got) {
            // A stream that needs a flush to release its first frame.
            avcodec_send_packet(decoder, nullptr);
            if (avcodec_receive_frame(decoder, frame) == 0) got = true;
        }
        if (!got) {
            if (error != nullptr) *error = "video thumbnail: no frame in " + path;
            goto cleanup;
        }
        if (frame->width <= 0 || frame->height <= 0) {
            if (error != nullptr) *error = "video thumbnail: empty frame in " + path;
            goto cleanup;
        }
        const double scale = std::min(
            {1.0, static_cast<double>(boxWidth) / frame->width,
             static_cast<double>(boxHeight) / frame->height});
        const int dstWidth = std::max(1, static_cast<int>(frame->width * scale));
        const int dstHeight = std::max(1, static_cast<int>(frame->height * scale));
        SwsContext* sws = swsContextFor(
            frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
            frame->color_range == AVCOL_RANGE_JPEG, dstWidth, dstHeight,
            AV_PIX_FMT_RGB24, SWS_BILINEAR);
        if (sws == nullptr) {
            if (error != nullptr) *error = "video thumbnail: scale setup failed";
            goto cleanup;
        }
        std::vector<std::uint8_t> rgb(
            static_cast<std::size_t>(dstWidth) * static_cast<std::size_t>(dstHeight) * 3);
        std::uint8_t* dstData[4] = {rgb.data(), nullptr, nullptr, nullptr};
        int dstLinesize[4] = {dstWidth * 3, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dstData,
                  dstLinesize);
        sws_freeContext(sws);
        ok = encodePng(rgb.data(), dstWidth, dstHeight, base64Png, error);
        *width = dstWidth;
        *height = dstHeight;
    }

cleanup:
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    if (format != nullptr) avformat_close_input(&format);
    return ok;
}

}  // namespace openshow::media
