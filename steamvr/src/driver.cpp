#include "openvr_driver.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace {

constexpr uint16_t kTrackingPort = 9945;
constexpr uint8_t kTrackingPacketType = 2;
constexpr size_t kHeadPoseOffset = 12;
constexpr size_t kHeadPoseEnd = 40;
constexpr uint32_t kEyeWidth = 1832;
constexpr uint32_t kEyeHeight = 1920;

uint32_t readU32Le(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

float readF32Le(const uint8_t* bytes) {
    const uint32_t bits = readU32Le(bytes);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

class Hmd final : public vr::ITrackedDeviceServerDriver,
                  public vr::IVRDisplayComponent,
                  public vr::IVRVirtualDisplay {
public:
    vr::EVRInitError Activate(uint32_t objectId) override {
        objectId_ = objectId;
        const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId);
        vr::VRProperties()->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "KitVR");
        vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, "KitVR Streamed HMD");
        vr::VRProperties()->SetStringProperty(container, vr::Prop_SerialNumber_String, "KITVR-HMD-001");
        vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "Crystal Kitsune Studios");
        vr::VRProperties()->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, 90.0f);
        vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, 0.064f);
        vr::VRProperties()->SetBoolProperty(container, vr::Prop_HasDisplayComponent_Bool, true);
        return vr::VRInitError_None;
    }

    void Deactivate() override { objectId_ = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}

    void* GetComponent(const char* name) override {
        if (std::strcmp(name, vr::IVRDisplayComponent_Version) == 0) {
            return static_cast<vr::IVRDisplayComponent*>(this);
        }
        if (std::strcmp(name, vr::IVRVirtualDisplay_Version) == 0) {
            return static_cast<vr::IVRVirtualDisplay*>(this);
        }
        return nullptr;
    }

    void DebugRequest(const char*, char* response, uint32_t responseSize) override {
        if (responseSize > 0) response[0] = '\0';
    }

    vr::DriverPose_t GetPose() override {
        std::lock_guard<std::mutex> lock(poseMutex_);
        return pose_;
    }

    uint32_t objectId() const { return objectId_; }

    void updatePose(const float* values, bool valid) {
        vr::DriverPose_t pose{};
        pose.poseTimeOffset = 0.0;
        pose.qWorldFromDriverRotation = {1.0, 0.0, 0.0, 0.0};
        pose.qDriverFromHeadRotation = {1.0, 0.0, 0.0, 0.0};
        pose.qRotation = {values[6], values[3], values[4], values[5]};
        pose.vecPosition[0] = values[0];
        pose.vecPosition[1] = values[1];
        pose.vecPosition[2] = values[2];
        pose.result = valid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
        pose.poseIsValid = valid;
        pose.deviceIsConnected = true;
        std::lock_guard<std::mutex> lock(poseMutex_);
        pose_ = pose;
    }

    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) override {
        *x = 0;
        *y = 0;
        *width = kEyeWidth * 2;
        *height = kEyeHeight;
    }

    bool IsDisplayOnDesktop() override { return false; }
    bool IsDisplayRealDisplay() override { return false; }

    void GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) override {
        *width = kEyeWidth * 2;
        *height = kEyeHeight;
    }

    void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y,
                              uint32_t* width, uint32_t* height) override {
        *x = eye == vr::Eye_Left ? 0 : kEyeWidth;
        *y = 0;
        *width = kEyeWidth;
        *height = kEyeHeight;
    }

    void GetProjectionRaw(vr::EVREye, float* left, float* right,
                          float* top, float* bottom) override {
        *left = -1.1f;
        *right = 1.1f;
        *top = -1.0f;
        *bottom = 1.0f;
    }

    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float u, float v) override {
        vr::DistortionCoordinates_t result{};
        result.rfRed[0] = result.rfGreen[0] = result.rfBlue[0] = u;
        result.rfRed[1] = result.rfGreen[1] = result.rfBlue[1] = v;
        return result;
    }

    bool ComputeInverseDistortion(vr::HmdVector2_t* result, vr::EVREye,
                                  uint32_t, float u, float v) override {
        result->v[0] = u;
        result->v[1] = v;
        return true;
    }

    void Present(const vr::PresentInfo_t*, uint32_t) override {
        lastPresent_ = std::chrono::steady_clock::now();
    }

    void WaitForPresent() override {}

    bool GetTimeSinceLastVsync(float* secondsSinceVsync, uint64_t* frameCounter) override {
        constexpr double frameInterval = 1.0 / 90.0;
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startTime_).count();
        const auto frame = static_cast<uint64_t>(elapsed / frameInterval);
        *secondsSinceVsync = static_cast<float>(elapsed - frame * frameInterval);
        *frameCounter = frame;
        return true;
    }

private:
    uint32_t objectId_ = vr::k_unTrackedDeviceIndexInvalid;
    std::mutex poseMutex_;
    vr::DriverPose_t pose_{};
    std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastPresent_{};
};

class Provider final : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return vr::VRInitError_Driver_Failed;
#endif
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == kInvalidSocket) return vr::VRInitError_Driver_Failed;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kTrackingPort);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            closeSocket();
            return vr::VRInitError_Driver_Failed;
        }
#ifdef _WIN32
        u_long nonBlocking = 1;
        ioctlsocket(socket_, FIONBIO, &nonBlocking);
#else
        const int flags = fcntl(socket_, F_GETFL, 0);
        fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
#endif

        return vr::VRServerDriverHost()->TrackedDeviceAdded(
                   "KITVR-HMD-001", vr::TrackedDeviceClass_HMD, &hmd_)
                   ? vr::VRInitError_None
                   : vr::VRInitError_Driver_HmdUnknown;
    }

    void Cleanup() override {
        closeSocket();
#ifdef _WIN32
        WSACleanup();
#endif
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }

    void RunFrame() override {
        std::array<uint8_t, 256> packet{};
        bool gotPose = false;
        float head[7]{};
        for (;;) {
#ifdef _WIN32
            const int count = recv(socket_, reinterpret_cast<char*>(packet.data()),
                                   static_cast<int>(packet.size()), 0);
#else
            const int count = static_cast<int>(recv(socket_, packet.data(), packet.size(), 0));
#endif
            if (count < static_cast<int>(kHeadPoseEnd)) break;
            if (packet[0] != kTrackingPacketType) continue;
            bool valid = true;
            float parsed[7]{};
            for (size_t i = 0; i < 7; ++i) {
                parsed[i] = readF32Le(packet.data() + kHeadPoseOffset + i * sizeof(float));
                valid &= std::isfinite(parsed[i]);
            }
            const float norm = std::sqrt(parsed[3] * parsed[3] + parsed[4] * parsed[4] +
                                         parsed[5] * parsed[5] + parsed[6] * parsed[6]);
            valid &= norm > 1e-6f;
            if (valid) {
                for (size_t i = 3; i < 7; ++i) parsed[i] /= norm;
                std::copy(std::begin(parsed), std::end(parsed), std::begin(head));
                gotPose = true;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (gotPose) {
            hmd_.updatePose(head, true);
            lastPose_ = now;
            havePose_ = true;
        } else if (havePose_ && now - lastPose_ > std::chrono::milliseconds(250)) {
            hmd_.updatePose(head, false);
            havePose_ = false;
        }

        if (hmd_.objectId() != vr::k_unTrackedDeviceIndexInvalid) {
            const auto pose = hmd_.GetPose();
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(
                hmd_.objectId(), pose, sizeof(pose));
        }
    }

    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    void closeSocket() {
        if (socket_ == kInvalidSocket) return;
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
        socket_ = kInvalidSocket;
    }

    Hmd hmd_;
    SocketHandle socket_ = kInvalidSocket;
    std::chrono::steady_clock::time_point lastPose_{};
    bool havePose_ = false;
};

Provider provider;

}  // namespace

#ifdef _WIN32
#define KITVR_EXPORT extern "C" __declspec(dllexport)
#else
#define KITVR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

KITVR_EXPORT void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
    if (std::strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version) == 0) {
        return &provider;
    }
    if (returnCode) *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
