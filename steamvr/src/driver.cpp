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
#define NOMINMAX
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
constexpr size_t kTrackingPacketLength = 136;
constexpr uint32_t kEyeWidth = 1832;
constexpr uint32_t kEyeHeight = 1920;
constexpr uint16_t kTriggerClick = 1u << 0;
constexpr uint16_t kGripClick = 1u << 1;
constexpr uint16_t kPrimaryClick = 1u << 2;
constexpr uint16_t kSecondaryClick = 1u << 3;
constexpr uint16_t kMenuClick = 1u << 4;
constexpr uint16_t kThumbstickClick = 1u << 5;

struct HandState {
    uint16_t buttons = 0;
    bool active = false;
    float trigger = 0.0f;
    float grip = 0.0f;
    float stickX = 0.0f;
    float stickY = 0.0f;
    float pose[7] = {0, 0, 0, 0, 0, 0, 1};
};

uint32_t readU32Le(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint16_t readU16Le(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           (static_cast<uint16_t>(bytes[1]) << 8);
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

vr::HmdMatrix34_t poseMatrix(const float* pose) {
    const double x = pose[3];
    const double y = pose[4];
    const double z = pose[5];
    const double w = pose[6];
    vr::HmdMatrix34_t matrix{};
    matrix.m[0][0] = static_cast<float>(1.0 - 2.0 * (y * y + z * z));
    matrix.m[0][1] = static_cast<float>(2.0 * (x * y - z * w));
    matrix.m[0][2] = static_cast<float>(2.0 * (x * z + y * w));
    matrix.m[1][0] = static_cast<float>(2.0 * (x * y + z * w));
    matrix.m[1][1] = static_cast<float>(1.0 - 2.0 * (x * x + z * z));
    matrix.m[1][2] = static_cast<float>(2.0 * (y * z - x * w));
    matrix.m[2][0] = static_cast<float>(2.0 * (x * z - y * w));
    matrix.m[2][1] = static_cast<float>(2.0 * (y * z + x * w));
    matrix.m[2][2] = static_cast<float>(1.0 - 2.0 * (x * x + y * y));
    matrix.m[0][3] = pose[0];
    matrix.m[1][3] = pose[1];
    matrix.m[2][3] = pose[2];
    return matrix;
}

class Controller final : public vr::ITrackedDeviceServerDriver {
public:
    explicit Controller(bool left) : left_(left) {}

    vr::EVRInitError Activate(uint32_t objectId) override {
        objectId_ = objectId;
        auto* properties = vr::VRProperties();
        const auto container = properties->TrackedDeviceToPropertyContainer(objectId);
        properties->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "KitVR");
        properties->SetStringProperty(container, vr::Prop_ModelNumber_String, "KitVR Touch Controller");
        properties->SetStringProperty(container, vr::Prop_ManufacturerName_String, "Crystal Kitsune Studios");
        properties->SetStringProperty(container, vr::Prop_InputProfilePath_String,
                                       "{kitvr}/input/kitvr_controller_profile.json");
        properties->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32,
                                     left_ ? vr::TrackedControllerRole_LeftHand
                                           : vr::TrackedControllerRole_RightHand);
        properties->SetUint64Property(container, vr::Prop_SupportedButtons_Uint64,
                                      vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu) |
                                          vr::ButtonMaskFromId(vr::k_EButton_Grip) |
                                          vr::ButtonMaskFromId(vr::k_EButton_A) |
                                          vr::ButtonMaskFromId(vr::k_EButton_Axis0) |
                                          vr::ButtonMaskFromId(vr::k_EButton_Axis1));
        input_ = vr::VRDriverInput();
        if (!input_) return vr::VRInitError_Driver_Failed;
        if (!createComponents(container)) return vr::VRInitError_Driver_Failed;
        return vr::VRInitError_None;
    }

    void Deactivate() override { objectId_ = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t size) override {
        if (size > 0) response[0] = '\0';
    }

    vr::DriverPose_t GetPose() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return pose_;
    }

    uint32_t objectId() const { return objectId_; }

    void update(const HandState& hand, bool valid) {
        vr::DriverPose_t pose{};
        pose.poseTimeOffset = 0.0;
        pose.qWorldFromDriverRotation = {1.0, 0.0, 0.0, 0.0};
        pose.qDriverFromHeadRotation = {1.0, 0.0, 0.0, 0.0};
        pose.vecPosition[0] = hand.pose[0];
        pose.vecPosition[1] = hand.pose[1];
        pose.vecPosition[2] = hand.pose[2];
        pose.qRotation = {hand.pose[6], hand.pose[3], hand.pose[4], hand.pose[5]};
        pose.result = valid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
        pose.poseIsValid = valid;
        pose.deviceIsConnected = valid;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pose_ = pose;
        }
        if (!input_ || objectId_ == vr::k_unTrackedDeviceIndexInvalid) return;

        const double timeOffset = 0.0;
        input_->UpdateBooleanComponent(triggerClick_, (hand.buttons & kTriggerClick) != 0, timeOffset);
        input_->UpdateBooleanComponent(gripClick_, (hand.buttons & kGripClick) != 0, timeOffset);
        input_->UpdateBooleanComponent(primaryClick_, (hand.buttons & kPrimaryClick) != 0, timeOffset);
        input_->UpdateBooleanComponent(secondaryClick_, (hand.buttons & kSecondaryClick) != 0, timeOffset);
        input_->UpdateBooleanComponent(menuClick_, (hand.buttons & kMenuClick) != 0, timeOffset);
        input_->UpdateBooleanComponent(thumbstickClick_, (hand.buttons & kThumbstickClick) != 0, timeOffset);
        input_->UpdateScalarComponent(triggerValue_, hand.trigger, timeOffset);
        input_->UpdateScalarComponent(gripValue_, hand.grip, timeOffset);
        input_->UpdateScalarComponent(thumbstickX_, hand.stickX, timeOffset);
        input_->UpdateScalarComponent(thumbstickY_, hand.stickY, timeOffset);
        const auto matrix = poseMatrix(hand.pose);
        input_->UpdatePoseComponent(poseComponent_, &matrix, timeOffset);
    }

private:
    bool createComponents(vr::PropertyContainerHandle_t container) {
        const auto makeBoolean = [&](const char* path, vr::VRInputComponentHandle_t& handle) {
            return input_->CreateBooleanComponent(container, path, &handle) == vr::VRInputError_None;
        };
        const auto makeScalar = [&](const char* path, vr::EVRScalarUnits units,
                                    vr::VRInputComponentHandle_t& handle) {
            return input_->CreateScalarComponent(container, path, &handle,
                                                 vr::VRScalarType_Absolute, units) == vr::VRInputError_None;
        };
        const std::string root = "/input/";
        if (!makeBoolean((root + "trigger/click").c_str(), triggerClick_) ||
            !makeBoolean((root + "grip/click").c_str(), gripClick_) ||
            !makeBoolean((root + "primary/click").c_str(), primaryClick_) ||
            !makeBoolean((root + "secondary/click").c_str(), secondaryClick_) ||
            !makeBoolean((root + "menu/click").c_str(), menuClick_) ||
            !makeBoolean((root + "joystick/click").c_str(), thumbstickClick_) ||
            !makeScalar((root + "trigger/value").c_str(), vr::VRScalarUnits_NormalizedOneSided, triggerValue_) ||
            !makeScalar((root + "grip/value").c_str(), vr::VRScalarUnits_NormalizedOneSided, gripValue_) ||
            !makeScalar((root + "joystick/x").c_str(), vr::VRScalarUnits_NormalizedTwoSided, thumbstickX_) ||
            !makeScalar((root + "joystick/y").c_str(), vr::VRScalarUnits_NormalizedTwoSided, thumbstickY_) ||
            input_->CreatePoseComponent(container, "/pose/raw", &poseComponent_) != vr::VRInputError_None) {
            return false;
        }
        return true;
    }

    bool left_;
    uint32_t objectId_ = vr::k_unTrackedDeviceIndexInvalid;
    vr::IVRDriverInput* input_ = nullptr;
    std::mutex mutex_;
    vr::DriverPose_t pose_{};
    vr::VRInputComponentHandle_t triggerClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t gripClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t primaryClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t secondaryClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t menuClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t thumbstickClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t triggerValue_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t gripValue_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t thumbstickX_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t thumbstickY_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t poseComponent_ = vr::k_ulInvalidInputComponentHandle;
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

        auto* host = vr::VRServerDriverHost();
        if (!host->TrackedDeviceAdded("KITVR-HMD-001", vr::TrackedDeviceClass_HMD, &hmd_) ||
            !host->TrackedDeviceAdded("KITVR-L-CONTROLLER-001", vr::TrackedDeviceClass_Controller,
                                      &leftController_) ||
            !host->TrackedDeviceAdded("KITVR-R-CONTROLLER-001", vr::TrackedDeviceClass_Controller,
                                      &rightController_)) {
            return vr::VRInitError_Driver_HmdUnknown;
        }
        return vr::VRInitError_None;
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
        std::array<HandState, 2> hands{};
        for (;;) {
#ifdef _WIN32
            const int count = recv(socket_, reinterpret_cast<char*>(packet.data()),
                                   static_cast<int>(packet.size()), 0);
#else
            const int count = static_cast<int>(recv(socket_, packet.data(), packet.size(), 0));
#endif
            if (count < static_cast<int>(kTrackingPacketLength)) break;
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
                for (size_t handIndex = 0; handIndex < hands.size(); ++handIndex) {
                    const size_t offset = handIndex == 0 ? 40 : 88;
                    auto& hand = hands[handIndex];
                    hand.buttons = readU16Le(packet.data() + offset);
                    hand.active = (readU16Le(packet.data() + offset + 2) & 1) != 0;
                    hand.trigger = readF32Le(packet.data() + offset + 4);
                    hand.grip = readF32Le(packet.data() + offset + 8);
                    hand.stickX = readF32Le(packet.data() + offset + 12);
                    hand.stickY = readF32Le(packet.data() + offset + 16);
                    bool handValid = std::isfinite(hand.trigger) && std::isfinite(hand.grip) &&
                                     std::isfinite(hand.stickX) && std::isfinite(hand.stickY);
                    for (size_t i = 0; i < 7; ++i) {
                        hand.pose[i] = readF32Le(packet.data() + offset + 20 + i * sizeof(float));
                        handValid &= std::isfinite(hand.pose[i]);
                    }
                    const float handNorm = std::sqrt(hand.pose[3] * hand.pose[3] +
                                                     hand.pose[4] * hand.pose[4] +
                                                     hand.pose[5] * hand.pose[5] +
                                                     hand.pose[6] * hand.pose[6]);
                    hand.active &= handValid && handNorm > 1e-6f;
                    if (hand.active) {
                        for (size_t i = 3; i < 7; ++i) hand.pose[i] /= handNorm;
                    }
                    hand.trigger = std::clamp(hand.trigger, 0.0f, 1.0f);
                    hand.grip = std::clamp(hand.grip, 0.0f, 1.0f);
                    hand.stickX = std::clamp(hand.stickX, -1.0f, 1.0f);
                    hand.stickY = std::clamp(hand.stickY, -1.0f, 1.0f);
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (gotPose) {
            hmd_.updatePose(head, true);
            leftController_.update(hands[0], hands[0].active);
            rightController_.update(hands[1], hands[1].active);
            lastPose_ = now;
            havePose_ = true;
        } else if (havePose_ && now - lastPose_ > std::chrono::milliseconds(250)) {
            hmd_.updatePose(head, false);
            leftController_.update(HandState{}, false);
            rightController_.update(HandState{}, false);
            havePose_ = false;
        }

        if (hmd_.objectId() != vr::k_unTrackedDeviceIndexInvalid) {
            const auto pose = hmd_.GetPose();
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(
                hmd_.objectId(), pose, sizeof(pose));
        }
        for (Controller* controller : {&leftController_, &rightController_}) {
            if (controller->objectId() != vr::k_unTrackedDeviceIndexInvalid) {
                const auto pose = controller->GetPose();
                vr::VRServerDriverHost()->TrackedDevicePoseUpdated(
                    controller->objectId(), pose, sizeof(pose));
            }
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
    Controller leftController_{true};
    Controller rightController_{false};
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
