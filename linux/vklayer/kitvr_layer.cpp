// KitVR Vulkan WSI layer.
//
// SteamVR on Linux cannot hand frames to an OpenVR driver: IVRVirtualDisplay is broken there and
// IVRDriverDirectModeComponent is not implemented at all. The supported way to get the composited
// image out is to intercept the swapchain that vrcompositor presents its mirror into, which is what
// this layer does.
//
// For every presented image the layer copies it to a host visible buffer and publishes the pixels
// into a POSIX shared memory ring. The KitVR server reads that ring with --source steamvr and feeds
// it to ffmpeg as rawvideo.
//
// The ring layout is documented in linux/README.md and mirrored by crates/server/src/steamvr.rs.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define KITVR_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr uint32_t kMagic = 0x4B565246u;  // "KVRF"
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 48;
constexpr uint32_t kFormatBgra8 = 0;
constexpr uint32_t kFormatRgba8 = 1;
constexpr uint32_t kMaxSwapchainImages = 8;

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
        const std::string path = std::string("/dev/shm/") + name;

        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd < 0) {
            logLine("cannot open %s", path.c_str());
            return false;
        }
        if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
            logLine("cannot size %s", path.c_str());
            ::close(fd);
            return false;
        }
        void* mapping = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (mapping == MAP_FAILED) {
            logLine("cannot map %s", path.c_str());
            return false;
        }

        mapping_ = static_cast<uint8_t*>(mapping);
        size_ = total;
        width_ = width;
        height_ = height;
        format_ = format;
        stride_ = stride;
        std::memset(mapping_, 0, kHeaderBytes);
        std::memcpy(mapping_ + 0, &kMagic, 4);
        std::memcpy(mapping_ + 4, &kVersion, 4);
        std::memcpy(mapping_ + 8, &width_, 4);
        std::memcpy(mapping_ + 12, &height_, 4);
        std::memcpy(mapping_ + 16, &format_, 4);
        std::memcpy(mapping_ + 20, &stride_, 4);
        const uint32_t payload32 = static_cast<uint32_t>(payload);
        std::memcpy(mapping_ + 24, &payload32, 4);
        logLine("publishing %ux%u stride %u to %s", width_, height_, stride_, path.c_str());
        return true;
    }

    // Copies a frame and publishes it. frameId is bumped last with release semantics so a reader
    // never sees a partially written frame with a fresh id.
    void publish(const void* pixels, size_t bytes) {
        if (mapping_ == nullptr) return;
        const uint32_t payload = *reinterpret_cast<uint32_t*>(mapping_ + 24);
        const size_t count = bytes < payload ? bytes : payload;
        std::memcpy(mapping_ + kHeaderBytes, pixels, count);
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const uint64_t stamp = static_cast<uint64_t>(now.tv_sec) * 1000000000ull +
                               static_cast<uint64_t>(now.tv_nsec);
        std::memcpy(mapping_ + 40, &stamp, 8);
        auto* id = reinterpret_cast<uint64_t*>(mapping_ + 32);
        __atomic_add_fetch(id, 1ull, __ATOMIC_RELEASE);
    }

    bool ready() const { return mapping_ != nullptr; }

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

Publisher g_publisher;

// ---------------------------------------------------------------------------------------------
// Per object state
// ---------------------------------------------------------------------------------------------

struct QueueRecord {
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
};

struct SwapchainState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    bool initialized = false;
    bool failed = false;
    uint32_t currentImage = 0;
    uint32_t imageCount = 0;
    std::vector<bool> captured;
};

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr next = nullptr;
    PFN_vkDestroyInstance destroy = nullptr;
    PFN_vkCreateDevice createDevice = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queueFamilies = nullptr;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    InstanceData* instance = nullptr;
    PFN_vkGetDeviceProcAddr next = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR swapchainImages = nullptr;
    PFN_vkAcquireNextImageKHR acquire = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkGetDeviceQueue getQueue = nullptr;
    PFN_vkCreateCommandPool createPool = nullptr;
    PFN_vkDestroyCommandPool destroyPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommands = nullptr;
    PFN_vkFreeCommandBuffers freeCommands = nullptr;
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
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkQueueWaitIdle queueWaitIdle = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    std::mutex mutex;
    std::vector<QueueRecord> queues;
    std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;
};

std::mutex g_instancesMutex;
std::unordered_map<VkInstance, InstanceData*> g_instances;
std::mutex g_devicesMutex;
std::unordered_map<VkDevice, DeviceData*> g_devices;

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

VkLayerInstanceCreateInfo* findInstanceLinkInfo(const VkInstanceCreateInfo* info) {
    auto* chain = reinterpret_cast<const VkBaseOutStructure*>(info->pNext);
    while (chain != nullptr) {
        if (chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            auto* loader = const_cast<VkLayerInstanceCreateInfo*>(
                reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain));
            if (loader->function == VK_LAYER_LINK_INFO) return loader;
        }
        chain = chain->pNext;
    }
    return nullptr;
}

VkLayerDeviceCreateInfo* findDeviceLinkInfo(const VkDeviceCreateInfo* info) {
    auto* chain = reinterpret_cast<const VkBaseOutStructure*>(info->pNext);
    while (chain != nullptr) {
        if (chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* loader = const_cast<VkLayerDeviceCreateInfo*>(
                reinterpret_cast<const VkLayerDeviceCreateInfo*>(chain));
            if (loader->function == VK_LAYER_LINK_INFO) return loader;
        }
        chain = chain->pNext;
    }
    return nullptr;
}

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
        default:
            return UINT32_MAX;
    }
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL hookCreateSwapchainKHR(VkDevice device,
                                                     const VkSwapchainCreateInfoKHR* createInfo,
                                                     const VkAllocationCallbacks* allocator,
                                                     VkSwapchainKHR* swapchain) {
    DeviceData* data = findDevice(device);
    if (data == nullptr || data->createSwapchain == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    // vkCmdCopyImageToBuffer needs TRANSFER_SRC on the image, which vrcompositor does not request.
    VkSwapchainCreateInfoKHR info = *createInfo;
    info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    const VkResult result = data->createSwapchain(device, &info, allocator, swapchain);
    if (result != VK_SUCCESS) return result;

    SwapchainState state;
    state.swapchain = *swapchain;
    state.device = device;
    state.extent = info.imageExtent;
    state.format = info.imageFormat;
    if (data->swapchainImages != nullptr) {
        uint32_t count = 0;
        if (data->swapchainImages(device, *swapchain, &count, nullptr) == VK_SUCCESS) {
            state.imageCount = count;
            state.captured.assign(count, false);
        }
    }
    std::lock_guard<std::mutex> lock(data->mutex);
    data->swapchains[*swapchain] = state;
    KITVR_LOG("swapchain %p %ux%u format %d", reinterpret_cast<void*>(*swapchain),
              state.extent.width, state.extent.height, static_cast<int>(state.format));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hookDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                   const VkAllocationCallbacks* allocator) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return;
    SwapchainState state;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(data->mutex);
        auto it = data->swapchains.find(swapchain);
        if (it != data->swapchains.end()) {
            state = it->second;
            data->swapchains.erase(it);
            found = true;
        }
    }
    if (found && state.initialized) {
        if (data->queueWaitIdle != nullptr) data->queueWaitIdle(VK_NULL_HANDLE);
        if (state.fence != VK_NULL_HANDLE) data->destroyFence(device, state.fence, allocator);
        if (state.buffer != VK_NULL_HANDLE) data->destroyBuffer(device, state.buffer, allocator);
        if (state.memory != VK_NULL_HANDLE) data->freeMemory(device, state.memory, allocator);
        if (state.pool != VK_NULL_HANDLE) data->destroyPool(device, state.pool, allocator);
    }
    if (data->destroySwapchain != nullptr) data->destroySwapchain(device, swapchain, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL hookAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                       uint64_t timeout, VkSemaphore semaphore,
                                                       VkFence fence, uint32_t* index) {
    DeviceData* data = findDevice(device);
    if (data == nullptr || data->acquire == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = data->acquire(device, swapchain, timeout, semaphore, fence, index);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        std::lock_guard<std::mutex> lock(data->mutex);
        auto it = data->swapchains.find(swapchain);
        if (it != data->swapchains.end()) it->second.currentImage = *index;
    }
    return result;
}

void hookGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    DeviceData* data = findDevice(device);
    if (data == nullptr || data->getQueue == nullptr) return;
    data->getQueue(device, family, index, queue);
    if (*queue == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(data->mutex);
    for (auto& record : data->queues) {
        if (record.queue == *queue) return;
    }
    if (data->queues.size() < 16) data->queues.push_back({*queue, family});
}

uint32_t familyForQueue(DeviceData* data, VkQueue queue) {
    std::lock_guard<std::mutex> lock(data->mutex);
    for (const auto& record : data->queues) {
        if (record.queue == queue) return record.family;
    }
    return UINT32_MAX;
}

bool initSwapchain(DeviceData* data, SwapchainState& state, uint32_t family) {
    const uint32_t format = shmFormat(state.format);
    if (format == UINT32_MAX) {
        logLine("unsupported swapchain format %d, capture disabled", static_cast<int>(state.format));
        state.failed = true;
        return false;
    }
    if (state.extent.width == 0 || state.extent.height == 0) {
        state.failed = true;
        return false;
    }

    const VkDevice device = state.device;
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family;
    if (data->createPool(device, &poolInfo, nullptr, &state.pool) != VK_SUCCESS) {
        state.failed = true;
        return false;
    }

    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = state.pool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (data->allocateCommands(device, &commandInfo, &state.commands) != VK_SUCCESS) {
        state.failed = true;
        return false;
    }

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = static_cast<VkDeviceSize>(state.extent.width) * state.extent.height * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (data->createBuffer(device, &bufferInfo, nullptr, &state.buffer) != VK_SUCCESS) {
        state.failed = true;
        return false;
    }

    VkMemoryRequirements requirements{};
    data->bufferRequirements(device, state.buffer, &requirements);

    VkPhysicalDeviceMemoryProperties properties{};
    auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        data->instance->next(data->instance->instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (getProperties == nullptr) {
        state.failed = true;
        return false;
    }
    getProperties(data->physical, &properties);

    uint32_t typeIndex = UINT32_MAX;
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const bool supported = (requirements.memoryTypeBits & (1u << i)) != 0;
        const bool usable = (properties.memoryTypes[i].propertyFlags & wanted) == wanted;
        if (supported && usable) {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == UINT32_MAX) {
        state.failed = true;
        return false;
    }

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = typeIndex;
    if (data->allocateMemory(device, &allocInfo, nullptr, &state.memory) != VK_SUCCESS) {
        state.failed = true;
        return false;
    }
    if (data->bindBuffer(device, state.buffer, state.memory, 0) != VK_SUCCESS) {
        state.failed = true;
        return false;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (data->createFence(device, &fenceInfo, nullptr, &state.fence) != VK_SUCCESS) {
        state.failed = true;
        return false;
    }

    state.queueFamily = family;
    state.initialized = true;
    if (!g_publisher.open(state.extent.width, state.extent.height, format,
                          state.extent.width * 4)) {
        state.failed = true;
        return false;
    }
    KITVR_LOG("capture ready on queue family %u", family);
    return true;
}

void captureFrame(DeviceData* data, SwapchainState& state, VkQueue queue, VkImage image) {
    if (data->beginCommands == nullptr || data->copyImage == nullptr) return;

    data->resetFences(state.device, 1, &state.fence);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (data->beginCommands(state.commands, &beginInfo) != VK_SUCCESS) return;

    const bool firstCapture = state.currentImage < state.captured.size() &&
                              !state.captured[state.currentImage];

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.oldLayout = firstCapture ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransfer.srcAccessMask = firstCapture ? 0 : VK_ACCESS_MEMORY_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    data->barrier(state.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {state.extent.width, state.extent.height, 1};
    data->copyImage(state.commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.buffer, 1,
                    &region);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    data->barrier(state.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);

    if (data->endCommands(state.commands) != VK_SUCCESS) return;

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &state.commands;
    if (data->submit(queue, 1, &submitInfo, state.fence) != VK_SUCCESS) return;

    if (state.currentImage < state.captured.size()) state.captured[state.currentImage] = true;

    // Blocking here costs a few milliseconds per frame but keeps the readback correct. Moving this
    // to the next present trades latency for the same result.
    if (data->waitForFences(state.device, 1, &state.fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) {
        return;
    }
    void* pixels = nullptr;
    const VkDeviceSize size = static_cast<VkDeviceSize>(state.extent.width) *
                              state.extent.height * 4;
    if (data->mapMemory(state.device, state.memory, 0, size, 0, &pixels) != VK_SUCCESS) return;
    g_publisher.publish(pixels, static_cast<size_t>(size));
    data->unmapMemory(state.device, state.memory);
}

VKAPI_ATTR VkResult VKAPI_CALL hookQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    // The device is recovered from the swapchain that is being presented.
    if (info->swapchainCount > 0) {
        VkDevice device = VK_NULL_HANDLE;
        SwapchainState* state = nullptr;
        DeviceData* data = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_devicesMutex);
            for (auto& entry : g_devices) {
                auto it = entry.second->swapchains.find(info->pSwapchains[0]);
                if (it != entry.second->swapchains.end()) {
                    data = entry.second;
                    state = &it->second;
                    device = entry.first;
                    break;
                }
            }
        }
        if (data != nullptr && state != nullptr && !state->failed) {
            uint32_t family = familyForQueue(data, queue);
            if (family == UINT32_MAX) family = state->queueFamily;
            if (!state->initialized) {
                initSwapchain(data, *state, family);
            }
            if (state->initialized && !state->failed) {
                auto getImages = data->swapchainImages;
                if (getImages != nullptr) {
                    std::vector<VkImage> images(state->imageCount);
                    uint32_t count = state->imageCount;
                    if (count > 0 && getImages(device, state->swapchain, &count, images.data()) ==
                                         VK_SUCCESS &&
                        state->currentImage < count) {
                        captureFrame(data, *state, queue, images[state->currentImage]);
                    }
                }
            }
        }
        if (data != nullptr && data->present != nullptr) return data->present(queue, info);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(VkPhysicalDevice physical,
                                                const VkDeviceCreateInfo* createInfo,
                                                const VkAllocationCallbacks* allocator,
                                                VkDevice* device);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGetDeviceProcAddr(VkDevice device, const char* name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGetInstanceProcAddr(VkInstance instance,
                                                                const char* name);

}  // namespace

namespace {

PFN_vkGetInstanceProcAddr g_globalGipa = nullptr;
std::mutex g_physicalMutex;
std::unordered_map<VkPhysicalDevice, InstanceData*> g_physicalInstances;

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
    VkLayerInstanceCreateInfo* loader = findInstanceLinkInfo(createInfo);
    if (loader == nullptr || loader->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerInstanceLink* link = loader->u.pLayerInfo;
    const PFN_vkGetInstanceProcAddr next = link->pfnNextGetInstanceProcAddr;
    loader->u.pLayerInfo = link->pNext;
    g_globalGipa = next;

    auto nextCreate = reinterpret_cast<PFN_vkCreateInstance>(next(nullptr, "vkCreateInstance"));
    if (nextCreate == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = nextCreate(createInfo, allocator, instance);
    if (result != VK_SUCCESS) return result;

    auto* data = new InstanceData();
    data->instance = *instance;
    data->next = next;
    data->destroy = reinterpret_cast<PFN_vkDestroyInstance>(next(*instance, "vkDestroyInstance"));
    data->createDevice = reinterpret_cast<PFN_vkCreateDevice>(next(*instance, "vkCreateDevice"));
    data->queueFamilies = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        next(*instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    {
        std::lock_guard<std::mutex> lock(g_instancesMutex);
        g_instances[*instance] = data;
    }
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

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(VkPhysicalDevice physical,
                                                const VkDeviceCreateInfo* createInfo,
                                                const VkAllocationCallbacks* allocator,
                                                VkDevice* device) {
    VkLayerDeviceCreateInfo* loader = findDeviceLinkInfo(createInfo);
    if (loader == nullptr || loader->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerDeviceLink* link = loader->u.pLayerInfo;
    const PFN_vkGetInstanceProcAddr nextGipa = link->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr nextGdpa = link->pfnNextGetDeviceProcAddr;
    loader->u.pLayerInfo = link->pNext;

    InstanceData* instance = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_physicalMutex);
        auto it = g_physicalInstances.find(physical);
        if (it != g_physicalInstances.end()) instance = it->second;
    }
    if (instance == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    auto nextCreate = reinterpret_cast<PFN_vkCreateDevice>(
        nextGipa(instance->instance, "vkCreateDevice"));
    if (nextCreate == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = nextCreate(physical, createInfo, allocator, device);
    if (result != VK_SUCCESS) return result;

    auto* data = new DeviceData();
    data->device = *device;
    data->physical = physical;
    data->instance = instance;
    data->next = nextGdpa;
    const auto load = [&](const char* name) {
        return nextGdpa(*device, name);
    };
    data->destroy = reinterpret_cast<PFN_vkDestroyDevice>(load("vkDestroyDevice"));
    data->createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(load("vkCreateSwapchainKHR"));
    data->destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(load("vkDestroySwapchainKHR"));
    data->swapchainImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(load("vkGetSwapchainImagesKHR"));
    data->acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(load("vkAcquireNextImageKHR"));
    data->present = reinterpret_cast<PFN_vkQueuePresentKHR>(load("vkQueuePresentKHR"));
    data->getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(load("vkGetDeviceQueue"));
    data->createPool = reinterpret_cast<PFN_vkCreateCommandPool>(load("vkCreateCommandPool"));
    data->destroyPool = reinterpret_cast<PFN_vkDestroyCommandPool>(load("vkDestroyCommandPool"));
    data->allocateCommands = reinterpret_cast<PFN_vkAllocateCommandBuffers>(load("vkAllocateCommandBuffers"));
    data->freeCommands = reinterpret_cast<PFN_vkFreeCommandBuffers>(load("vkFreeCommandBuffers"));
    data->beginCommands = reinterpret_cast<PFN_vkBeginCommandBuffer>(load("vkBeginCommandBuffer"));
    data->endCommands = reinterpret_cast<PFN_vkEndCommandBuffer>(load("vkEndCommandBuffer"));
    data->barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(load("vkCmdPipelineBarrier"));
    data->copyImage = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(load("vkCmdCopyImageToBuffer"));
    data->createBuffer = reinterpret_cast<PFN_vkCreateBuffer>(load("vkCreateBuffer"));
    data->destroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(load("vkDestroyBuffer"));
    data->bufferRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(load("vkGetBufferMemoryRequirements"));
    data->allocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(load("vkAllocateMemory"));
    data->freeMemory = reinterpret_cast<PFN_vkFreeMemory>(load("vkFreeMemory"));
    data->bindBuffer = reinterpret_cast<PFN_vkBindBufferMemory>(load("vkBindBufferMemory"));
    data->mapMemory = reinterpret_cast<PFN_vkMapMemory>(load("vkMapMemory"));
    data->unmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(load("vkUnmapMemory"));
    data->submit = reinterpret_cast<PFN_vkQueueSubmit>(load("vkQueueSubmit"));
    data->queueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(load("vkQueueWaitIdle"));
    data->createFence = reinterpret_cast<PFN_vkCreateFence>(load("vkCreateFence"));
    data->destroyFence = reinterpret_cast<PFN_vkDestroyFence>(load("vkDestroyFence"));
    data->waitForFences = reinterpret_cast<PFN_vkWaitForFences>(load("vkWaitForFences"));
    data->resetFences = reinterpret_cast<PFN_vkResetFences>(load("vkResetFences"));
    if (data->createSwapchain == nullptr || data->present == nullptr ||
        data->createPool == nullptr || data->submit == nullptr) {
        logLine("device is missing required entry points, capture disabled");
    }
    {
        std::lock_guard<std::mutex> lock(g_devicesMutex);
        g_devices[*device] = data;
    }
    KITVR_LOG("device %p created", reinterpret_cast<void*>(*device));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hookDestroyDevice(VkDevice device,
                                             const VkAllocationCallbacks* allocator) {
    DeviceData* data = findDevice(device);
    if (data == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(g_devicesMutex);
        g_devices.erase(device);
    }
    if (data->destroy != nullptr) data->destroy(device, allocator);
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
    entry.implementationVersion = 1;
    std::snprintf(entry.description, sizeof(entry.description),
                  "KitVR SteamVR frame capture (publishes to shared memory)");
    properties[0] = entry;
    *count = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGetDeviceProcAddr(VkDevice device, const char* name) {
    if (name == nullptr) return nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookGetDeviceProcAddr);
    }
    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookCreateSwapchainKHR);
    }
    if (std::strcmp(name, "vkDestroySwapchainKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookDestroySwapchainKHR);
    }
    if (std::strcmp(name, "vkAcquireNextImageKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookAcquireNextImageKHR);
    }
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookQueuePresentKHR);
    }
    if (std::strcmp(name, "vkGetDeviceQueue") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookGetDeviceQueue);
    }
    if (std::strcmp(name, "vkDestroyDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hookDestroyDevice);
    }
    DeviceData* data = findDevice(device);
    return data != nullptr && data->next != nullptr ? data->next(device, name) : nullptr;
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
    InstanceData* data = findInstance(instance);
    if (data == nullptr || data->next == nullptr) return nullptr;
    return data->next(instance, name);
}

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
