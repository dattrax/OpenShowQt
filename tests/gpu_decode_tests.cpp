// Real codec -> custom get_buffer2 Vulkan allocation -> one image copy ->
// borrowed YCbCr texture. Readback and reference CPU decoding are test-only.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "media/FFmpeg.h"
#include "media/VideoFrameSource.h"
#include "output/VulkanContext.h"
#include "output/VulkanQueueGuard.h"
#include "render/YuvGpu.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"
#include "src/core/SkYUVMath.h"

using namespace openshow::media;
using openshow::worker::GpuYuvUploader;
using openshow::worker::VulkanContext;
namespace {
bool check(bool ok, const char* message) {
    std::fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "ok" : "FAIL", message);
    return ok;
}
std::array<int, 3> rgb(double y, double u, double v) {
    float m[20];
    SkColorMatrix_YUV2RGB(kRec601_Limited_SkYUVColorSpace, m);
    std::array<int, 3> out;
    for (int c = 0; c < 3; ++c)
        out[c] = std::clamp(static_cast<int>(std::lround(255 *
            (m[5*c]*y + m[5*c+1]*u + m[5*c+2]*v + m[5*c+4]))), 0, 255);
    return out;
}
std::vector<uint8_t> draw(GrDirectContext* gr, SkImage* image, int w, int h) {
    std::lock_guard queueLock(openshow::worker::graphicsQueueMutex());
    if (!image) return {};
    auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType,
                                 SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::RenderTarget(gr, skgpu::Budgeted::kNo, info);
    if (!surface) return {};
    surface->getCanvas()->drawImage(image, 0, 0);
    std::vector<uint8_t> pixels(w*h*4);
    if (!surface->readPixels(info, pixels.data(), w*4, 0, 0)) return {};
    return pixels;
}
bool matches(const std::vector<uint8_t>& pixels, const std::array<int, 3>& expected) {
    if (pixels.size() < 4) return false;
    for (int c = 0; c < 3; ++c) {
        if (std::abs(int(pixels[c]) - expected[c]) > 3) {
            std::fprintf(stderr, "pixel channel %d got %u expected %d\n", c, pixels[c], expected[c]);
            return false;
        }
    }
    return true;
}
bool create(GpuYuvUploader& gpu, VulkanContext& vk, GrDirectContext* gr,
            int width, int height, FrameFormat format, std::string& error) {
    return gpu.create(vk.device(), vk.physicalDevice(), vk.queueIndex(), vk.uploadQueueIndex(),
        vk.uploadQueue(), gr, width, height, format, kRec601_Limited_SkYUVColorSpace, &error);
}
std::vector<std::array<int, 3>> reference(const std::string& path) {
    AVFormatContext* container = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<std::array<int, 3>> colors;
    if (avformat_open_input(&container, path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(container, nullptr) < 0) goto done;
    {
        int stream = av_find_best_stream(container, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream < 0) goto done;
        codec = avcodec_alloc_context3(avcodec_find_decoder(container->streams[stream]->codecpar->codec_id));
        if (!codec || avcodec_parameters_to_context(codec, container->streams[stream]->codecpar) < 0 ||
            avcodec_open2(codec, codec->codec, nullptr) < 0) goto done;
        auto drain = [&] {
            while (avcodec_receive_frame(codec, frame) == 0) {
                if (frame->format == AV_PIX_FMT_YUV420P)
                    colors.push_back(rgb(frame->data[0][0]/255.0,
                                         frame->data[1][0]/255.0, frame->data[2][0]/255.0));
                if (frame->format == AV_PIX_FMT_YUV420P10LE)
                    colors.push_back(rgb(*reinterpret_cast<uint16_t*>(frame->data[0])/1023.0,
                                         *reinterpret_cast<uint16_t*>(frame->data[1])/1023.0,
                                         *reinterpret_cast<uint16_t*>(frame->data[2])/1023.0));
                av_frame_unref(frame);
            }
        };
        while (av_read_frame(container, packet) >= 0) {
            if (packet->stream_index == stream && avcodec_send_packet(codec, packet) == 0) drain();
            av_packet_unref(packet);
        }
        avcodec_send_packet(codec, nullptr);
        drain();
    }
done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&container);
    return colors;
}
// Exercises padded native allocations, full presentation rings, independent
// FFmpeg references, B-frame order, EOF, looping, and stop while blocked.
bool decode(VulkanContext& vk, GrDirectContext* gr, const std::string& path, bool loop, bool compatibility = false) {
    const auto expected = reference(path);
    GpuYuvUploader gpu;
    VideoFrameSource source;
    source.setDirectDecodeEnabled(!compatibility);
    source.setHardwareDecodeEnabled(false);
    std::string error;
    if (expected.empty() || !source.open(path, &error) ||
        !create(gpu, vk, gr, source.frameWidth(), source.frameHeight(), source.ringFormat(), error) ||
        !source.createRing(GpuYuvUploader::kSlots, &gpu, &error) || !source.startDecode(loop, &error)) {
        std::fprintf(stderr, "direct decode setup: %s\n", error.c_str());
        source.close();
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60)); // bounded full ring
    bool ok = true;
    size_t count = 0;
    const size_t target = loop ? expected.size()*2 : expected.size();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (count < target && std::chrono::steady_clock::now() < deadline) {
        VideoFrame frame;
        if (!source.ring().poll(&frame, std::chrono::milliseconds(50))) {
            if (source.ring().stopped()) break;
            continue;
        }
        ok &= matches(draw(gr, gpu.imageForSlot(frame.slot, &error).get(), frame.width, frame.height),
                      expected[count % expected.size()]);
        ok &= std::abs(frame.ptsSeconds - (count % expected.size()) / 12.0) < 0.001;
        // readPixels above waits for sampling; release without a semaphore is safe.
        gpu.release(frame.slot);
        source.ring().release(frame.slot);
        ++count;
    }
    source.requestStop();
    gpu.requestShutdown();
    source.join();
    auto stats = source.stats();
    ok &= check(count == target, "real decoded frames preserve B-frame order, pixels, and PTS");
    ok &= check(stats.fatalError.empty() && stats.decodeErrors == 0, "direct decode has no errors");
    if (!compatibility) {
        ok &= check(stats.directFrames == stats.framesProduced && stats.directFrames > 0 &&
                    stats.copiedFrames == 0 && stats.convertedFrames == 0 && stats.downloadedFrames == 0,
                    "all decoded frame planes belong to mapped Vulkan allocations; zero CPU copies/downloads/conversions");
        ok &= check(gpu.stats().directCopies == stats.framesProduced &&
                    gpu.stats().uploads == stats.framesProduced, "exactly one vkCmdCopyImage per published frame");
        ok &= check(gpu.stats().decodeImagesCreated > 4 && gpu.stats().decodeImagesCreated <= 64 &&
                    gpu.stats().slotsCreated == 4 && source.ring().stats().peakFilled <= 4,
                    "decoder reference pool is independent of the bounded four presentation slots");
    } else {
        ok &= check(stats.directFrames == 0 && stats.convertedFrames == stats.framesProduced &&
                    gpu.stats().directCopies == 0, "explicit High10 compatibility conversion is reported");
    }
    if (loop) ok &= check(stats.loops > 0, "loop flushes decoder and reuses mapped allocations");
    std::printf("direct capture: frames=%llu copies=%llu decoder-images=%llu wraps=%llu\n",
                (unsigned long long)stats.framesProduced, (unsigned long long)gpu.stats().uploads,
                (unsigned long long)gpu.stats().decodeImagesCreated, (unsigned long long)gpu.stats().wraps);
    source.close();
    return ok;
}

bool allocationTests(VulkanContext& vk, GrDirectContext* gr) {
    bool ok = true;
    for (auto format : {FrameFormat::kYUV420P, FrameFormat::kNV12, FrameFormat::kP010}) {
        GpuYuvUploader gpu;
        std::string error;
        if (!create(gpu, vk, gr, 64, 64, format, error)) return check(false, error.c_str());
        FrameAllocation request{format, 128, 128, {64, 64, 64, 0}};
        auto storage = gpu.allocateDecodeFrame(request, &error);
        if (!storage) return check(false, error.c_str());
        auto* original = storage.get();
        for (int iteration = 0; iteration < 3; ++iteration) {
            const int y = 64 + iteration*30, u = 96, v = 160;
            if (format == FrameFormat::kP010) {
                for (int row = 0; row < 64; ++row)
                    std::fill_n(reinterpret_cast<uint16_t*>(storage->data[0] + row*storage->linesize[0]), 64, y << 8);
                for (int row = 0; row < 32; ++row) {
                    auto* uv = reinterpret_cast<uint16_t*>(storage->data[1] + row*storage->linesize[1]);
                    for (int x = 0; x < 32; ++x) { uv[x*2] = u << 8; uv[x*2+1] = v << 8; }
                }
            } else {
                for (int row = 0; row < 64; ++row) std::memset(storage->data[0] + row*storage->linesize[0], y, 64);
                for (int row = 0; row < 32; ++row) {
                    if (format == FrameFormat::kYUV420P) {
                        std::memset(storage->data[1] + row*storage->linesize[1], u, 32);
                        std::memset(storage->data[2] + row*storage->linesize[2], v, 32);
                    } else {
                        auto* uv = storage->data[1] + row*storage->linesize[1];
                        for (int x = 0; x < 32; ++x) { uv[x*2] = u; uv[x*2+1] = v; }
                    }
                }
            }
            ok &= gpu.commitFrame(0, storage);
            auto image = gpu.imageForSlot(0, &error);
            const double scale = format == FrameFormat::kP010 ? 1023.0/4 : 255.0;
            ok &= matches(draw(gr, image.get(), 64, 64), rgb(y/scale, u/scale, v/scale));
            gpu.release(0);
        }
        storage.reset();
        storage = gpu.allocateDecodeFrame(request, &error);
        ok &= check(storage.get() == original, "released native image is reused after copy completion");
        std::vector<std::shared_ptr<FrameStorage>> retained{storage};
        while (retained.size() < GpuYuvUploader::kMaxDecodeImages) {
            auto next = gpu.allocateDecodeFrame(request, &error);
            if (!next) break;
            retained.push_back(std::move(next));
        }
        ok &= check(retained.size() == 64 && !gpu.allocateDecodeFrame(request, &error) &&
                    error.find("exhausted") != std::string::npos, "64 retained decoder references fail promptly instead of deadlocking");
        retained.clear();
        storage.reset();
        ok &= check(bool(gpu.allocateDecodeFrame(request, &error)), "pool recovers when decoder references are released");
        ok &= check(gpu.stats().uploads == 3 && gpu.stats().wraps == 1,
                    "native two/three-plane images use one copy and one persistent sampler wrapper");
    }
    return ok;
}

bool blockedStop(VulkanContext& vk, GrDirectContext* gr) {
    GpuYuvUploader gpu;
    VideoFrameSource source;
    std::string error;
    if (!source.open("direct-fixtures/h264.mp4", &error) ||
        !create(gpu, vk, gr, source.frameWidth(), source.frameHeight(), source.ringFormat(), error) ||
        !source.createRing(4, &gpu, &error) || !source.startDecode(true, &error)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto start = std::chrono::steady_clock::now();
    source.requestStop(); // must also release a backing commit wait
    source.join();
    bool ok = check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1) &&
                    source.stats().fatalError.empty(), "stop joins a direct decoder blocked by a full presentation ring");
    source.close();
    return ok;
}

class RefusingBacking final : public FrameBacking {
public:
    bool slotPlanes(unsigned, uint8_t* [4], int [4]) override { return false; }
    bool commit(unsigned) override { return false; }
    std::shared_ptr<FrameStorage> allocateDecodeFrame(const FrameAllocation&, std::string* error) override {
        *error = "test device cannot supply aligned native planes";
        return {};
    }
};

bool initialFailure() {
    RefusingBacking backing;
    VideoFrameSource source;
    std::string error;
    if (!source.open("direct-fixtures/h264.mp4", &error)) return false;
    bool ok = check(!source.createRing(4, &backing, &error) &&
                    error.find("aligned native planes") != std::string::npos &&
                    source.stats().framesProduced == 0,
                    "incompatible backing fails before publishing, with the allocator's reason");
    source.close();
    return ok;
}

bool formatChange(VulkanContext& vk, GrDirectContext* gr) {
    GpuYuvUploader gpu;
    VideoFrameSource source;
    std::string error;
    if (!source.open("direct-fixtures/change.mp4", &error) ||
        !create(gpu, vk, gr, source.frameWidth(), source.frameHeight(), source.ringFormat(), error) ||
        !source.createRing(4, &gpu, &error) || !source.startDecode(false, &error)) {
        std::fprintf(stderr, "format-change setup: %s\n", error.c_str());
        source.close();
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        VideoFrame frame;
        if (!source.ring().poll(&frame, std::chrono::milliseconds(50))) {
            if (source.ring().stopped()) break;
            continue;
        }
        gpu.release(frame.slot); // dropped without drawing, consumes ready on reuse
        source.ring().release(frame.slot);
    }
    source.requestStop();
    source.join();
    auto stats = source.stats();
    bool ok = check(stats.framesProduced > 0 && !stats.fatalError.empty() && stats.decodeErrors > 0,
                    "midstream resolution changes terminate with an error after initial direct frames");
    source.close();
    return ok;
}
}
int main(int argc, char** argv) {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) return 77;
    VulkanContext vk;
    if (!vk.createHeadless()) return 77;
    auto gr = GrDirectContexts::MakeVulkan(vk.backendContext());
    if (!gr) return 77;
    std::filesystem::create_directories("direct-fixtures");
    const std::string input = "ffmpeg -hide_banner -loglevel error -nostdin -y -f lavfi -i "
        "\"nullsrc=s=130x98:r=12,geq=lum='32+mod(N*7,180)':cb='64+mod(N*5,128)':cr='80+mod(N*3,112)'\" -frames:v 24 ";
    if (std::system((input + "-c:v libx264 -preset fast -bf 3 -g 24 direct-fixtures/h264.mp4").c_str()) != 0 ||
        std::system((input + "-c:v libx265 -preset fast -x265-params pools=1:frame-threads=1:log-level=error:bframes=3 direct-fixtures/hevc.mp4").c_str()) != 0 ||
        std::system((input + "-pix_fmt yuv420p10le -c:v libx264 -preset fast -profile:v high10 direct-fixtures/high10.mp4").c_str()) != 0)
        return check(false, "generate H.264, HEVC, and High10 fixtures") ? 0 : 1;
    if (std::system((input.substr(0, input.find("-frames:v")) +
        "-frames:v 4 -pix_fmt nv12 -c:v rawvideo direct-fixtures/packet-backed.nut").c_str()) != 0)
        return 1;
    if (argc == 2 && std::strcmp(argv[1], "--capture-direct") == 0) {
        bool ok = decode(vk, gr.get(), "direct-fixtures/h264.mp4", false);
        gr.reset();
        vk.destroy();
        return ok && vk.validationErrors() == 0 ? 0 : 1;
    }
    auto changedInput = input;
    changedInput.replace(changedInput.find("130x98"), 6, "128x96");
    std::ofstream("direct-fixtures/change.txt") << "file 'h264.mp4'\nfile 'other.mp4'\n";
    if (std::system((changedInput + "-c:v libx264 -preset fast -bf 3 direct-fixtures/other.mp4").c_str()) != 0 ||
        std::system("ffmpeg -hide_banner -loglevel error -nostdin -y -f concat -safe 0 -i direct-fixtures/change.txt -c copy direct-fixtures/change.mp4") != 0)
        return 1;
    bool ok = allocationTests(vk, gr.get());
    ok &= initialFailure();
    ok &= blockedStop(vk, gr.get());
    ok &= formatChange(vk, gr.get());
    ok &= decode(vk, gr.get(), "direct-fixtures/h264.mp4", false);
    ok &= decode(vk, gr.get(), "direct-fixtures/hevc.mp4", true);
    VideoFrameSource unsupported;
    std::string error;
    ok &= check(!unsupported.open("direct-fixtures/high10.mp4", &error) && error.find("--allow-video-copy") != std::string::npos,
                "planar low-bit-aligned 10-bit cannot silently copy in direct mode");
    VideoFrameSource packetBacked;
    ok &= check(!packetBacked.open("direct-fixtures/packet-backed.nut", &error) &&
                error.find("get_buffer2") != std::string::npos,
                "packet-backed rawvideo cannot bypass direct allocation even with native NV12 bytes");
    packetBacked.close();
    unsupported.setDirectDecodeEnabled(false);
    unsupported.setHardwareDecodeEnabled(false);
    ok &= check(unsupported.open("direct-fixtures/high10.mp4", &error), "explicit compatibility mode accepts planar High10");
    unsupported.close();
    ok &= decode(vk, gr.get(), "direct-fixtures/high10.mp4", false, true);
    gr.reset();
    vk.destroy();
    ok &= check(vk.validationErrors() == 0, "no Vulkan validation or synchronization errors");
    return ok ? 0 : 1;
}
