// Drives the layer's virtual display the way vrcompositor does: find the display through
// VK_KHR_display, create a display plane surface and a swapchain, present cleared frames, and check
// the pacing, the vblank counter and what lands in the frame ring.
//
//   KITVR_VIRTUAL_DISPLAY=1 KITVR_DISPLAY_WIDTH=640 KITVR_DISPLAY_HEIGHT=360
//   KITVR_STEAMVR_SHM=kitvr-display-test VK_LAYER_PATH=<build dir>
//   VK_INSTANCE_LAYERS=VK_LAYER_CKS_kitvr ./virtual_display_test
//
// Only needs a Vulkan driver; the loader is opened at runtime.

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

PFN_vkGetInstanceProcAddr gipa = nullptr;
VkInstance instance = VK_NULL_HANDLE;
VkDevice device = VK_NULL_HANDLE;

#define LOAD_I(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
#define LOAD_D(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

bool require(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    std::printf("FAIL %s (%d)\n", what, static_cast<int>(result));
    ++failures;
    return false;
}

}  // namespace

int main() {
    const uint32_t width = 640;
    const uint32_t height = 360;
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW);
    if (loader == nullptr) {
        std::printf("FAIL no Vulkan loader\n");
        return 1;
    }
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));

    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME,
                                        VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = 3;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    if (!require(vkCreateInstance(&instanceInfo, nullptr, &instance), "create instance")) return 1;

    LOAD_I(vkEnumeratePhysicalDevices);
    LOAD_I(vkGetPhysicalDeviceDisplayPropertiesKHR);
    LOAD_I(vkGetPhysicalDeviceDisplayPlanePropertiesKHR);
    LOAD_I(vkGetDisplayPlaneSupportedDisplaysKHR);
    LOAD_I(vkGetDisplayModePropertiesKHR);
    LOAD_I(vkCreateDisplayPlaneSurfaceKHR);
    LOAD_I(vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_I(vkGetPhysicalDeviceSurfaceCapabilities2EXT);
    LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(vkCreateDevice);
    LOAD_I(vkGetDeviceProcAddr);
    LOAD_I(vkDestroySurfaceKHR);
    LOAD_I(vkDestroyInstance);

    uint32_t gpuCount = 1;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu);
    if (gpu == VK_NULL_HANDLE) {
        std::printf("FAIL no GPU\n");
        return 1;
    }

    uint32_t displayCount = 0;
    vkGetPhysicalDeviceDisplayPropertiesKHR(gpu, &displayCount, nullptr);
    std::vector<VkDisplayPropertiesKHR> displays(displayCount);
    vkGetPhysicalDeviceDisplayPropertiesKHR(gpu, &displayCount, displays.data());
    VkDisplayKHR display = VK_NULL_HANDLE;
    for (const auto& entry : displays) {
        if (entry.displayName != nullptr && std::strstr(entry.displayName, "KitVR") != nullptr) {
            display = entry.display;
            check(entry.physicalResolution.width == width && entry.physicalResolution.height == height,
                  "display resolution follows KITVR_DISPLAY_WIDTH and KITVR_DISPLAY_HEIGHT");
        }
    }
    check(display != VK_NULL_HANDLE, "virtual display is enumerated");
    if (display == VK_NULL_HANDLE) return 1;

    uint32_t planeCount = 0;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(gpu, &planeCount, nullptr);
    uint32_t plane = UINT32_MAX;
    for (uint32_t i = 0; i < planeCount && plane == UINT32_MAX; ++i) {
        uint32_t count = 0;
        vkGetDisplayPlaneSupportedDisplaysKHR(gpu, i, &count, nullptr);
        std::vector<VkDisplayKHR> supported(count);
        vkGetDisplayPlaneSupportedDisplaysKHR(gpu, i, &count, supported.data());
        for (VkDisplayKHR candidate : supported) {
            if (candidate == display) plane = i;
        }
    }
    check(plane != UINT32_MAX, "a plane supports the virtual display");

    uint32_t modeCount = 1;
    VkDisplayModePropertiesKHR mode{};
    vkGetDisplayModePropertiesKHR(gpu, display, &modeCount, &mode);
    check(modeCount == 1 && mode.parameters.refreshRate == 90000, "mode refreshes at 90 Hz");

    VkDisplaySurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.displayMode = mode.displayMode;
    surfaceInfo.planeIndex = plane;
    surfaceInfo.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    surfaceInfo.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    surfaceInfo.imageExtent = mode.parameters.visibleRegion;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!require(vkCreateDisplayPlaneSurfaceKHR(instance, &surfaceInfo, nullptr, &surface),
                 "create display plane surface")) {
        return 1;
    }
    VkSurfaceCapabilities2EXT capabilities{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_EXT};
    vkGetPhysicalDeviceSurfaceCapabilities2EXT(gpu, surface, &capabilities);
    check(capabilities.supportedSurfaceCounters & VK_SURFACE_COUNTER_VBLANK_BIT_EXT,
          "surface exposes a vblank counter");

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());
    uint32_t family = 0;
    while (family < familyCount && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    VkBool32 presentable = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(gpu, family, surface, &presentable);
    check(presentable == VK_TRUE, "graphics queue can present to the surface");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                      VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 2;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    if (!require(vkCreateDevice(gpu, &deviceInfo, nullptr, &device), "create device")) return 1;

    LOAD_D(vkGetDeviceQueue);
    LOAD_D(vkCreateSwapchainKHR);
    LOAD_D(vkDestroySwapchainKHR);
    LOAD_D(vkGetSwapchainImagesKHR);
    LOAD_D(vkAcquireNextImageKHR);
    LOAD_D(vkQueuePresentKHR);
    LOAD_D(vkQueueSubmit);
    LOAD_D(vkCreateCommandPool);
    LOAD_D(vkDestroyCommandPool);
    LOAD_D(vkAllocateCommandBuffers);
    LOAD_D(vkBeginCommandBuffer);
    LOAD_D(vkEndCommandBuffer);
    LOAD_D(vkCmdPipelineBarrier);
    LOAD_D(vkCmdClearColorImage);
    LOAD_D(vkCreateSemaphore);
    LOAD_D(vkDestroySemaphore);
    LOAD_D(vkCreateFence);
    LOAD_D(vkDestroyFence);
    LOAD_D(vkWaitForFences);
    LOAD_D(vkResetFences);
    LOAD_D(vkDeviceWaitIdle);
    LOAD_D(vkGetSwapchainCounterEXT);
    LOAD_D(vkRegisterDisplayEventEXT);
    LOAD_D(vkDestroyDevice);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    VkSwapchainCreateInfoKHR swapchainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchainInfo.surface = surface;
    swapchainInfo.minImageCount = 3;
    swapchainInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchainInfo.imageExtent = {width, height};
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (!require(vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &swapchain), "create swapchain")) {
        return 1;
    }
    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    std::vector<VkImage> images(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
    check(imageCount >= 3, "swapchain has at least three images");

    VkFence vblank = VK_NULL_HANDLE;
    VkDisplayEventInfoEXT eventInfo{VK_STRUCTURE_TYPE_DISPLAY_EVENT_INFO_EXT};
    eventInfo.displayEvent = VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT;
    check(vkRegisterDisplayEventEXT(device, display, &eventInfo, nullptr, &vblank) == VK_SUCCESS &&
              vkWaitForFences(device, 1, &vblank, VK_TRUE, 100000000ull) == VK_SUCCESS,
          "vblank event fence signals");
    vkDestroyFence(device, vblank, nullptr);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &poolInfo, nullptr, &pool);
    constexpr uint32_t kInFlight = 2;
    VkCommandBuffer commands[kInFlight]{};
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = pool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = kInFlight;
    vkAllocateCommandBuffers(device, &commandInfo, commands);
    VkSemaphore acquired[kInFlight]{};
    VkSemaphore rendered[kInFlight]{};
    VkFence done[kInFlight]{};
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kInFlight; ++i) {
        vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired[i]);
        vkCreateSemaphore(device, &semaphoreInfo, nullptr, &rendered[i]);
        vkCreateFence(device, &fenceInfo, nullptr, &done[i]);
    }

    uint64_t counterBefore = 0;
    vkGetSwapchainCounterEXT(device, swapchain, VK_SURFACE_COUNTER_VBLANK_BIT_EXT, &counterBefore);
    constexpr uint32_t kFrames = 90;
    const auto start = std::chrono::steady_clock::now();
    float lastColor[4]{};
    const char* name = std::getenv("KITVR_STEAMVR_SHM");
    const std::string path = std::string("/dev/shm/") + (name ? name : "kitvr-steamvr");
    int heldFd = -1;
    std::vector<uint8_t> lockedFrame(48 + static_cast<size_t>(width) * height * 4);
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        if (frame == 20) {
            heldFd = open(path.c_str(), O_RDONLY);
            check(heldFd >= 0, "frame ring available during presents");
            if (heldFd >= 0) {
                check(flock(heldFd, LOCK_SH) == 0, "acquire reader lock");
                check(pread(heldFd, lockedFrame.data(), lockedFrame.size(), 0) ==
                          static_cast<ssize_t>(lockedFrame.size()), "read locked frame");
            }
        }
        if (frame == 30 && heldFd >= 0) {
            std::vector<uint8_t> after(lockedFrame.size());
            check(pread(heldFd, after.data(), after.size(), 0) ==
                      static_cast<ssize_t>(after.size()) && after == lockedFrame,
                  "publisher does not overwrite a locked frame");
            check(flock(heldFd, LOCK_UN) == 0, "release reader lock");
            close(heldFd);
            heldFd = -1;
        }
        const uint32_t slot = frame % kInFlight;
        vkWaitForFences(device, 1, &done[slot], VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &done[slot]);
        uint32_t index = 0;
        if (!require(vkAcquireNextImageKHR(device, swapchain, 1000000000ull, acquired[slot],
                                           VK_NULL_HANDLE, &index),
                     "acquire")) {
            break;
        }
        VkCommandBuffer cmd = commands[slot];
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &begin);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        const float shade = static_cast<float>(frame + 1) / kFrames;
        VkClearColorValue color{{shade, 0.25f, 1.0f - shade, 1.0f}};
        std::memcpy(lastColor, color.float32, sizeof(lastColor));
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                             &barrier.subresourceRange);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cmd);
        const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired[slot];
        submit.pWaitDstStageMask = &wait;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered[slot];
        vkQueueSubmit(queue, 1, &submit, done[slot]);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered[slot];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        if (!require(vkQueuePresentKHR(queue, &present), "present")) break;
    }
    if (heldFd >= 0) {
        flock(heldFd, LOCK_UN);
        close(heldFd);
    }
    vkDeviceWaitIdle(device);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    uint64_t counterAfter = 0;
    vkGetSwapchainCounterEXT(device, swapchain, VK_SURFACE_COUNTER_VBLANK_BIT_EXT, &counterAfter);
    std::printf("     %u frames in %.3f s, vblank counter advanced %llu\n", kFrames, seconds,
                static_cast<unsigned long long>(counterAfter - counterBefore));
    // 90 frames at 90 Hz take about a second; the first images come back without waiting.
    check(seconds > 0.85 && seconds < 1.3, "presents are paced to the 90 Hz refresh");
    check(counterAfter - counterBefore >= 80 && counterAfter - counterBefore <= 120,
          "vblank counter follows the refresh");

    // Give the ring worker a moment to publish the last copy.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int fd = open(path.c_str(), O_RDONLY);
    check(fd >= 0, "frame ring exists");
    if (fd >= 0) {
        struct stat info{};
        fstat(fd, &info);
        auto* ring = static_cast<const uint8_t*>(
            mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0));
        uint32_t header[8]{};
        uint64_t frameId = 0;
        std::memcpy(header, ring, sizeof(header));
        std::memcpy(&frameId, ring + 32, sizeof(frameId));
        check(header[1] == 2 && header[2] == width && header[3] == height && header[4] == 0,
              "ring holds version 2 640x360 BGRA8");
        std::printf("     %llu frames published\n", static_cast<unsigned long long>(frameId));
        check(frameId >= kFrames - 16, "unlocked frames reached the ring");
        // Centre pixel of the last frame, BGRA.
        const uint8_t* pixel = ring + 48 + (static_cast<size_t>(height / 2) * width + width / 2) * 4;
        const auto near = [](uint8_t value, float expected) {
            return std::abs(static_cast<int>(value) - static_cast<int>(expected * 255.0f + 0.5f)) <= 2;
        };
        std::printf("     last pixel B=%u G=%u R=%u\n", pixel[0], pixel[1], pixel[2]);
        check(near(pixel[2], lastColor[0]) && near(pixel[1], lastColor[1]) &&
                  near(pixel[0], lastColor[2]),
              "ring holds the last presented frame");
        munmap(const_cast<uint8_t*>(ring), static_cast<size_t>(info.st_size));
        close(fd);
    }

    for (uint32_t i = 0; i < kInFlight; ++i) {
        vkDestroySemaphore(device, acquired[i], nullptr);
        vkDestroySemaphore(device, rendered[i], nullptr);
        vkDestroyFence(device, done[i], nullptr);
    }
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::printf(failures == 0 ? "PASS\n" : "FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
