#include "media/VideoThumbnail.h"

#include "media/FFmpeg.h"
#include "media/SwsScale.h"

#include <png.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

bool encodePngBytes(const std::uint8_t* rgb, int width, int height, std::string* out,
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
    *out = std::move(writer->bytes);
    delete writer;
    return true;
}

// FNV-1a over the source's identity and the requested box: a stable name for
// the same file at the same size, and a different one when either changes.
std::string fingerprint(const std::string& path, std::uintmax_t size,
                        std::int64_t modified, int boxWidth, int boxHeight) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const void* data, std::size_t count) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    mix(path.data(), path.size());
    mix(&size, sizeof(size));
    mix(&modified, sizeof(modified));
    mix(&boxWidth, sizeof(boxWidth));
    mix(&boxHeight, sizeof(boxHeight));
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// The cache's budgets, applied after every write (review D7b): at most
// kThumbnailCacheMaxFiles entries, kThumbnailCacheMaxBytes on disk and
// kThumbnailCacheMaxAgeSeconds of life. Oldest-first, so a bin the operator
// keeps using keeps its pictures.
void pruneCache(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    std::error_code error;
    struct Entry {
        fs::path path;
        fs::file_time_type written;
        std::uintmax_t bytes;
    };
    std::vector<Entry> entries;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        if (error || !entry.is_regular_file(error)) {
            continue;
        }
        entries.push_back({entry.path(), entry.last_write_time(error),
                           entry.file_size(error)});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.written < b.written; });
    const auto now = fs::file_time_type::clock::now();
    std::uintmax_t total = 0;
    for (const Entry& entry : entries) {
        total += entry.bytes;
    }
    std::size_t kept = entries.size();
    for (const Entry& entry : entries) {
        const bool tooOld =
            std::chrono::duration_cast<std::chrono::seconds>(now - entry.written).count() >
            kThumbnailCacheMaxAgeSeconds;
        if (tooOld || kept > kThumbnailCacheMaxFiles || total > kThumbnailCacheMaxBytes) {
            fs::remove(entry.path, error);
            if (!tooOld) {
                total -= entry.bytes;
                --kept;
            } else {
                total -= entry.bytes;
                --kept;
            }
        }
    }
}

}  // namespace

bool videoThumbnailPng(const std::string& path, int maxWidth, int maxHeight,
                       std::string* png, int* width, int* height, std::string* error) {
    if (png == nullptr || width == nullptr || height == nullptr) {
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
        ok = encodePngBytes(rgb.data(), dstWidth, dstHeight, png, error);
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

bool videoThumbnailBase64(const std::string& path, int maxWidth, int maxHeight,
                          std::string* base64Png, int* width, int* height,
                          std::string* error) {
    std::string png;
    if (!videoThumbnailPng(path, maxWidth, maxHeight, &png, width, height, error)) {
        return false;
    }
    if (base64Png == nullptr) {
        if (error != nullptr) *error = "video thumbnail: null output";
        return false;
    }
    *base64Png = base64Encode(reinterpret_cast<const std::uint8_t*>(png.data()), png.size());
    return true;
}

bool videoThumbnailCachePath(const std::string& path, int maxWidth, int maxHeight,
                             const std::string& cacheDir, std::string* cachePath,
                             int* width, int* height, std::string* error) {
    namespace fs = std::filesystem;
    if (cachePath == nullptr) {
        if (error != nullptr) *error = "video thumbnail: null cache path";
        return false;
    }
    const int boxWidth = std::clamp(maxWidth, 16, 1024);
    const int boxHeight = std::clamp(maxHeight, 16, 1024);
    std::error_code statusError;
    const std::uintmax_t size = fs::file_size(path, statusError);
    if (statusError) {
        if (error != nullptr) *error = "video thumbnail: cannot read " + path;
        return false;
    }
    // The mtime in the name is what makes a re-encoded clip a NEW card rather
    // than the old picture under a new file's name.
    const auto modified = fs::last_write_time(path, statusError);
    const std::int64_t modifiedSeconds =
        statusError
            ? 0
            : std::chrono::duration_cast<std::chrono::seconds>(modified.time_since_epoch())
                  .count();
    const fs::path directory(cacheDir);
    fs::create_directories(directory, statusError);
    if (statusError) {
        if (error != nullptr) *error = "video thumbnail: cannot create the cache directory";
        return false;
    }
    const fs::path target =
        directory /
        (fingerprint(path, size, modifiedSeconds, boxWidth, boxHeight) + ".png");
    if (fs::exists(target, statusError) && !statusError) {
        // The budgets hold on a HIT too: the directory can have been filled
        // by another session between writes.
        pruneCache(directory);
        *cachePath = fs::absolute(target).string();
        // The size was measured when the entry was written; a caller that
        // needs it re-reads the PNG header instead of the source.
        if (width != nullptr) *width = boxWidth;
        if (height != nullptr) *height = boxHeight;
        return true;
    }
    std::string png;
    int thumbWidth = 0;
    int thumbHeight = 0;
    if (!videoThumbnailPng(path, boxWidth, boxHeight, &png, &thumbWidth, &thumbHeight,
                           error)) {
        return false;
    }
    // Written through a temporary file and renamed, so a reader never sees a
    // torn PNG. The name is content-identity, so a concurrent writer writes
    // the same bytes.
    const fs::path staging = target.string() + ".tmp";
    {
        std::ofstream out(staging, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error != nullptr) *error = "video thumbnail: cannot write the cache file";
            return false;
        }
        out.write(png.data(), static_cast<std::streamsize>(png.size()));
        if (!out) {
            if (error != nullptr) *error = "video thumbnail: short write to the cache";
            return false;
        }
    }
    fs::rename(staging, target, statusError);
    if (statusError) {
        if (error != nullptr) *error = "video thumbnail: cannot publish the cache file";
        return false;
    }
    pruneCache(directory);
    *cachePath = fs::absolute(target).string();
    if (width != nullptr) *width = thumbWidth;
    if (height != nullptr) *height = thumbHeight;
    return true;
}

}  // namespace openshow::media
