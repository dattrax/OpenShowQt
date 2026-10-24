#pragma once

#include <mutex>

namespace openshow::worker {

// When no dedicated upload queue exists, Skia and the decoder submit to the
// same VkQueue. Vulkan requires external host synchronization for that queue.
// Recursive because image acquisition may run inside the render operation.
// It also orders copy submission against redraws and their completion handoff
// when separate queues exist; decoding itself never holds this guard.
inline std::recursive_mutex& graphicsQueueMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

}  // namespace openshow::worker
