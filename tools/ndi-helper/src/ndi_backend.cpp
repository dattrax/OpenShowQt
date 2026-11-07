#include "ndi_backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "shm_ring.h"

#include <Processing.NDI.Lib.h>
#include <Processing.NDI.DynamicLoad.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace openshow::ndi {

namespace {

std::string libraryError() {
#ifdef _WIN32
    return "LoadLibrary failed";
#else
    const char* message = dlerror();
    return message ? message : "unknown dynamic-loader error";
#endif
}

void* openLibrary(const std::string& path, std::string* error) {
#ifdef _WIN32
    HMODULE handle = ::LoadLibraryA(path.c_str());
    if (!handle && error) {
        *error = "could not load '" + path + "': " + libraryError();
    }
    return handle;
#else
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle && error) {
        *error = "could not load '" + path + "': " + libraryError();
    }
    return handle;
#endif
}

void* librarySymbol(void* handle, const char* name, std::string* error) {
#ifdef _WIN32
    FARPROC symbol = ::GetProcAddress(static_cast<HMODULE>(handle), name);
    if (!symbol && error) {
        *error = std::string("missing NDI symbol '") + name + "'";
    }
    return reinterpret_cast<void*>(symbol);
#else
    void* symbol = ::dlsym(handle, name);
    if (!symbol && error) {
        *error = std::string("missing NDI symbol '") + name + "': " + libraryError();
    }
    return symbol;
#endif
}

void closeLibrary(void* handle) {
    if (!handle) {
        return;
    }
#ifdef _WIN32
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

std::string executableDir() {
#ifdef __linux__
    std::error_code ec;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return path.parent_path().string();
    }
#endif
    return {};
}

std::vector<std::string> runtimeCandidates() {
    std::vector<std::string> candidates;
#ifdef _WIN32
    const char* runtimeNames[] = {"ProcessingNDI.dll"};
#else
    const char* runtimeNames[] = {"libndi.so.6", "libndi.so"};
#endif
    auto addDirectory = [&candidates, &runtimeNames](const std::string& dir) {
        if (dir.empty()) {
            return;
        }
        for (const char* name : runtimeNames) {
            candidates.push_back(dir + "/" + name);
        }
    };

    if (const char* full = std::getenv("OPENSHOW_NDI_RUNTIME"); full && *full) {
        candidates.emplace_back(full);
    }
    if (const char* redist = std::getenv("NDILIB_REDIST_FOLDER"); redist && *redist) {
        addDirectory(redist);
    }
    addDirectory(executableDir());
#ifdef OPENSHOW_NDI_RUNTIME_DIR
    addDirectory(OPENSHOW_NDI_RUNTIME_DIR);
#endif
    for (const char* name : runtimeNames) {
        candidates.emplace_back(name);
    }
    return candidates;
}

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

struct NdiBackend::Impl {
    void* handle = nullptr;
    const NDIlib_v6* ndi = nullptr;
    bool initialized = false;
    NDIlib_find_instance_t finder = nullptr;
    NDIlib_recv_instance_t recv = nullptr;
    std::string sourceName;
    std::string sourceUrl;
    ShmRing ring;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> streaming{false};
    std::mutex readyMutex;
    std::mutex sinkMutex;
    std::condition_variable readyCv;
    bool ready = false;
    std::string threadError;
    double fps = 0.0;
    std::function<void(const Event&)> sink;

    ~Impl() {
        stopCapture();
        if (finder && ndi) {
            ndi->find_destroy(finder);
            finder = nullptr;
        }
        if (initialized && ndi) {
            ndi->destroy();
            initialized = false;
        }
        closeLibrary(handle);
        handle = nullptr;
        ndi = nullptr;
    }

    bool ensureLoaded(std::string* error) {
        if (ndi) {
            return true;
        }
        std::string lastError;
        for (const std::string& candidate : runtimeCandidates()) {
            handle = openLibrary(candidate, &lastError);
            if (handle) {
                break;
            }
        }
        if (!handle) {
            if (error) {
                *error = "NDI runtime not found (set OPENSHOW_NDI_RUNTIME or place the "
                         "runtime beside the helper): " + lastError;
            }
            return false;
        }
        using LoadFn = const NDIlib_v6* (*)();
        auto load = reinterpret_cast<LoadFn>(librarySymbol(handle, "NDIlib_v6_load", error));
        if (!load) {
            closeLibrary(handle);
            handle = nullptr;
            return false;
        }
        ndi = load();
        if (!ndi) {
            if (error) {
                *error = "NDIlib_v6_load returned no function table";
            }
            closeLibrary(handle);
            handle = nullptr;
            return false;
        }
        if (!ndi->initialize()) {
            if (error) {
                *error = "NDIlib_initialize failed (the runtime could not start)";
            }
            closeLibrary(handle);
            handle = nullptr;
            ndi = nullptr;
            return false;
        }
        initialized = true;
        return true;
    }

    bool refreshFinder(std::uint32_t waitMs, std::string* error) {
        if (!finder) {
            NDIlib_find_create_t findDesc;
            findDesc.show_local_sources = true;
            findDesc.p_groups = nullptr;
            findDesc.p_extra_ips = nullptr;
            finder = ndi->find_create_v2(&findDesc);
            if (!finder) {
                if (error) {
                    *error = "NDIlib_find_create_v2 failed";
                }
                return false;
            }
        }
        if (waitMs > 0) {
            ndi->find_wait_for_sources(finder, waitMs);
        }
        return true;
    }

    bool findSource(const std::string& name, SourceInfo* out, std::uint32_t waitMs,
                    std::string* error) {
        if (!refreshFinder(waitMs, error)) {
            return false;
        }
        auto search = [this, &name, out]() {
            std::uint32_t count = 0;
            const NDIlib_source_t* sources = ndi->find_get_current_sources(finder, &count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const char* sourceName = sources[i].p_ndi_name;
                if (sourceName && name == sourceName) {
                    if (out) {
                        out->name = sourceName;
                        out->url = sources[i].p_url_address ? sources[i].p_url_address : "";
                    }
                    return true;
                }
            }
            return false;
        };
        if (search()) {
            return true;
        }
        if (waitMs > 0) {
            ndi->find_wait_for_sources(finder, waitMs);
            if (search()) {
                return true;
            }
        }
        if (error) {
            *error = "NDI source not found: " + name;
        }
        return false;
    }

    void emit(Event event) {
        std::function<void(const Event&)> copy;
        {
            std::lock_guard<std::mutex> lock(sinkMutex);
            copy = sink;
        }
        if (copy) {
            copy(event);
        }
    }

    void threadMain() {
        std::string error;
        while (!stop.load()) {
            NDIlib_video_frame_v2_t video;
            NDIlib_frame_type_e type = ndi->recv_capture_v3(recv, &video, nullptr, nullptr, 100);
            if (type == NDIlib_frame_type_video) {
                const std::uint32_t width = static_cast<std::uint32_t>(video.xres);
                const std::uint32_t height = static_cast<std::uint32_t>(video.yres);
                const std::uint32_t stride = static_cast<std::uint32_t>(
                    video.line_stride_in_bytes > 0 ? video.line_stride_in_bytes : video.xres * 4);
                if (!ring.valid()) {
                    if (!ring.create(width, height, stride,
                                     static_cast<std::uint32_t>(
                                         openshow::live::FrameFormat::BGRA8),
                                     &error)) {
                        {
                            std::lock_guard<std::mutex> lock(readyMutex);
                            threadError = error;
                            ready = true;
                        }
                        readyCv.notify_all();
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lock(readyMutex);
                        fps = video.frame_rate_D > 0
                                  ? static_cast<double>(video.frame_rate_N) / video.frame_rate_D
                                  : 0.0;
                        ready = true;
                    }
                    readyCv.notify_all();
                }
                openshow::live::FrameView frame;
                frame.data = video.p_data;
                frame.width = width;
                frame.height = height;
                frame.stride = stride;
                frame.format = static_cast<std::uint32_t>(
                    openshow::live::FrameFormat::BGRA8);
                frame.flags = openshow::live::kFrameFlagAlpha;
                const std::int64_t timestamp = video.timestamp;
                frame.timestampNs =
                    timestamp > 0 ? static_cast<std::uint64_t>(timestamp) * 100 : nowNs();
                if (!ring.write(frame, &error)) {
                    {
                        std::lock_guard<std::mutex> lock(readyMutex);
                        threadError = error;
                    }
                    emit({"receiver-error", sourceName, error});
                    ndi->recv_free_video_v2(recv, &video);
                    break;
                }
                ndi->recv_free_video_v2(recv, &video);
            } else if (type == NDIlib_frame_type_error) {
                {
                    std::lock_guard<std::mutex> lock(readyMutex);
                    threadError = "the NDI source stopped sending";
                    ready = true;
                }
                readyCv.notify_all();
                emit({"source-lost", sourceName, "the NDI source stopped sending"});
                break;
            }
        }
        streaming.store(false);
    }

    void stopCapture() {
        stop.store(true);
        if (thread.joinable()) {
            thread.join();
        }
        streaming.store(false);
        if (recv && ndi) {
            ndi->recv_destroy(recv);
            recv = nullptr;
        }
        ring.close();
    }
};

NdiBackend::NdiBackend() : fImpl(std::make_unique<Impl>()) {}
NdiBackend::~NdiBackend() = default;

bool NdiBackend::probe(ProbeInfo* out, std::string* error) {
    if (!fImpl->ensureLoaded(error)) {
        return false;
    }
    if (!out) {
        if (error) {
            *error = "probe: no output";
        }
        return false;
    }
    out->helperVersion = kHelperVersion;
    out->protocolVersion = kProtocolVersion;
    const char* version = fImpl->ndi->version();
    out->ndiVersion = version ? version : "";
    out->canList = true;
    out->canReceive = true;
    return true;
}

bool NdiBackend::list(std::uint32_t waitMs, std::vector<SourceInfo>* out, std::string* error) {
    if (!out) {
        if (error) {
            *error = "list: no output";
        }
        return false;
    }
    out->clear();
    if (!fImpl->ensureLoaded(error) || !fImpl->refreshFinder(waitMs, error)) {
        return false;
    }
    std::uint32_t count = 0;
    const NDIlib_source_t* sources = fImpl->ndi->find_get_current_sources(fImpl->finder, &count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const char* name = sources[i].p_ndi_name;
        if (!name || !*name) {
            continue;
        }
        out->push_back({name, sources[i].p_url_address ? sources[i].p_url_address : ""});
    }
    return true;
}

bool NdiBackend::open(const std::string& source, OpenInfo* out, std::string* error) {
    if (!out) {
        if (error) {
            *error = "open: no output";
        }
        return false;
    }
    if (fImpl->streaming.load()) {
        if (error) {
            *error = "open: a source is already open";
        }
        return false;
    }
    if (!fImpl->ensureLoaded(error)) {
        return false;
    }
    SourceInfo info;
    if (!fImpl->findSource(source, &info, 3000, error)) {
        return false;
    }
    fImpl->sourceName = info.name;
    fImpl->sourceUrl = info.url;

    NDIlib_recv_create_v3_t recvDesc;
    recvDesc.source_to_connect_to.p_ndi_name = fImpl->sourceName.c_str();
    recvDesc.source_to_connect_to.p_url_address =
        fImpl->sourceUrl.empty() ? nullptr : fImpl->sourceUrl.c_str();
    recvDesc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    recvDesc.bandwidth = NDIlib_recv_bandwidth_highest;
    recvDesc.allow_video_fields = false;
    recvDesc.p_ndi_recv_name = "OpenShow NDI Helper";
    fImpl->recv = fImpl->ndi->recv_create_v3(&recvDesc);
    if (!fImpl->recv) {
        if (error) {
            *error = "NDIlib_recv_create_v3 failed for '" + source + "'";
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> readyLock(fImpl->readyMutex);
        fImpl->ready = false;
        fImpl->threadError.clear();
    }
    fImpl->stop.store(false);
    fImpl->streaming.store(true);
    fImpl->thread = std::thread([impl = fImpl.get()] { impl->threadMain(); });

    // Wait for the first frame (the ring is created from its dimensions).
    bool gotFrame = false;
    {
        std::unique_lock<std::mutex> readyLock(fImpl->readyMutex);
        gotFrame = fImpl->readyCv.wait_for(readyLock, std::chrono::seconds(10),
                                           [impl = fImpl.get()] { return impl->ready; });
        if (!gotFrame) {
            // The wait timed out; stop the receiver below and report.
        } else if (!fImpl->threadError.empty()) {
            const std::string message = fImpl->threadError;
            readyLock.unlock();
            fImpl->stopCapture();
            if (error) {
                *error = message;
            }
            return false;
        }
    }
    if (!gotFrame) {
        fImpl->stopCapture();
        if (error) {
            *error = "no NDI video frame arrived within 10 seconds";
        }
        return false;
    }

    out->shmName = fImpl->ring.name();
    const openshow::live::FrameRingHeader* header = fImpl->ring.header();
    out->width = header ? header->width : 0;
    out->height = header ? header->height : 0;
    out->stride = header ? header->stride : 0;
    out->format = header ? header->format : 0;
    out->bytes = fImpl->ring.bytes();
    out->fps = fImpl->fps;
    return true;
}

bool NdiBackend::close(std::string* /*error*/) {
    fImpl->stopCapture();
    fImpl->sourceName.clear();
    fImpl->sourceUrl.clear();
    return true;
}

bool NdiBackend::streaming() const { return fImpl->streaming.load(); }

void NdiBackend::setEventSink(std::function<void(const Event&)> sink) {
    std::lock_guard<std::mutex> lock(fImpl->sinkMutex);
    fImpl->sink = std::move(sink);
}

}  // namespace openshow::ndi
