#include "../src/driver.cpp"

#include <cmath>

namespace {

class TestProperties final : public vr::IVRProperties {
public:
    vr::ETrackedPropertyError ReadPropertyBatch(vr::PropertyContainerHandle_t,
                                                vr::PropertyRead_t*, uint32_t) override {
        return vr::TrackedProp_Success;
    }
    vr::ETrackedPropertyError WritePropertyBatch(vr::PropertyContainerHandle_t,
                                                 vr::PropertyWrite_t*, uint32_t) override {
        return vr::TrackedProp_Success;
    }
    const char* GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override { return ""; }
    vr::PropertyContainerHandle_t TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t) override {
        return 1;
    }
};

class TestInput final : public vr::IVRDriverInput {
public:
    vr::EVRInputError CreateBooleanComponent(vr::PropertyContainerHandle_t, const char*,
                                             vr::VRInputComponentHandle_t* handle) override {
        *handle = ++nextHandle;
        return vr::VRInputError_None;
    }
    vr::EVRInputError UpdateBooleanComponent(vr::VRInputComponentHandle_t, bool, double) override {
        return vr::VRInputError_None;
    }
    vr::EVRInputError CreateScalarComponent(vr::PropertyContainerHandle_t, const char*,
                                            vr::VRInputComponentHandle_t* handle,
                                            vr::EVRScalarType, vr::EVRScalarUnits) override {
        *handle = ++nextHandle;
        return vr::VRInputError_None;
    }
    vr::EVRInputError UpdateScalarComponent(vr::VRInputComponentHandle_t, float, double) override {
        return vr::VRInputError_None;
    }
    vr::EVRInputError CreateHapticComponent(vr::PropertyContainerHandle_t, const char*,
                                            vr::VRInputComponentHandle_t*) override {
        return vr::VRInputError_None;
    }
    vr::EVRInputError CreateSkeletonComponent(vr::PropertyContainerHandle_t, const char*, const char*,
                                              const char*, vr::EVRSkeletalTrackingLevel,
                                              const vr::VRBoneTransform_t*, uint32_t,
                                              vr::VRInputComponentHandle_t*) override {
        return vr::VRInputError_None;
    }
    vr::EVRInputError UpdateSkeletonComponent(vr::VRInputComponentHandle_t, vr::EVRSkeletalMotionRange,
                                              const vr::VRBoneTransform_t*, uint32_t) override {
        return vr::VRInputError_None;
    }
    vr::EVRInputError CreatePoseComponent(vr::PropertyContainerHandle_t, const char*,
                                          vr::VRInputComponentHandle_t* handle) override {
        *handle = ++nextHandle;
        poseHandle = *handle;
        return vr::VRInputError_None;
    }
    vr::EVRInputError UpdatePoseComponent(vr::VRInputComponentHandle_t handle,
                                          const vr::HmdMatrix34_t* offset, double) override {
        if (handle == poseHandle) {
            poseOffset = *offset;
            ++poseUpdates;
        }
        return vr::VRInputError_None;
    }
    vr::EVRInputError CreateEyeTrackingComponent(vr::PropertyContainerHandle_t, const char*,
                                                 vr::VRInputComponentHandle_t*) override {
        return vr::VRInputError_None;
    }
    vr::EVRInputError UpdateEyeTrackingComponent(vr::VRInputComponentHandle_t,
                                                  const vr::VREyeTrackingData_t*, double) override {
        return vr::VRInputError_None;
    }

    vr::VRInputComponentHandle_t nextHandle = 0;
    vr::VRInputComponentHandle_t poseHandle = 0;
    vr::HmdMatrix34_t poseOffset{};
    int poseUpdates = 0;
};

class TestContext final : public vr::IVRDriverContext {
public:
    void* GetGenericInterface(const char* version, vr::EVRInitError* error) override {
        if (error) *error = vr::VRInitError_None;
        if (std::strcmp(version, vr::IVRProperties_Version) == 0) return &properties;
        if (std::strcmp(version, vr::IVRDriverInput_Version) == 0) return &input;
        if (error) *error = vr::VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }
    vr::DriverHandle_t GetDriverHandle() override { return 1; }

    TestProperties properties;
    TestInput input;
};

}  // namespace

int main() {
    TestContext context;
    vr::VRDriverContext() = &context;
    vr::OpenVRInternal_ModuleServerDriverContext().Clear();
    Controller controller(false);
    if (controller.Activate(1) != vr::VRInitError_None) return 1;

    HandState hand;
    hand.active = true;
    hand.pose[0] = 1.0f;
    hand.pose[1] = 1.5f;
    hand.pose[2] = 0.5f;
    hand.pose[4] = 0.70710678f;
    hand.pose[6] = 0.70710678f;
    controller.update(hand, true);
    const auto devicePose = controller.GetPose();
    if (!devicePose.poseIsValid || devicePose.vecPosition[0] != hand.pose[0] ||
        devicePose.vecPosition[1] != hand.pose[1] || devicePose.vecPosition[2] != hand.pose[2] ||
        std::abs(devicePose.qRotation.y - hand.pose[4]) > 1e-6) return 2;
    if (context.input.poseUpdates != 1) return 3;
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            const float expected = row == column ? 1.0f : 0.0f;
            if (context.input.poseOffset.m[row][column] != expected) return 4;
        }
    }
    vr::CleanupDriverContext();
}
