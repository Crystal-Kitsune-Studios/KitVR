// KitVR Vulkan WSI layer.
//
// SteamVR on Linux cannot hand frames to an OpenVR driver: IVRVirtualDisplay is broken there and
// IVRDriverDirectModeComponent is not implemented at all. The supported way to get the composited
// image out is to intercept the swapchain that vrcompositor presents the HMD window into, which is
// what this layer does.
//
// On every present of the largest swapchain the layer records a copy of the presented image into a
// host visible buffer. The copy waits on the present's semaphores and the present waits on the copy,
// so the image is read after the compositor finished drawing it and before it is shown. A worker
// thread waits for the copy and publishes the pixels into a POSIX shared memory ring, so the
// compositor's present thread never blocks on the readback. The KitVR server reads that ring with
// --source steamvr and feeds it to ffmpeg as rawvideo.
//
// The ring layout is documented in linux/README.md and mirrored by crates/server/src/steamvr.rs.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define KITVR_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr uint32_t kMagic = 0x4B565246u;  // "KVRF"
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 48;
constexpr uint32_t kFormatBgra8 = 0;
constexpr uint32_t kFormatRgba8 = 1;
constexpr uint32_t kFormatX2Rgb10 = 2;
constexpr uint32_t kFormatX2Bgr10 = 3;
// Copies in flight per swapchain. A frame is skipped when every slot is still being read back.
constexpr size_t kSlots = 3;

bool debugEnabled() {
    static const bool enabled = std::getenv("KITVR_LAYER_DEBUG") != nullptr;
    return enabled;
}

void logLine(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::fputs("kitvr-layer: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
    std::fflush(stderr);
}

#define KITVR_LOG(...)                                       \
    do {                                                     \
        if (debugEnabled()) logLine(__VA_ARGS__);            \
    } while (0)

// ---------------------------------------------------------------------------------------------
// Shared memory publisher
// ---------------------------------------------------------------------------------------------

class Publisher {
public:
    ~Publisher() { close(); }

    bool open(uint32_t width, uint32_t height, uint32_t format, uint32_t stride) {
        if (width == 0 || height == 0 || stride == 0) return false;
        const size_t payload = static_cast<size_t>(stride) * height;
        const size_t total = kHeaderBytes + payload;
        if (mapping_ && total <= size_ && width == width_ && height == height_ &&
            format == format_ && stride == stride_) {
            return true;
        }
        close();

        const char* name = std::getenv("KITVR_STEAMVR_SHM");
        if (name == nullptr || name[0] == '\0') name = "kitvr-steamvr";
        // Overridable so a sandboxed server (for example a Flatpak) can be pointed at a directory
        // both sandboxes can see.
        const char* directory = std::getenv("KITVR_STEAMVR_SHM_DIR");
        if (directory == nullptr || directory[0] == '\0') directory = "/dev/shm";
        const std::string path = std::string(directory) + "/" + name;

        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd < 0) {
            logLine("cannot open %s", path.c_str());
            return false;
        }
        // Never shrink the file: a reader that mapped the old size would fault on the missing tail.
        struct stat info{};
        size_t fileSize = total;
        if (::fstat(fd, &info) == 0 && static_cast<size_t>(info.st_size) > fileSize) {
            fileSize = static_cast<size_t>(info.st_size);
        }
        if (::ftruncate(fd, static_cast<off_t>(fileSize)) != 0) {
            logLine("cannot size %s", path.c_str());
            ::close(fd);
            return false;
        }
        void* mapping = ::mmap(nullptr, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (mapping == MAP_FAILED) {
            logLine("cannot map %s", path.c_str());
            return false;
        }

        mapping_ = static_cast<uint8_t*>(mapping);
        size_ = fileSize;
        width_ = width;
        height_ = height;
        format_ = format;
        stride_ = stride;
        // A geometry change keeps the frame id counting up, so readers notice the new header.
        std::memcpy(mapping_ + 0, &kMagic, 4);
        std::memcpy(mapping_ + 4, &kVersion, 4);
        std::memcpy(mapping_ + 8, &width_, 4);
        std::memcpy(mapping_ + 12, &height_, 4);
        std::memcpy(mapping_ + 16, &format_, 4);
        std::memcpy(mapping_ + 20, &stride_, 4);
        const uint32_t payload32 = static_cast<uint32_t>(payload);
        std::memcpy(mapping_ + 24, &payload32, 4);
        std::memset(mapping_ + 28, 0, 4);
        logLine("publishing %ux%u stride %u to %s", width_, height_, stride_, path.c_str());
        return true;
    }

    // Copies a frame and publishes it. frameId is bumped last with release semantics so a reader
    // never sees a partially written frame with a fresh id.
    void publish(const void* pixels, size_t bytes) {
        if (mapping_ == nullptr) return;
        const size_t payload = static_cast<size_t>(stride_) * height_;
        std::memcpy(mapping_ + kHeaderBytes, pixels, std::min(bytes, payload));
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const uint64_t stamp = static_cast<uint64_t>(now.tv_sec) * 1000000000ull +
                               static_cast<uint64_t>(now.tv_nsec);
        std::memcpy(mapping_ + 40, &stamp, 8);
        auto* id = reinterpret_cast<uint64_t*>(mapping_ + 32);
        __atomic_add_fetch(id, 1ull, __ATOMIC_RELEASE);
    }

private:
    void close() {
        if (mapping_ == nullptr) return;
        ::munmap(mapping_, size_);
        mapping_ = nullptr;
        size_ = 0;
    }

    uint8_t* mapping_ = nullptr;
    size_t size_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t format_ = 0;
    uint32_t stride_ = 0;
};

uint32_t shmFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SNORM:
            return kFormatBgra8;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SNORM:
            return kFormatRgba8;
        // Wayland drivers often pick a 10 bit format for the window; ffmpeg reads both packings.
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            return kFormatX2Rgb10;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return kFormatX2Bgr10;
        default:
            return UINT32_MAX;
    }
}

// ---------------------------------------------------------------------------------------------
// Per object state
// ---------------------------------------------------------------------------------------------

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr next = nullptr;
    PFN_vkDestroyInstance destroy = nullptr;
};

struct DeviceData;

// One readback in flight: the copy is recorded into `commands`, lands in `buffer`, and `fence`
// signals when the worker may publish it.
struct Slot {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool coherent = true;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore copied = VK_NULL_HANDLE;
    bool busy = false;  // guarded by Capture::mutex
};

// A submitted copy for the worker to publish. `fence` signals when the copy finished; `release`, if
// set, is cleared after publishing so a virtual swapchain image can be handed out again.
struct Job {
    size_t slot = 0;
    VkFence fence = VK_NULL_HANDLE;
    std::atomic<bool>* release = nullptr;
};

// Readback state for the swapchain that is being captured.
struct Capture {
    DeviceData* device = nullptr;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    uint32_t format = 0;
    uint32_t queueFamily = UINT32_MAX;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<Slot, kSlots> slots{};
    size_t nextSlot = 0;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> pending;
    bool stopping = false;
    std::thread worker;
};

struct SwapchainInfo {
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    InstanceData* instance = nullptr;
    PFN_vkGetDeviceProcAddr next = nullptr;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
    PFN_vkGetDeviceQueue getQueue = nullptr;
    PFN_vkGetDeviceQueue2 getQueue2 = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR swapchainImages = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkCreateCommandPool createPool = nullptr;
    PFN_vkDestroyCommandPool destroyPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommands = nullptr;
    PFN_vkResetCommandBuffer resetCommands = nullptr;
    PFN_vkBeginCommandBuffer beginCommands = nullptr;
    PFN_vkEndCommandBuffer endCommands = nullptr;
    PFN_vkCmdPipelineBarrier barrier = nullptr;
    PFN_vkCmdCopyImageToBuffer copyImage = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements bufferRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindBufferMemory bindBuffer = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkInvalidateMappedMemoryRanges invalidate = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkGetFenceStatus fenceStatus = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements imageRequirements = nullptr;
    PFN_vkBindImageMemory bindImage = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkDeviceWaitIdle waitIdle = nullptr;
    bool complete = false;  // every entry point the capture path needs was found

    std::mutex mutex;  // guards swapchains and capture
    std::unordered_map<VkSwapchainKHR, SwapchainInfo> swapchains;
    std::unique_ptr<Capture> capture;
};

std::mutex g_instancesMutex;
std::unordered_map<VkInstance, InstanceData*> g_instances;
std::mutex g_physicalMutex;
std::unordered_map<VkPhysicalDevice, InstanceData*> g_physicalInstances;
// Devices are looked up by device handle and by queue, since vkQueuePresentKHR only has a queue.
std::mutex g_devicesMutex;
std::unordered_map<VkDevice, DeviceData*> g_devices;
std::unordered_map<VkQueue, std::pair<DeviceData*, uint32_t>> g_queues;
PFN_vkGetInstanceProcAddr g_globalGipa = nullptr;

InstanceData* findInstance(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    auto it = g_instances.find(instance);
    return it == g_instances.end() ? nullptr : it->second;
}

DeviceData* findDevice(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_devicesMutex);
    auto it = g_devices.find(device);
    return it == g_devices.end() ? nullptr : it->second;
}

bool findQueue(VkQueue queue, DeviceData** device, uint32_t* family) {
    std::lock_guard<std::mutex> lock(g_devicesMutex);
    auto it = g_queues.find(queue);
    if (it == g_queues.end()) return false;
    *device = it->second.first;
    *family = it->second.second;
    return true;
}

void rememberQueue(DeviceData* data, VkQueue queue, uint32_t family) {
    if (queue == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(g_devicesMutex);
    g_queues[queue] = {data, family};
}

template <typename T>
T* findLinkInfo(const void* next, VkStructureType type, VkLayerFunction function) {
    auto* chain = reinterpret_cast<const VkBaseInStructure*>(next);
    while (chain != nullptr) {
        if (chain->sType == type) {
            auto* info = const_cast<T*>(reinterpret_cast<const T*>(chain));
            if (info->function == function) return info;
        }
        chain = chain->pNext;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------------------------

void publishWorker(Capture* capture) {
    DeviceData* data = capture->device;
    const size_t bytes = static_cast<size_t>(capture->extent.width) * capture->extent.height * 4;
    Publisher publisher;
    bool open = publisher.open(capture->extent.width, capture->extent.height, capture->format,
                               capture->extent.width * 4);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(capture->mutex);
            capture->wake.wait(lock, [&] { return capture->stopping || !capture->pending.empty(); });
            if (capture->pending.empty()) return;
            job = capture->pending.front();
            capture->pending.pop_front();
        }
        Slot& slot = capture->slots[job.slot];
        const VkResult waited = data->waitForFences(data->device, 1, &job.fence, VK_TRUE,
                                                    1000000000ull);
        if (waited == VK_SUCCESS && open) {
            if (!slot.coherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = slot.memory;
                range.size = VK_WHOLE_SIZE;
                data->invalidate(data->device, 1, &range);
            }
            publisher.publish(slot.mapped, bytes);
        } else if (waited != VK_SUCCESS) {
            logLine("frame readback did not finish (%d)", static_cast<int>(waited));
        }
        if (job.release != nullptr && waited == VK_SUCCESS) job.release->store(false);
        std::lock_guard<std::mutex> lock(capture->mutex);
        // A timed out slot stays busy so its fence and semaphore are never reused while pending.
        if (waited == VK_SUCCESS) slot.busy = false;
    }
}

VkPhysicalDeviceMemoryProperties memoryProperties(DeviceData* data) {
    VkPhysicalDeviceMemoryProperties properties{};
    auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        data->instance->next(data->instance->instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (getProperties != nullptr) getProperties(data->physical, &properties);
    return properties;
}

uint32_t pickMemoryType(DeviceData* data, uint32_t allowed, bool* coherent) {
    const VkPhysicalDeviceMemoryProperties properties = memoryProperties(data);
    // Cached memory matters: reading tens of megabytes per frame from uncached memory is very slow.
    const VkMemoryPropertyFlags preferences[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    for (VkMemoryPropertyFlags wanted : preferences) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((allowed & (1u << i)) != 0 &&
                (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
                *coherent = (properties.memoryTypes[i].propertyFlags &
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                return i;
            }
        }
    }
    return UINT32_MAX;
}

void destroyCapture(DeviceData* data, std::unique_ptr<Capture> capture) {
    if (!capture) return;
    {
        std::lock_guard<std::mutex> lock(capture->mutex);
        capture->stopping = true;
    }
    capture->wake.notify_all();
    if (capture->worker.joinable()) capture->worker.join();
    for (const Job& job : capture->pending) {
        if (job.release != nullptr) job.release->store(false);
    }
    // The copies may still be referenced by queued presents; let the device finish first.
    data->waitIdle(data->device);
    for (Slot& slot : capture->slots) {
        if (slot.copied != VK_NULL_HANDLE) data->destroySemaphore(data->device, slot.copied, nullptr);
        if (slot.fence != VK_NULL_HANDLE) data->destroyFence(data->device, slot.fence, nullptr);
        if (slot.buffer != VK_NULL_HANDLE) data->destroyBuffer(data->device, slot.buffer, nullptr);
        if (slot.memory != VK_NULL_HANDLE) data->freeMemory(data->device, slot.memory, nullptr);
    }
    if (capture->pool != VK_NULL_HANDLE) data->destroyPool(data->device, capture->pool, nullptr);
}

std::unique_ptr<Capture> createCapture(DeviceData* data, VkSwapchainKHR swapchain,
                                       const SwapchainInfo& info, uint32_t family) {
    const uint32_t format = shmFormat(info.format);
    if (format == UINT32_MAX) {
        logLine("unsupported swapchain format %d, capture disabled", static_cast<int>(info.format));
        return nullptr;
    }
    auto capture = std::make_unique<Capture>();
    capture->device = data;
    capture->swapchain = swapchain;
    capture->extent = info.extent;
    capture->format = format;
    capture->queueFamily = family;
    const VkDevice device = data->device;
    const auto fail = [&](const char* what) {
        logLine("cannot capture: %s", what);
        destroyCapture(data, std::move(capture));
        return nullptr;
    };

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family;
    if (data->createPool(device, &poolInfo, nullptr, &capture->pool) != VK_SUCCESS) {
        return fail("command pool");
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(info.extent.width) * info.extent.height * 4;
    for (Slot& slot : capture->slots) {
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = capture->pool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        if (data->allocateCommands(device, &commandInfo, &slot.commands) != VK_SUCCESS) {
            return fail("command buffer");
        }
        // Command buffers made inside a layer need the loader's dispatch pointer, or any layer
        // below this one that looks them up by dispatch key will crash.
        if (data->setLoaderData != nullptr &&
            data->setLoaderData(device, slot.commands) != VK_SUCCESS) {
            return fail("command buffer dispatch");
        }

        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (data->createBuffer(device, &bufferInfo, nullptr, &slot.buffer) != VK_SUCCESS) {
            return fail("readback buffer");
        }
        VkMemoryRequirements requirements{};
        data->bufferRequirements(device, slot.buffer, &requirements);
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = pickMemoryType(data, requirements.memoryTypeBits, &slot.coherent);
        if (allocInfo.memoryTypeIndex == UINT32_MAX ||
            data->allocateMemory(device, &allocInfo, nullptr, &slot.memory) != VK_SUCCESS ||
            data->bindBuffer(device, slot.buffer, slot.memory, 0) != VK_SUCCESS ||
            data->mapMemory(device, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped) != VK_SUCCESS) {
            return fail("readback memory");
        }
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (data->createFence(device, &fenceInfo, nullptr, &slot.fence) != VK_SUCCESS ||
            data->createSemaphore(device, &semaphoreInfo, nullptr, &slot.copied) != VK_SUCCESS) {
            return fail("synchronization objects");
        }
    }
    capture->worker = std::thread(publishWorker, capture.get());
    logLine("capturing swapchain %ux%u format %d on queue family %u", info.extent.width,
            info.extent.height, static_cast<int>(info.format), family);
    return capture;
}

// Records and submits the copy of `image`, waiting on `waits` first. The copy signals the slot's
// semaphore when `signalCopied` is set, and `fence` (or the slot's own fence when null). Returns
// the slot index, or -1 when every slot is busy or recording failed and nothing was submitted.
int submitCopy(DeviceData* data, Capture& capture, VkQueue queue, VkImage image,
               uint32_t waitCount, const VkSemaphore* waits, bool signalCopied, VkFence fence,
               std::atomic<bool>* release) {
    size_t index = kSlots;
    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        for (size_t i = 0; i < kSlots; ++i) {
            const size_t candidate = (capture.nextSlot + i) % kSlots;
            if (!capture.slots[candidate].busy) {
                index = candidate;
                break;
            }
        }
    }
    if (index == kSlots) return -1;
    Slot& slot = capture.slots[index];
    if (fence == VK_NULL_HANDLE) {
        fence = slot.fence;
        data->resetFences(data->device, 1, &fence);
    }
    data->resetCommands(slot.commands, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (data->beginCommands(slot.commands, &beginInfo) != VK_SUCCESS) return -1;

    // A presented image is always in PRESENT_SRC; the contents must be preserved.
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    data->barrier(slot.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {capture.extent.width, capture.extent.height, 1};
    data->copyImage(slot.commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.buffer, 1,
                    &region);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = 0;
    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = slot.buffer;
    toHost.size = VK_WHOLE_SIZE;
    data->barrier(slot.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                  1, &toHost, 1, &toPresent);
    if (data->endCommands(slot.commands) != VK_SUCCESS) return -1;

    // The copy consumes the semaphores the present would have waited on, so it runs after the
    // compositor's rendering.
    std::vector<VkPipelineStageFlags> stages(waitCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = waitCount;
    submitInfo.pWaitSemaphores = waits;
    submitInfo.pWaitDstStageMask = stages.data();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot.commands;
    submitInfo.signalSemaphoreCount = signalCopied ? 1 : 0;
    submitInfo.pSignalSemaphores = &slot.copied;
    if (release != nullptr) release->store(true);
    if (data->submit(queue, 1, &submitInfo, fence) != VK_SUCCESS) {
        if (release != nullptr) release->store(false);
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        slot.busy = true;
        capture.nextSlot = (index + 1) % kSlots;
        capture.pending.push_back({index, fence, release});
    }
    capture.wake.notify_one();
    return static_cast<int>(index);
}

// ---------------------------------------------------------------------------------------------
// Virtual direct mode display
// ---------------------------------------------------------------------------------------------
//
// SteamVR for Linux only renders to a display it can drive directly. With KITVR_VIRTUAL_DISPLAY=1
// the layer adds a display to VK_KHR_display enumeration, accepts every attempt to acquire it, and
// implements the swapchain on its surface itself: the images are ordinary device images, presents
// are paced to a simulated refresh and copied into the frame ring instead of reaching a screen.

struct VirtualDisplayConfig {
    bool enabled = false;
    uint32_t width = 3664;
    uint32_t height = 1920;
    uint32_t refreshMilliHz = 90000;
};

uint32_t envNumber(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return fallback;
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    return parsed > 0 && parsed < 65536 ? static_cast<uint32_t>(parsed) : fallback;
}

// Read once; vrcompositor gets these from linux/vrcompositor-wrapper.sh. The size must match what
// the SteamVR driver reports in GetWindowBounds.
const VirtualDisplayConfig& virtualDisplay() {
    static const VirtualDisplayConfig config = [] {
        VirtualDisplayConfig c;
        const char* enabled = std::getenv("KITVR_VIRTUAL_DISPLAY");
        c.enabled = enabled != nullptr && std::strcmp(enabled, "1") == 0;
        c.width = envNumber("KITVR_DISPLAY_WIDTH", c.width);
        c.height = envNumber("KITVR_DISPLAY_HEIGHT", c.height);
        c.refreshMilliHz = envNumber("KITVR_DISPLAY_HZ", c.refreshMilliHz / 1000) * 1000;
        if (c.enabled) {
            logLine("virtual display %ux%u at %u Hz", c.width, c.height, c.refreshMilliHz / 1000);
        }
        return c;
    }();
    return config;
}

// Fake handles use a non-canonical address range, so they can never collide with driver handles.
constexpr uint64_t kHandleBase = 0x4B56520000000000ull;  // "KVR"
const VkDisplayKHR kVirtualDisplay = reinterpret_cast<VkDisplayKHR>(kHandleBase | 0x1);
const VkDisplayModeKHR kVirtualMode = reinterpret_cast<VkDisplayModeKHR>(kHandleBase | 0x2);
const VkSurfaceKHR kVirtualSurface = reinterpret_cast<VkSurfaceKHR>(kHandleBase | 0x3);
std::atomic<uint64_t> g_nextVirtualSwapchain{0x100};

bool isVirtual(VkDisplayKHR display) { return virtualDisplay().enabled && display == kVirtualDisplay; }
bool isVirtual(VkDisplayModeKHR mode) { return virtualDisplay().enabled && mode == kVirtualMode; }
bool isVirtual(VkSurfaceKHR surface) { return virtualDisplay().enabled && surface == kVirtualSurface; }

uint64_t monotonicNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
}

// The simulated display refreshes on a fixed clock that starts when the layer is first used.
uint64_t refreshPeriodNs() { return 1000000000000ull / virtualDisplay().refreshMilliHz; }

uint64_t vblankEpochNs() {
    static const uint64_t epoch = monotonicNs();
    return epoch;
}

uint64_t vblankCount(uint64_t nowNs) { return (nowNs - vblankEpochNs()) / refreshPeriodNs(); }

uint64_t nextVblankNs(uint64_t nowNs) {
    return vblankEpochNs() + (vblankCount(nowNs) + 1) * refreshPeriodNs();
}

void sleepUntilNs(uint64_t deadline) {
    const uint64_t now = monotonicNs();
    if (deadline <= now) return;
    const uint64_t wait = deadline - now;
    timespec duration{static_cast<time_t>(wait / 1000000000ull), static_cast<long>(wait % 1000000000ull)};
    nanosleep(&duration, nullptr);
}

struct VirtualImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFence released = VK_NULL_HANDLE;  // signals when the present that used the image finished
    bool submitted = false;              // `released` has a pending or finished submission
    bool acquired = false;
    uint64_t availableNs = 0;            // when the next frame replaces it on the simulated screen
    std::atomic<bool> copying{false};    // the frame ring worker still reads its copy
};

struct VirtualSwapchain {
    DeviceData* device = nullptr;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<std::unique_ptr<VirtualImage>> images;
    size_t nextImage = 0;
    VkQueue queue = VK_NULL_HANDLE;  // queue for signalling acquires, the last one presented on
    uint64_t lastScanoutNs = 0;      // refresh that shows the most recent present
    std::mutex mutex;
};

std::mutex g_virtualMutex;
std::unordered_map<VkSwapchainKHR, std::unique_ptr<VirtualSwapchain>> g_virtualSwapchains;

VirtualSwapchain* findVirtualSwapchain(VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(g_virtualMutex);
    auto it = g_virtualSwapchains.find(swapchain);
    return it == g_virtualSwapchains.end() ? nullptr : it->second.get();
}

VkQueue anyQueue(DeviceData* data) {
    std::lock_guard<std::mutex> lock(g_devicesMutex);
    for (const auto& entry : g_queues) {
        if (entry.second.first == data) return entry.first;
    }
    return VK_NULL_HANDLE;
}

void destroyVirtualImages(DeviceData* data, VirtualSwapchain& swapchain) {
    data->waitIdle(data->device);
    for (auto& image : swapchain.images) {
        if (image->released != VK_NULL_HANDLE) data->destroyFence(data->device, image->released, nullptr);
        if (image->image != VK_NULL_HANDLE) data->destroyImage(data->device, image->image, nullptr);
        if (image->memory != VK_NULL_HANDLE) data->freeMemory(data->device, image->memory, nullptr);
    }
    swapchain.images.clear();
}

template <typename T>
const T* findChained(const void* next, VkStructureType type) {
    for (auto* chain = reinterpret_cast<const VkBaseInStructure*>(next); chain != nullptr;
         chain = chain->pNext) {
        if (chain->sType == type) return reinterpret_cast<const T*>(chain);
    }
    return nullptr;
}

VkResult createVirtualSwapchain(DeviceData* data, const VkSwapchainCreateInfoKHR* info,
                                VkSwapchainKHR* handle) {
    auto swapchain = std::make_unique<VirtualSwapchain>();
    swapchain->device = data;
    swapchain->extent = info->imageExtent;
    swapchain->format = info->imageFormat;
    swapchain->handle = reinterpret_cast<VkSwapchainKHR>(kHandleBase | g_nextVirtualSwapchain++);

    // The compositor creates views in other formats when it asks for a mutable format swapchain.
    VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
        imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (const auto* list = findChained<VkImageFormatListCreateInfo>(
                info->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)) {
            formatList.viewFormatCount = list->viewFormatCount;
            formatList.pViewFormats = list->pViewFormats;
            imageInfo.pNext = &formatList;
        }
    }
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = info->imageFormat;
    imageInfo.extent = {info->imageExtent.width, info->imageExtent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = std::max(1u, info->imageArrayLayers);
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = info->imageSharingMode;
    imageInfo.queueFamilyIndexCount = info->queueFamilyIndexCount;
    imageInfo.pQueueFamilyIndices = info->pQueueFamilyIndices;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const VkPhysicalDeviceMemoryProperties memory = memoryProperties(data);
    const uint32_t count = std::max(3u, info->minImageCount);
    for (uint32_t i = 0; i < count; ++i) {
        auto image = std::make_unique<VirtualImage>();
        VkResult result = data->createImage(data->device, &imageInfo, nullptr, &image->image);
        if (result != VK_SUCCESS) {
            logLine("virtual swapchain image creation failed (%d)", static_cast<int>(result));
            swapchain->images.push_back(std::move(image));
            destroyVirtualImages(data, *swapchain);
            return result;
        }
        VkMemoryRequirements requirements{};
        data->imageRequirements(data->device, image->image, &requirements);
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = UINT32_MAX;
        for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
            if ((requirements.memoryTypeBits & (1u << type)) &&
                (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                allocInfo.memoryTypeIndex = type;
                break;
            }
        }
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = image->image;
        allocInfo.pNext = &dedicated;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (allocInfo.memoryTypeIndex == UINT32_MAX ||
            (result = data->allocateMemory(data->device, &allocInfo, nullptr, &image->memory)) != VK_SUCCESS ||
            (result = data->bindImage(data->device, image->image, image->memory, 0)) != VK_SUCCESS ||
            (result = data->createFence(data->device, &fenceInfo, nullptr, &image->released)) != VK_SUCCESS) {
            logLine("virtual swapchain memory setup failed (%d)", static_cast<int>(result));
            swapchain->images.push_back(std::move(image));
            destroyVirtualImages(data, *swapchain);
            return result == VK_SUCCESS ? VK_ERROR_OUT_OF_DEVICE_MEMORY : result;
        }
        swapchain->images.push_back(std::move(image));
    }
    // A replaced swapchain stays valid until destroyed, but the compositor stops presenting to it.
    *handle = swapchain->handle;
    logLine("virtual swapchain %ux%u format %d with %u images", info->imageExtent.width,
            info->imageExtent.height, static_cast<int>(info->imageFormat), count);
    std::lock_guard<std::mutex> lock(g_virtualMutex);
    g_virtualSwapchains[*handle] = std::move(swapchain);
    return VK_SUCCESS;
}

void destroyVirtualSwapchain(DeviceData* data, VkSwapchainKHR handle) {
    std::unique_ptr<VirtualSwapchain> swapchain;
    {
        std::lock_guard<std::mutex> lock(g_virtualMutex);
        auto it = g_virtualSwapchains.find(handle);
        if (it == g_virtualSwapchains.end()) return;
        swapchain = std::move(it->second);
        g_virtualSwapchains.erase(it);
    }
    std::unique_ptr<Capture> capture;
    {
        std::lock_guard<std::mutex> lock(data->mutex);
        if (data->capture && data->capture->swapchain == handle) capture = std::move(data->capture);
    }
    destroyCapture(data, std::move(capture));
    destroyVirtualImages(data, *swapchain);
}

// Signals an acquire's semaphore and fence with an empty submission, as a real driver would once
// the image is no longer being scanned out.
VkResult signalAcquire(DeviceData* data, VkQueue queue, VkSemaphore semaphore, VkFence fence) {
    if (semaphore == VK_NULL_HANDLE && fence == VK_NULL_HANDLE) return VK_SUCCESS;
    if (queue == VK_NULL_HANDLE) return VK_ERROR_DEVICE_LOST;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.signalSemaphoreCount = semaphore != VK_NULL_HANDLE ? 1 : 0;
    submit.pSignalSemaphores = &semaphore;
    return data->submit(queue, 1, &submit, fence);
}

VkResult acquireVirtual(DeviceData* data, VirtualSwapchain& swapchain, uint64_t timeout,
                        VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    const uint64_t start = monotonicNs();
    const uint64_t deadline = timeout > UINT64_MAX - start ? UINT64_MAX : start + timeout;
    for (;;) {
        uint64_t availableNs = 0;
        bool found = false;
        VkQueue queue = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(swapchain.mutex);
            const size_t count = swapchain.images.size();
            for (size_t i = 0; i < count && !found; ++i) {
                const size_t candidate = (swapchain.nextImage + i) % count;
                VirtualImage& image = *swapchain.images[candidate];
                if (image.acquired || image.copying.load()) continue;
                if (image.submitted &&
                    data->fenceStatus(data->device, image.released) != VK_SUCCESS) {
                    continue;
                }
                image.acquired = true;
                availableNs = image.availableNs;
                swapchain.nextImage = (candidate + 1) % count;
                *index = static_cast<uint32_t>(candidate);
                found = true;
            }
            queue = swapchain.queue != VK_NULL_HANDLE ? swapchain.queue : anyQueue(data);
        }
        if (found) {
            // FIFO behaviour: an image comes back once the next frame has replaced it on screen.
            sleepUntilNs(std::min(availableNs, deadline));
            return signalAcquire(data, queue, semaphore, fence);
        }
        if (timeout == 0) return VK_NOT_READY;
        if (monotonicNs() >= deadline) return VK_TIMEOUT;
        sleepUntilNs(std::min<uint64_t>(monotonicNs() + 500000ull, deadline));
    }
}

// Retires the images of `swapchains` that are virtual. The first submission consumes the
// present's wait semaphores; the copy into the frame ring rides along when a slot is free.
void presentVirtual(DeviceData* data, uint32_t family, VkQueue queue, const VkPresentInfoKHR* info,
                    bool waitsConsumed) {
    uint32_t waitCount = waitsConsumed ? 0 : info->waitSemaphoreCount;
    const VkSemaphore* waits = info->pWaitSemaphores;
    const uint64_t now = monotonicNs();
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        VirtualSwapchain* swapchain = findVirtualSwapchain(info->pSwapchains[i]);
        if (swapchain == nullptr) continue;
        const uint32_t index = info->pImageIndices[i];
        VkResult result = VK_ERROR_OUT_OF_DATE_KHR;
        if (index < swapchain->images.size()) {
            VirtualImage& image = *swapchain->images[index];
            data->resetFences(data->device, 1, &image.released);

            Capture* capture = nullptr;
            {
                std::lock_guard<std::mutex> lock(data->mutex);
                if (!data->capture || data->capture->swapchain != swapchain->handle) {
                    destroyCapture(data, std::move(data->capture));
                    SwapchainInfo record;
                    record.extent = swapchain->extent;
                    record.format = swapchain->format;
                    for (auto& entry : swapchain->images) record.images.push_back(entry->image);
                    data->capture = createCapture(data, swapchain->handle, record, family);
                }
                capture = data->capture.get();
            }
            int slot = -1;
            if (capture != nullptr && capture->queueFamily == family) {
                slot = submitCopy(data, *capture, queue, image.image, waitCount, waits, false,
                                  image.released, &image.copying);
            }
            if (slot < 0) {
                std::vector<VkPipelineStageFlags> stages(waitCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.waitSemaphoreCount = waitCount;
                submit.pWaitSemaphores = waits;
                submit.pWaitDstStageMask = stages.data();
                result = data->submit(queue, 1, &submit, image.released);
            } else {
                result = VK_SUCCESS;
            }
            waitCount = 0;
            // FIFO: each present is shown one refresh after the previous one, and stays on screen
            // for one refresh.
            std::lock_guard<std::mutex> lock(swapchain->mutex);
            const uint64_t scanout = std::max(nextVblankNs(now),
                                              swapchain->lastScanoutNs + refreshPeriodNs());
            swapchain->lastScanoutNs = scanout;
            image.submitted = result == VK_SUCCESS;
            image.acquired = false;
            image.availableNs = scanout + refreshPeriodNs();
            swapchain->queue = queue;
        }
        if (info->pResults != nullptr) info->pResults[i] = result;
    }
}

// ---------------------------------------------------------------------------------------------
// Device hooks
// ---------------------------------------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL hookGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index,
                                              VkQueue* queue) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return;
    data->getQueue(device, family, index, queue);
    rememberQueue(data, *queue, family);
}

VKAPI_ATTR void VKAPI_CALL hookGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info,
                                               VkQueue* queue) {
    DeviceData* data = findDevice(device);
    if (data == nullptr || data->getQueue2 == nullptr) return;
    data->getQueue2(device, info, queue);
    rememberQueue(data, *queue, info->queueFamilyIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateSwapchainKHR(VkDevice device,
                                                     const VkSwapchainCreateInfoKHR* createInfo,
                                                     const VkAllocationCallbacks* allocator,
                                                     VkSwapchainKHR* swapchain) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (isVirtual(createInfo->surface)) {
        return data->complete ? createVirtualSwapchain(data, createInfo, swapchain)
                              : VK_ERROR_INITIALIZATION_FAILED;
    }

    // vkCmdCopyImageToBuffer needs TRANSFER_SRC on the image, which vrcompositor does not request.
    VkSwapchainCreateInfoKHR info = *createInfo;
    if (data->complete) info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult result = data->createSwapchain(device, &info, allocator, swapchain);
    if (result != VK_SUCCESS && info.imageUsage != createInfo->imageUsage) {
        // Fall back to the compositor's own request rather than failing its window.
        logLine("swapchain rejected TRANSFER_SRC usage, this swapchain will not be captured");
        return data->createSwapchain(device, createInfo, allocator, swapchain);
    }
    if (result != VK_SUCCESS) return result;

    SwapchainInfo record;
    record.extent = info.imageExtent;
    record.format = info.imageFormat;
    uint32_t count = 0;
    if (data->swapchainImages(device, *swapchain, &count, nullptr) == VK_SUCCESS && count > 0) {
        record.images.resize(count);
        if (data->swapchainImages(device, *swapchain, &count, record.images.data()) != VK_SUCCESS) {
            record.images.clear();
        }
    }
    logLine("swapchain %ux%u format %d with %u images", record.extent.width, record.extent.height,
            static_cast<int>(record.format), count);
    if (data->complete && !record.images.empty()) {
        std::lock_guard<std::mutex> lock(data->mutex);
        data->swapchains[*swapchain] = std::move(record);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hookDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                   const VkAllocationCallbacks* allocator) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return;
    if (findVirtualSwapchain(swapchain) != nullptr) {
        destroyVirtualSwapchain(data, swapchain);
        return;
    }
    std::unique_ptr<Capture> capture;
    {
        std::lock_guard<std::mutex> lock(data->mutex);
        data->swapchains.erase(swapchain);
        if (data->capture && data->capture->swapchain == swapchain) capture = std::move(data->capture);
    }
    destroyCapture(data, std::move(capture));
    data->destroySwapchain(device, swapchain, allocator);
}

// Captures the largest swapchain in the present, which is the HMD window rather than a mirror.
// Returns the semaphore the present must wait on instead of its own, or VK_NULL_HANDLE.
VkSemaphore captureForPresent(DeviceData* data, uint32_t family, VkQueue queue,
                              const VkPresentInfoKHR* info) {
    std::lock_guard<std::mutex> lock(data->mutex);
    uint32_t chosen = UINT32_MAX;
    uint64_t chosenArea = 0;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        auto it = data->swapchains.find(info->pSwapchains[i]);
        if (it == data->swapchains.end()) continue;
        const uint64_t area = static_cast<uint64_t>(it->second.extent.width) * it->second.extent.height;
        if (area > chosenArea) {
            chosen = i;
            chosenArea = area;
        }
    }
    if (chosen == UINT32_MAX) return VK_NULL_HANDLE;
    const VkSwapchainKHR swapchain = info->pSwapchains[chosen];

    // Switch to this swapchain when nothing is captured yet or it is bigger than the current one.
    Capture* capture = data->capture.get();
    if (capture == nullptr || (capture->swapchain != swapchain &&
                               chosenArea > static_cast<uint64_t>(capture->extent.width) *
                                                capture->extent.height)) {
        destroyCapture(data, std::move(data->capture));
        data->capture = createCapture(data, swapchain, data->swapchains[swapchain], family);
        if (!data->capture) {
            // Do not retry every frame on the same swapchain.
            data->swapchains.erase(swapchain);
            return VK_NULL_HANDLE;
        }
        capture = data->capture.get();
    }
    const SwapchainInfo& record = data->swapchains[swapchain];
    const uint32_t imageIndex = info->pImageIndices[chosen];
    if (capture->swapchain != swapchain || capture->queueFamily != family ||
        imageIndex >= record.images.size()) {
        return VK_NULL_HANDLE;
    }
    // The present waits on the copy instead of the compositor's semaphores.
    const int slot = submitCopy(data, *capture, queue, record.images[imageIndex],
                                info->waitSemaphoreCount, info->pWaitSemaphores, true,
                                VK_NULL_HANDLE, nullptr);
    return slot < 0 ? VK_NULL_HANDLE : capture->slots[slot].copied;
}

VKAPI_ATTR VkResult VKAPI_CALL hookQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    DeviceData* data = nullptr;
    uint32_t family = UINT32_MAX;
    if (!findQueue(queue, &data, &family)) {
        // The queue was fetched before the layer could see it; any device will do for dispatch,
        // but nothing is captured.
        std::lock_guard<std::mutex> lock(g_devicesMutex);
        if (g_devices.empty()) return VK_ERROR_DEVICE_LOST;
        data = g_devices.begin()->second;
    }
    uint32_t virtualCount = 0;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        if (findVirtualSwapchain(info->pSwapchains[i]) != nullptr) ++virtualCount;
    }
    if (virtualCount == info->swapchainCount) {
        presentVirtual(data, family, queue, info, false);
        return VK_SUCCESS;
    }
    if (virtualCount > 0) {
        // Not expected from vrcompositor: the real swapchains keep the semaphores and the virtual
        // images are retired without waiting for their rendering.
        static std::once_flag warned;
        std::call_once(warned, [] { logLine("mixed virtual and real present, capture may tear"); });
        presentVirtual(data, family, queue, info, true);
        std::vector<VkSwapchainKHR> swapchains;
        std::vector<uint32_t> indices;
        std::vector<uint32_t> positions;
        for (uint32_t i = 0; i < info->swapchainCount; ++i) {
            if (findVirtualSwapchain(info->pSwapchains[i]) != nullptr) continue;
            swapchains.push_back(info->pSwapchains[i]);
            indices.push_back(info->pImageIndices[i]);
            positions.push_back(i);
        }
        std::vector<VkResult> results(swapchains.size(), VK_SUCCESS);
        VkPresentInfoKHR forwarded = *info;
        forwarded.pNext = nullptr;  // per swapchain extension structs no longer line up
        forwarded.swapchainCount = static_cast<uint32_t>(swapchains.size());
        forwarded.pSwapchains = swapchains.data();
        forwarded.pImageIndices = indices.data();
        forwarded.pResults = results.data();
        const VkResult result = data->present(queue, &forwarded);
        if (info->pResults != nullptr) {
            for (size_t i = 0; i < positions.size(); ++i) info->pResults[positions[i]] = results[i];
        }
        return result;
    }
    if (family == UINT32_MAX || !data->complete) return data->present(queue, info);
    const VkSemaphore copied = captureForPresent(data, family, queue, info);
    if (copied == VK_NULL_HANDLE) return data->present(queue, info);
    VkPresentInfoKHR forwarded = *info;
    forwarded.waitSemaphoreCount = 1;
    forwarded.pWaitSemaphores = &copied;
    return data->present(queue, &forwarded);
}

template <typename T>
T nextDevice(DeviceData* data, const char* name) {
    return reinterpret_cast<T>(data->next(data->device, name));
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                        uint32_t* count, VkImage* images) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    VirtualSwapchain* virt = findVirtualSwapchain(swapchain);
    if (virt == nullptr) return data->swapchainImages(device, swapchain, count, images);
    const uint32_t total = static_cast<uint32_t>(virt->images.size());
    if (images == nullptr) {
        *count = total;
        return VK_SUCCESS;
    }
    const uint32_t written = std::min(*count, total);
    for (uint32_t i = 0; i < written; ++i) images[i] = virt->images[i]->image;
    *count = written;
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL hookAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                      uint64_t timeout, VkSemaphore semaphore,
                                                      VkFence fence, uint32_t* index) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (VirtualSwapchain* virt = findVirtualSwapchain(swapchain)) {
        return acquireVirtual(data, *virt, timeout, semaphore, fence, index);
    }
    auto next = nextDevice<PFN_vkAcquireNextImageKHR>(data, "vkAcquireNextImageKHR");
    return next(device, swapchain, timeout, semaphore, fence, index);
}

VKAPI_ATTR VkResult VKAPI_CALL hookAcquireNextImage2KHR(VkDevice device,
                                                       const VkAcquireNextImageInfoKHR* info,
                                                       uint32_t* index) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (VirtualSwapchain* virt = findVirtualSwapchain(info->swapchain)) {
        return acquireVirtual(data, *virt, info->timeout, info->semaphore, info->fence, index);
    }
    auto next = nextDevice<PFN_vkAcquireNextImage2KHR>(data, "vkAcquireNextImage2KHR");
    return next(device, info, index);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR swapchain) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (findVirtualSwapchain(swapchain) != nullptr) return VK_SUCCESS;
    return nextDevice<PFN_vkGetSwapchainStatusKHR>(data, "vkGetSwapchainStatusKHR")(device, swapchain);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetSwapchainCounterEXT(VkDevice device, VkSwapchainKHR swapchain,
                                                         VkSurfaceCounterFlagBitsEXT counter,
                                                         uint64_t* value) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (findVirtualSwapchain(swapchain) != nullptr) {
        *value = vblankCount(monotonicNs());
        return VK_SUCCESS;
    }
    return nextDevice<PFN_vkGetSwapchainCounterEXT>(data, "vkGetSwapchainCounterEXT")(
        device, swapchain, counter, value);
}

VKAPI_ATTR VkResult VKAPI_CALL hookDisplayPowerControlEXT(VkDevice device, VkDisplayKHR display,
                                                         const VkDisplayPowerInfoEXT* info) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (isVirtual(display)) return VK_SUCCESS;
    return nextDevice<PFN_vkDisplayPowerControlEXT>(data, "vkDisplayPowerControlEXT")(device, display, info);
}

// vrcompositor times its frames from the first-pixel-out event. The fence is created signalled
// after sleeping until the next simulated refresh, which is when a real display would signal it.
VKAPI_ATTR VkResult VKAPI_CALL hookRegisterDisplayEventEXT(VkDevice device, VkDisplayKHR display,
                                                          const VkDisplayEventInfoEXT* info,
                                                          const VkAllocationCallbacks* allocator,
                                                          VkFence* fence) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (!isVirtual(display)) {
        return nextDevice<PFN_vkRegisterDisplayEventEXT>(data, "vkRegisterDisplayEventEXT")(
            device, display, info, allocator, fence);
    }
    sleepUntilNs(nextVblankNs(monotonicNs()));
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return data->createFence(device, &fenceInfo, allocator, fence);
}

// Hotplug never happens on the virtual display: hand back a fence that never signals.
VKAPI_ATTR VkResult VKAPI_CALL hookRegisterDeviceEventEXT(VkDevice device,
                                                         const VkDeviceEventInfoEXT* info,
                                                         const VkAllocationCallbacks* allocator,
                                                         VkFence* fence) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (!virtualDisplay().enabled) {
        return nextDevice<PFN_vkRegisterDeviceEventEXT>(data, "vkRegisterDeviceEventEXT")(
            device, info, allocator, fence);
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return data->createFence(device, &fenceInfo, allocator, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL hookWaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    uint64_t presentId, uint64_t timeout) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (findVirtualSwapchain(swapchain) != nullptr) return VK_SUCCESS;
    return nextDevice<PFN_vkWaitForPresentKHR>(data, "vkWaitForPresentKHR")(
        device, swapchain, presentId, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetRefreshCycleDurationGOOGLE(
    VkDevice device, VkSwapchainKHR swapchain, VkRefreshCycleDurationGOOGLE* properties) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (findVirtualSwapchain(swapchain) != nullptr) {
        properties->refreshDuration = refreshPeriodNs();
        return VK_SUCCESS;
    }
    return nextDevice<PFN_vkGetRefreshCycleDurationGOOGLE>(data, "vkGetRefreshCycleDurationGOOGLE")(
        device, swapchain, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPastPresentationTimingGOOGLE(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t* count,
    VkPastPresentationTimingGOOGLE* timings) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (findVirtualSwapchain(swapchain) != nullptr) {
        *count = 0;
        return VK_SUCCESS;
    }
    return nextDevice<PFN_vkGetPastPresentationTimingGOOGLE>(
        data, "vkGetPastPresentationTimingGOOGLE")(device, swapchain, count, timings);
}

VKAPI_ATTR void VKAPI_CALL hookSetHdrMetadataEXT(VkDevice device, uint32_t count,
                                                 const VkSwapchainKHR* swapchains,
                                                 const VkHdrMetadataEXT* metadata) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return;
    std::vector<VkSwapchainKHR> real;
    std::vector<VkHdrMetadataEXT> realMetadata;
    for (uint32_t i = 0; i < count; ++i) {
        if (findVirtualSwapchain(swapchains[i]) != nullptr) continue;
        real.push_back(swapchains[i]);
        realMetadata.push_back(metadata[i]);
    }
    if (real.empty()) return;
    nextDevice<PFN_vkSetHdrMetadataEXT>(data, "vkSetHdrMetadataEXT")(
        device, static_cast<uint32_t>(real.size()), real.data(), realMetadata.data());
}

VKAPI_ATTR void VKAPI_CALL hookDestroyDevice(VkDevice device,
                                             const VkAllocationCallbacks* allocator) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return;
    std::unique_ptr<Capture> capture;
    {
        std::lock_guard<std::mutex> lock(data->mutex);
        capture = std::move(data->capture);
    }
    destroyCapture(data, std::move(capture));
    {
        std::lock_guard<std::mutex> lock(g_devicesMutex);
        g_devices.erase(device);
        for (auto it = g_queues.begin(); it != g_queues.end();) {
            it = it->second.first == data ? g_queues.erase(it) : std::next(it);
        }
    }
    data->destroy(device, allocator);
    delete data;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGetDeviceProcAddr(VkDevice device, const char* name);

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(VkPhysicalDevice physical,
                                                const VkDeviceCreateInfo* createInfo,
                                                const VkAllocationCallbacks* allocator,
                                                VkDevice* device) {
    auto* link = findLinkInfo<VkLayerDeviceCreateInfo>(
        createInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);
    auto* loaderData = findLinkInfo<VkLayerDeviceCreateInfo>(
        createInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LOADER_DATA_CALLBACK);
    if (link == nullptr || link->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr nextGipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr nextGdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    InstanceData* instance = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_physicalMutex);
        auto it = g_physicalInstances.find(physical);
        if (it != g_physicalInstances.end()) instance = it->second;
    }
    if (instance == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    auto nextCreate = reinterpret_cast<PFN_vkCreateDevice>(nextGipa(instance->instance, "vkCreateDevice"));
    if (nextCreate == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = nextCreate(physical, createInfo, allocator, device);
    if (result != VK_SUCCESS) return result;

    auto* data = new DeviceData();
    data->device = *device;
    data->physical = physical;
    data->instance = instance;
    data->next = nextGdpa;
    data->setLoaderData = loaderData != nullptr ? loaderData->u.pfnSetDeviceLoaderData : nullptr;
#define KITVR_LOAD(member, name) data->member = reinterpret_cast<PFN_##name>(nextGdpa(*device, #name))
    KITVR_LOAD(destroy, vkDestroyDevice);
    KITVR_LOAD(getQueue, vkGetDeviceQueue);
    KITVR_LOAD(getQueue2, vkGetDeviceQueue2);
    KITVR_LOAD(createSwapchain, vkCreateSwapchainKHR);
    KITVR_LOAD(destroySwapchain, vkDestroySwapchainKHR);
    KITVR_LOAD(swapchainImages, vkGetSwapchainImagesKHR);
    KITVR_LOAD(present, vkQueuePresentKHR);
    KITVR_LOAD(createPool, vkCreateCommandPool);
    KITVR_LOAD(destroyPool, vkDestroyCommandPool);
    KITVR_LOAD(allocateCommands, vkAllocateCommandBuffers);
    KITVR_LOAD(resetCommands, vkResetCommandBuffer);
    KITVR_LOAD(beginCommands, vkBeginCommandBuffer);
    KITVR_LOAD(endCommands, vkEndCommandBuffer);
    KITVR_LOAD(barrier, vkCmdPipelineBarrier);
    KITVR_LOAD(copyImage, vkCmdCopyImageToBuffer);
    KITVR_LOAD(createBuffer, vkCreateBuffer);
    KITVR_LOAD(destroyBuffer, vkDestroyBuffer);
    KITVR_LOAD(bufferRequirements, vkGetBufferMemoryRequirements);
    KITVR_LOAD(allocateMemory, vkAllocateMemory);
    KITVR_LOAD(freeMemory, vkFreeMemory);
    KITVR_LOAD(bindBuffer, vkBindBufferMemory);
    KITVR_LOAD(mapMemory, vkMapMemory);
    KITVR_LOAD(invalidate, vkInvalidateMappedMemoryRanges);
    KITVR_LOAD(submit, vkQueueSubmit);
    KITVR_LOAD(createFence, vkCreateFence);
    KITVR_LOAD(destroyFence, vkDestroyFence);
    KITVR_LOAD(waitForFences, vkWaitForFences);
    KITVR_LOAD(resetFences, vkResetFences);
    KITVR_LOAD(fenceStatus, vkGetFenceStatus);
    KITVR_LOAD(createImage, vkCreateImage);
    KITVR_LOAD(destroyImage, vkDestroyImage);
    KITVR_LOAD(imageRequirements, vkGetImageMemoryRequirements);
    KITVR_LOAD(bindImage, vkBindImageMemory);
    KITVR_LOAD(createSemaphore, vkCreateSemaphore);
    KITVR_LOAD(destroySemaphore, vkDestroySemaphore);
    KITVR_LOAD(waitIdle, vkDeviceWaitIdle);
#undef KITVR_LOAD
    data->complete = data->createSwapchain && data->destroySwapchain && data->swapchainImages &&
                     data->present && data->createPool && data->destroyPool &&
                     data->allocateCommands && data->resetCommands && data->beginCommands &&
                     data->endCommands && data->barrier && data->copyImage && data->createBuffer &&
                     data->destroyBuffer && data->bufferRequirements && data->allocateMemory &&
                     data->freeMemory && data->bindBuffer && data->mapMemory && data->invalidate &&
                     data->submit && data->createFence && data->destroyFence &&
                     data->waitForFences && data->resetFences && data->createSemaphore &&
                     data->destroySemaphore && data->waitIdle && data->getQueue &&
                     data->fenceStatus && data->createImage && data->destroyImage &&
                     data->imageRequirements && data->bindImage;
    if (!data->complete) logLine("device is missing entry points, capture disabled");

    {
        std::lock_guard<std::mutex> lock(g_devicesMutex);
        g_devices[*device] = data;
    }
    // Learn every queue up front, since vkQueuePresentKHR only identifies the queue.
    for (uint32_t i = 0; i < createInfo->queueCreateInfoCount; ++i) {
        const VkDeviceQueueCreateInfo& queueInfo = createInfo->pQueueCreateInfos[i];
        for (uint32_t q = 0; q < queueInfo.queueCount; ++q) {
            VkQueue queue = VK_NULL_HANDLE;
            if (queueInfo.flags != 0) {
                if (data->getQueue2 == nullptr) continue;
                VkDeviceQueueInfo2 info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
                info.flags = queueInfo.flags;
                info.queueFamilyIndex = queueInfo.queueFamilyIndex;
                info.queueIndex = q;
                data->getQueue2(*device, &info, &queue);
            } else if (data->getQueue != nullptr) {
                data->getQueue(*device, queueInfo.queueFamilyIndex, q, &queue);
            }
            rememberQueue(data, queue, queueInfo.queueFamilyIndex);
        }
    }
    KITVR_LOG("device %p created", reinterpret_cast<void*>(*device));
    return VK_SUCCESS;
}

#define KITVR_HOOK(api, hook) \
    if (std::strcmp(name, #api) == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook)

// Device functions the layer intercepts, also used when they are requested through the instance.
PFN_vkVoidFunction deviceHook(const char* name) {
    KITVR_HOOK(vkGetDeviceProcAddr, hookGetDeviceProcAddr);
    KITVR_HOOK(vkDestroyDevice, hookDestroyDevice);
    KITVR_HOOK(vkGetDeviceQueue, hookGetDeviceQueue);
    KITVR_HOOK(vkCreateSwapchainKHR, hookCreateSwapchainKHR);
    KITVR_HOOK(vkDestroySwapchainKHR, hookDestroySwapchainKHR);
    KITVR_HOOK(vkQueuePresentKHR, hookQueuePresentKHR);
    if (!virtualDisplay().enabled) return nullptr;
    KITVR_HOOK(vkGetSwapchainImagesKHR, hookGetSwapchainImagesKHR);
    KITVR_HOOK(vkAcquireNextImageKHR, hookAcquireNextImageKHR);
    KITVR_HOOK(vkAcquireNextImage2KHR, hookAcquireNextImage2KHR);
    KITVR_HOOK(vkGetSwapchainStatusKHR, hookGetSwapchainStatusKHR);
    KITVR_HOOK(vkGetSwapchainCounterEXT, hookGetSwapchainCounterEXT);
    KITVR_HOOK(vkDisplayPowerControlEXT, hookDisplayPowerControlEXT);
    KITVR_HOOK(vkRegisterDisplayEventEXT, hookRegisterDisplayEventEXT);
    KITVR_HOOK(vkRegisterDeviceEventEXT, hookRegisterDeviceEventEXT);
    KITVR_HOOK(vkWaitForPresentKHR, hookWaitForPresentKHR);
    KITVR_HOOK(vkGetRefreshCycleDurationGOOGLE, hookGetRefreshCycleDurationGOOGLE);
    KITVR_HOOK(vkGetPastPresentationTimingGOOGLE, hookGetPastPresentationTimingGOOGLE);
    KITVR_HOOK(vkSetHdrMetadataEXT, hookSetHdrMetadataEXT);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGetDeviceProcAddr(VkDevice device, const char* name) {
    if (name == nullptr) return nullptr;
    if (PFN_vkVoidFunction hook = deviceHook(name)) return hook;
    DeviceData* data = findDevice(device);
    if (std::strcmp(name, "vkGetDeviceQueue2") == 0) {
        // Only intercept it when the driver has it, so the application still sees null otherwise.
        return data != nullptr && data->getQueue2 != nullptr
                   ? reinterpret_cast<PFN_vkVoidFunction>(hookGetDeviceQueue2)
                   : nullptr;
    }
    return data != nullptr && data->next != nullptr ? data->next(device, name) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Instance hooks
// ---------------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL hookEnumeratePhysicalDevices(VkInstance instance, uint32_t* count,
                                                           VkPhysicalDevice* devices) {
    InstanceData* data = findInstance(instance);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto next = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        data->next(instance, "vkEnumeratePhysicalDevices"));
    if (next == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = next(instance, count, devices);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && devices != nullptr) {
        std::lock_guard<std::mutex> lock(g_physicalMutex);
        for (uint32_t i = 0; i < *count; ++i) g_physicalInstances[devices[i]] = data;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateInstance(const VkInstanceCreateInfo* createInfo,
                                                  const VkAllocationCallbacks* allocator,
                                                  VkInstance* instance) {
    auto* link = findLinkInfo<VkLayerInstanceCreateInfo>(
        createInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, VK_LAYER_LINK_INFO);
    if (link == nullptr || link->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    g_globalGipa = next;

    auto nextCreate = reinterpret_cast<PFN_vkCreateInstance>(next(nullptr, "vkCreateInstance"));
    if (nextCreate == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = nextCreate(createInfo, allocator, instance);
    if (result != VK_SUCCESS) return result;

    auto* data = new InstanceData();
    data->instance = *instance;
    data->next = next;
    data->destroy = reinterpret_cast<PFN_vkDestroyInstance>(next(*instance, "vkDestroyInstance"));
    {
        std::lock_guard<std::mutex> lock(g_instancesMutex);
        g_instances[*instance] = data;
    }
    KITVR_LOG("instance %p created", reinterpret_cast<void*>(*instance));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hookDestroyInstance(VkInstance instance,
                                               const VkAllocationCallbacks* allocator) {
    InstanceData* data = findInstance(instance);
    if (data == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(g_instancesMutex);
        g_instances.erase(instance);
    }
    {
        std::lock_guard<std::mutex> lock(g_physicalMutex);
        for (auto it = g_physicalInstances.begin(); it != g_physicalInstances.end();) {
            it = it->second == data ? g_physicalInstances.erase(it) : std::next(it);
        }
    }
    if (data->destroy != nullptr) data->destroy(instance, allocator);
    delete data;
}

VKAPI_ATTR VkResult VKAPI_CALL hookEnumerateInstanceLayerProperties(uint32_t* count,
                                                                   VkLayerProperties* properties) {
    if (count == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (properties == nullptr) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count < 1) return VK_INCOMPLETE;
    VkLayerProperties entry{};
    std::snprintf(entry.layerName, sizeof(entry.layerName), "VK_LAYER_CKS_kitvr");
    entry.specVersion = VK_API_VERSION_1_1;
    entry.implementationVersion = 2;
    std::snprintf(entry.description, sizeof(entry.description),
                  "KitVR SteamVR frame capture (publishes to shared memory)");
    properties[0] = entry;
    *count = 1;
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// Virtual display: instance hooks
// ---------------------------------------------------------------------------------------------

template <typename T>
T nextPhysical(VkPhysicalDevice physical, const char* name) {
    InstanceData* instance = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_physicalMutex);
        auto it = g_physicalInstances.find(physical);
        if (it != g_physicalInstances.end()) instance = it->second;
    }
    return instance == nullptr ? nullptr
                               : reinterpret_cast<T>(instance->next(instance->instance, name));
}

#define KITVR_NEXT(physical, name) nextPhysical<PFN_##name>(physical, #name)

VkDisplayPropertiesKHR virtualDisplayProperties() {
    VkDisplayPropertiesKHR properties{};
    properties.display = kVirtualDisplay;
    properties.displayName = "KitVR virtual HMD";
    properties.physicalDimensions = {120, 63};
    properties.physicalResolution = {virtualDisplay().width, virtualDisplay().height};
    properties.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    properties.planeReorderPossible = VK_FALSE;
    properties.persistentContent = VK_FALSE;
    return properties;
}

VkDisplayModePropertiesKHR virtualModeProperties() {
    VkDisplayModePropertiesKHR properties{};
    properties.displayMode = kVirtualMode;
    properties.parameters.visibleRegion = {virtualDisplay().width, virtualDisplay().height};
    properties.parameters.refreshRate = virtualDisplay().refreshMilliHz;
    return properties;
}

VkDisplayPlaneCapabilitiesKHR virtualPlaneCapabilities() {
    const VkExtent2D extent{virtualDisplay().width, virtualDisplay().height};
    VkDisplayPlaneCapabilitiesKHR capabilities{};
    capabilities.supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    capabilities.minSrcExtent = capabilities.maxSrcExtent = extent;
    capabilities.minDstExtent = capabilities.maxDstExtent = extent;
    return capabilities;
}

VkSurfaceCapabilitiesKHR virtualSurfaceCapabilities() {
    const VkExtent2D extent{virtualDisplay().width, virtualDisplay().height};
    VkSurfaceCapabilitiesKHR capabilities{};
    capabilities.minImageCount = 2;
    capabilities.maxImageCount = 4;
    capabilities.currentExtent = capabilities.minImageExtent = capabilities.maxImageExtent = extent;
    capabilities.maxImageArrayLayers = 1;
    capabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    capabilities.supportedUsageFlags =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    return capabilities;
}

constexpr VkSurfaceFormatKHR kVirtualFormats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
constexpr VkPresentModeKHR kVirtualPresentModes[] = {
    VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};

// Writes `total` items produced by `item(i)` with the usual two call enumeration contract.
template <typename T, typename F>
VkResult enumerate(uint32_t total, uint32_t* count, T* out, F item) {
    if (out == nullptr) {
        *count = total;
        return VK_SUCCESS;
    }
    const uint32_t written = std::min(*count, total);
    for (uint32_t i = 0; i < written; ++i) out[i] = item(i);
    *count = written;
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

// The virtual display is listed first, so the real displays keep their order after it.
template <typename T, typename Next>
VkResult prependDisplay(uint32_t* count, T* out, const T& virtualEntry, Next next) {
    if (out == nullptr) {
        uint32_t real = 0;
        const VkResult result = next(&real, nullptr);
        *count = (result == VK_SUCCESS ? real : 0) + 1;
        return VK_SUCCESS;
    }
    if (*count == 0) return VK_INCOMPLETE;
    out[0] = virtualEntry;
    uint32_t rest = *count - 1;
    const VkResult result = rest > 0 ? next(&rest, out + 1) : VK_SUCCESS;
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) rest = 0;
    *count = rest + 1;
    if (*count == 1) {
        uint32_t real = 0;
        next(&real, nullptr);
        return real > 0 ? VK_INCOMPLETE : VK_SUCCESS;
    }
    return result == VK_INCOMPLETE ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceDisplayPropertiesKHR(
    VkPhysicalDevice physical, uint32_t* count, VkDisplayPropertiesKHR* properties) {
    auto next = KITVR_NEXT(physical, vkGetPhysicalDeviceDisplayPropertiesKHR);
    return prependDisplay(count, properties, virtualDisplayProperties(),
                          [&](uint32_t* n, VkDisplayPropertiesKHR* p) {
                              return next ? next(physical, n, p) : VK_ERROR_EXTENSION_NOT_PRESENT;
                          });
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceDisplayProperties2KHR(
    VkPhysicalDevice physical, uint32_t* count, VkDisplayProperties2KHR* properties) {
    auto next = KITVR_NEXT(physical, vkGetPhysicalDeviceDisplayProperties2KHR);
    VkDisplayProperties2KHR entry{VK_STRUCTURE_TYPE_DISPLAY_PROPERTIES_2_KHR};
    entry.displayProperties = virtualDisplayProperties();
    if (properties != nullptr && *count > 0) entry.pNext = properties[0].pNext;
    return prependDisplay(count, properties, entry, [&](uint32_t* n, VkDisplayProperties2KHR* p) {
        return next ? next(physical, n, p) : VK_ERROR_EXTENSION_NOT_PRESENT;
    });
}

// The virtual plane is appended after the real planes, since plane indices are positional.
uint32_t realPlaneCount(VkPhysicalDevice physical) {
    auto next = KITVR_NEXT(physical, vkGetPhysicalDeviceDisplayPlanePropertiesKHR);
    uint32_t count = 0;
    if (next == nullptr || next(physical, &count, nullptr) != VK_SUCCESS) return 0;
    return count;
}

template <typename T, typename Next>
VkResult appendPlane(VkPhysicalDevice physical, uint32_t* count, T* out, const T& virtualEntry,
                     Next next) {
    const uint32_t real = realPlaneCount(physical);
    if (out == nullptr) {
        *count = real + 1;
        return VK_SUCCESS;
    }
    uint32_t written = std::min(*count, real);
    if (written > 0 && next(&written, out) < VK_SUCCESS) written = 0;
    if (written == real && *count > real) {
        out[real] = virtualEntry;
        *count = real + 1;
        return VK_SUCCESS;
    }
    *count = written;
    return VK_INCOMPLETE;
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceDisplayPlanePropertiesKHR(
    VkPhysicalDevice physical, uint32_t* count, VkDisplayPlanePropertiesKHR* properties) {
    auto next = KITVR_NEXT(physical, vkGetPhysicalDeviceDisplayPlanePropertiesKHR);
    return appendPlane(physical, count, properties, VkDisplayPlanePropertiesKHR{kVirtualDisplay, 0},
                       [&](uint32_t* n, VkDisplayPlanePropertiesKHR* p) { return next(physical, n, p); });
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceDisplayPlaneProperties2KHR(
    VkPhysicalDevice physical, uint32_t* count, VkDisplayPlaneProperties2KHR* properties) {
    auto next = KITVR_NEXT(physical, vkGetPhysicalDeviceDisplayPlaneProperties2KHR);
    VkDisplayPlaneProperties2KHR entry{VK_STRUCTURE_TYPE_DISPLAY_PLANE_PROPERTIES_2_KHR};
    entry.displayPlaneProperties = {kVirtualDisplay, 0};
    return appendPlane(physical, count, properties, entry,
                       [&](uint32_t* n, VkDisplayPlaneProperties2KHR* p) {
                           return next ? next(physical, n, p) : VK_ERROR_EXTENSION_NOT_PRESENT;
                       });
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetDisplayPlaneSupportedDisplaysKHR(
    VkPhysicalDevice physical, uint32_t plane, uint32_t* count, VkDisplayKHR* displays) {
    if (plane == realPlaneCount(physical)) {
        return enumerate(1, count, displays, [](uint32_t) { return kVirtualDisplay; });
    }
    return KITVR_NEXT(physical, vkGetDisplayPlaneSupportedDisplaysKHR)(physical, plane, count, displays);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetDisplayModePropertiesKHR(
    VkPhysicalDevice physical, VkDisplayKHR display, uint32_t* count,
    VkDisplayModePropertiesKHR* properties) {
    if (isVirtual(display)) {
        return enumerate(1, count, properties, [](uint32_t) { return virtualModeProperties(); });
    }
    return KITVR_NEXT(physical, vkGetDisplayModePropertiesKHR)(physical, display, count, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetDisplayModeProperties2KHR(
    VkPhysicalDevice physical, VkDisplayKHR display, uint32_t* count,
    VkDisplayModeProperties2KHR* properties) {
    if (isVirtual(display)) {
        return enumerate(1, count, properties, [&](uint32_t i) {
            VkDisplayModeProperties2KHR entry{VK_STRUCTURE_TYPE_DISPLAY_MODE_PROPERTIES_2_KHR};
            entry.pNext = properties[i].pNext;
            entry.displayModeProperties = virtualModeProperties();
            return entry;
        });
    }
    return KITVR_NEXT(physical, vkGetDisplayModeProperties2KHR)(physical, display, count, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDisplayModeKHR(VkPhysicalDevice physical, VkDisplayKHR display,
                                                       const VkDisplayModeCreateInfoKHR* info,
                                                       const VkAllocationCallbacks* allocator,
                                                       VkDisplayModeKHR* mode) {
    if (isVirtual(display)) {
        *mode = kVirtualMode;
        return VK_SUCCESS;
    }
    return KITVR_NEXT(physical, vkCreateDisplayModeKHR)(physical, display, info, allocator, mode);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetDisplayPlaneCapabilitiesKHR(
    VkPhysicalDevice physical, VkDisplayModeKHR mode, uint32_t plane,
    VkDisplayPlaneCapabilitiesKHR* capabilities) {
    if (isVirtual(mode)) {
        *capabilities = virtualPlaneCapabilities();
        return VK_SUCCESS;
    }
    return KITVR_NEXT(physical, vkGetDisplayPlaneCapabilitiesKHR)(physical, mode, plane, capabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetDisplayPlaneCapabilities2KHR(
    VkPhysicalDevice physical, const VkDisplayPlaneInfo2KHR* info,
    VkDisplayPlaneCapabilities2KHR* capabilities) {
    if (isVirtual(info->mode)) {
        capabilities->capabilities = virtualPlaneCapabilities();
        return VK_SUCCESS;
    }
    return KITVR_NEXT(physical, vkGetDisplayPlaneCapabilities2KHR)(physical, info, capabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDisplayPlaneSurfaceKHR(
    VkInstance instance, const VkDisplaySurfaceCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    if (isVirtual(info->displayMode)) {
        logLine("virtual display surface created");
        *surface = kVirtualSurface;
        return VK_SUCCESS;
    }
    InstanceData* data = findInstance(instance);
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    return reinterpret_cast<PFN_vkCreateDisplayPlaneSurfaceKHR>(
        data->next(instance, "vkCreateDisplayPlaneSurfaceKHR"))(instance, info, allocator, surface);
}

VKAPI_ATTR void VKAPI_CALL hookDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                 const VkAllocationCallbacks* allocator) {
    if (isVirtual(surface)) return;
    InstanceData* data = findInstance(instance);
    if (data == nullptr) return;
    reinterpret_cast<PFN_vkDestroySurfaceKHR>(data->next(instance, "vkDestroySurfaceKHR"))(
        instance, surface, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physical, uint32_t family, VkSurfaceKHR surface, VkBool32* supported) {
    if (isVirtual(surface)) {
        *supported = VK_TRUE;
        return VK_SUCCESS;
    }
    return KITVR_NEXT(physical, vkGetPhysicalDeviceSurfaceSupportKHR)(physical, family, surface, supported);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* capabilities) {
    if (isVirtual(surface)) {
        *capabilities = virtualSurfaceCapabilities();
        return VK_SUCCESS;
    }
    return KITVR_NEXT(physical, vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(physical, surface, capabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR* info,
    VkSurfaceCapabilities2KHR* capabilities) {
    if (isVirtual(info->surface)) {
        capabilities->surfaceCapabilities = virtualSurfaceCapabilities();
        return VK_SUCCESS;
    }
    return KITVR_NEXT(physical, vkGetPhysicalDeviceSurfaceCapabilities2KHR)(physical, info, capabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceSurfaceCapabilities2EXT(
    VkPhysicalDevice physical, VkSurfaceKHR surface, VkSurfaceCapabilities2EXT* capabilities) {
    if (isVirtual(surface)) {
        const VkSurfaceCapabilitiesKHR base = virtualSurfaceCapabilities();
        capabilities->minImageCount = base.minImageCount;
        capabilities->maxImageCount = base.maxImageCount;
        capabilities->currentExtent = base.currentExtent;
        capabilities->minImageExtent = base.minImageExtent;
        capabilities->maxImageExtent = base.maxImageExtent;
        capabilities->maxImageArrayLayers = base.maxImageArrayLayers;
        capabilities->supportedTransforms = base.supportedTransforms;
        capabilities->currentTransform = base.currentTransform;
        capabilities->supportedCompositeAlpha = base.supportedCompositeAlpha;
        capabilities->supportedUsageFlags = base.supportedUsageFlags;
        capabilities->supportedSurfaceCounters = VK_SURFACE_COUNTER_VBLANK_BIT_EXT;
        return VK_SUCCESS;
    }
    return KITVR_NEXT(physical, vkGetPhysicalDeviceSurfaceCapabilities2EXT)(physical, surface, capabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t* count, VkSurfaceFormatKHR* formats) {
    if (isVirtual(surface)) {
        return enumerate(std::size(kVirtualFormats), count, formats,
                         [](uint32_t i) { return kVirtualFormats[i]; });
    }
    return KITVR_NEXT(physical, vkGetPhysicalDeviceSurfaceFormatsKHR)(physical, surface, count, formats);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR* info, uint32_t* count,
    VkSurfaceFormat2KHR* formats) {
    if (isVirtual(info->surface)) {
        return enumerate(std::size(kVirtualFormats), count, formats, [&](uint32_t i) {
            VkSurfaceFormat2KHR entry{VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR};
            entry.pNext = formats[i].pNext;
            entry.surfaceFormat = kVirtualFormats[i];
            return entry;
        });
    }
    return KITVR_NEXT(physical, vkGetPhysicalDeviceSurfaceFormats2KHR)(physical, info, count, formats);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t* count, VkPresentModeKHR* modes) {
    if (isVirtual(surface)) {
        return enumerate(std::size(kVirtualPresentModes), count, modes,
                         [](uint32_t i) { return kVirtualPresentModes[i]; });
    }
    return KITVR_NEXT(physical, vkGetPhysicalDeviceSurfacePresentModesKHR)(physical, surface, count, modes);
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPhysicalDevicePresentRectanglesKHR(
    VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t* count, VkRect2D* rects) {
    if (isVirtual(surface)) {
        return enumerate(1, count, rects, [](uint32_t) {
            return VkRect2D{{0, 0}, {virtualDisplay().width, virtualDisplay().height}};
        });
    }
    return KITVR_NEXT(physical, vkGetPhysicalDevicePresentRectanglesKHR)(physical, surface, count, rects);
}

// Acquiring the virtual display always succeeds; there is nothing to take from the desktop.
VKAPI_ATTR VkResult VKAPI_CALL hookAcquireDrmDisplayEXT(VkPhysicalDevice physical, int32_t drmFd,
                                                       VkDisplayKHR display) {
    if (isVirtual(display)) return VK_SUCCESS;
    return KITVR_NEXT(physical, vkAcquireDrmDisplayEXT)(physical, drmFd, display);
}

using PFN_AcquireXlibDisplay = VkResult(VKAPI_PTR*)(VkPhysicalDevice, void*, VkDisplayKHR);
VKAPI_ATTR VkResult VKAPI_CALL hookAcquireXlibDisplayEXT(VkPhysicalDevice physical, void* dpy,
                                                        VkDisplayKHR display) {
    if (isVirtual(display)) return VK_SUCCESS;
    return nextPhysical<PFN_AcquireXlibDisplay>(physical, "vkAcquireXlibDisplayEXT")(physical, dpy, display);
}

VKAPI_ATTR VkResult VKAPI_CALL hookReleaseDisplayEXT(VkPhysicalDevice physical, VkDisplayKHR display) {
    if (isVirtual(display)) return VK_SUCCESS;
    return KITVR_NEXT(physical, vkReleaseDisplayEXT)(physical, display);
}

#undef KITVR_NEXT

PFN_vkVoidFunction displayHook(const char* name) {
    if (!virtualDisplay().enabled) return nullptr;
    KITVR_HOOK(vkGetPhysicalDeviceDisplayPropertiesKHR, hookGetPhysicalDeviceDisplayPropertiesKHR);
    KITVR_HOOK(vkGetPhysicalDeviceDisplayProperties2KHR, hookGetPhysicalDeviceDisplayProperties2KHR);
    KITVR_HOOK(vkGetPhysicalDeviceDisplayPlanePropertiesKHR, hookGetPhysicalDeviceDisplayPlanePropertiesKHR);
    KITVR_HOOK(vkGetPhysicalDeviceDisplayPlaneProperties2KHR, hookGetPhysicalDeviceDisplayPlaneProperties2KHR);
    KITVR_HOOK(vkGetDisplayPlaneSupportedDisplaysKHR, hookGetDisplayPlaneSupportedDisplaysKHR);
    KITVR_HOOK(vkGetDisplayModePropertiesKHR, hookGetDisplayModePropertiesKHR);
    KITVR_HOOK(vkGetDisplayModeProperties2KHR, hookGetDisplayModeProperties2KHR);
    KITVR_HOOK(vkCreateDisplayModeKHR, hookCreateDisplayModeKHR);
    KITVR_HOOK(vkGetDisplayPlaneCapabilitiesKHR, hookGetDisplayPlaneCapabilitiesKHR);
    KITVR_HOOK(vkGetDisplayPlaneCapabilities2KHR, hookGetDisplayPlaneCapabilities2KHR);
    KITVR_HOOK(vkCreateDisplayPlaneSurfaceKHR, hookCreateDisplayPlaneSurfaceKHR);
    KITVR_HOOK(vkDestroySurfaceKHR, hookDestroySurfaceKHR);
    KITVR_HOOK(vkGetPhysicalDeviceSurfaceSupportKHR, hookGetPhysicalDeviceSurfaceSupportKHR);
    KITVR_HOOK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, hookGetPhysicalDeviceSurfaceCapabilitiesKHR);
    KITVR_HOOK(vkGetPhysicalDeviceSurfaceCapabilities2KHR, hookGetPhysicalDeviceSurfaceCapabilities2KHR);
    KITVR_HOOK(vkGetPhysicalDeviceSurfaceCapabilities2EXT, hookGetPhysicalDeviceSurfaceCapabilities2EXT);
    KITVR_HOOK(vkGetPhysicalDeviceSurfaceFormatsKHR, hookGetPhysicalDeviceSurfaceFormatsKHR);
    KITVR_HOOK(vkGetPhysicalDeviceSurfaceFormats2KHR, hookGetPhysicalDeviceSurfaceFormats2KHR);
    KITVR_HOOK(vkGetPhysicalDeviceSurfacePresentModesKHR, hookGetPhysicalDeviceSurfacePresentModesKHR);
    KITVR_HOOK(vkGetPhysicalDevicePresentRectanglesKHR, hookGetPhysicalDevicePresentRectanglesKHR);
    KITVR_HOOK(vkAcquireDrmDisplayEXT, hookAcquireDrmDisplayEXT);
    KITVR_HOOK(vkAcquireXlibDisplayEXT, hookAcquireXlibDisplayEXT);
    KITVR_HOOK(vkReleaseDisplayEXT, hookReleaseDisplayEXT);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGetInstanceProcAddr(VkInstance instance,
                                                                const char* name) {
    if (name == nullptr) return nullptr;
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookGetInstanceProcAddr);
    }
    if (std::strcmp(name, "vkCreateInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookCreateInstance);
    }
    if (std::strcmp(name, "vkEnumerateInstanceLayerProperties") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookEnumerateInstanceLayerProperties);
    }
    if (instance == VK_NULL_HANDLE) {
        return g_globalGipa != nullptr ? g_globalGipa(instance, name) : nullptr;
    }
    if (std::strcmp(name, "vkCreateDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookCreateDevice);
    }
    if (std::strcmp(name, "vkDestroyInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookDestroyInstance);
    }
    if (std::strcmp(name, "vkEnumeratePhysicalDevices") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookEnumeratePhysicalDevices);
    }
    if (PFN_vkVoidFunction hook = displayHook(name)) return hook;
    // Device functions requested through the instance must still reach the device hooks.
    if (PFN_vkVoidFunction hook = deviceHook(name)) return hook;
    InstanceData* data = findInstance(instance);
    if (data == nullptr || data->next == nullptr) return nullptr;
    return data->next(instance, name);
}

#undef KITVR_HOOK

}  // namespace

KITVR_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* version) {
    if (version == nullptr || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (version->loaderLayerInterfaceVersion > 2) version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = hookGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = hookGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
