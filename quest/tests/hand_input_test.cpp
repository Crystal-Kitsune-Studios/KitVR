#include "hand_input.h"

#include <limits>

int main() {
    int controllerReads = 0;
    int trackedReads = 0;
    kitvr::TrackingHand controller;
    controller.buttons = 1;
    controller.trigger = 1.0f;
    kitvr::TrackingHand tracked;
    tracked.active = true;
    tracked.grip = 0.6f;
    const auto readController = [&] { ++controllerReads; return controller; };
    const auto readTracked = [&] { ++trackedReads; return tracked; };

    auto result = kitvr::readPreferredHand(false, readController, readTracked);
    if (controllerReads != 0 || trackedReads != 1 || !result.active ||
        result.buttons != 0 || result.trigger != 0.0f || result.grip != 0.6f) return 1;

    result = kitvr::readPreferredHand(true, readController, readTracked);
    if (controllerReads != 1 || trackedReads != 2 || !result.active ||
        result.buttons != 0 || result.trigger != 0.0f || result.grip != 0.6f) return 2;

    controller.active = true;
    result = kitvr::readPreferredHand(true, readController, readTracked);
    if (controllerReads != 2 || trackedReads != 2 || !result.active ||
        result.buttons != 1 || result.trigger != 1.0f) return 3;

    tracked = {};
    result = kitvr::readPreferredHand(false, readController, readTracked);
    if (result.active || result.buttons != 0) return 4;

    std::array<kitvr::HandJoint, kitvr::HandJointCount> joints{};
    joints[kitvr::Palm].positionValid = true;
    joints[kitvr::Palm].orientationValid = true;
    joints[kitvr::Palm].position = {1, 2, 3};
    joints[kitvr::Palm].orientation = {0, 0, 0, 2};
    result = kitvr::trackedHandFromJoints(joints);
    if (!result.active || result.pose[0] != 1 || result.pose[6] != 1 ||
        result.trigger != 0 || result.grip != 0 || result.buttons != 0) return 5;

    for (size_t i = kitvr::ThumbTip; i < kitvr::HandJointCount; ++i) {
        joints[i].positionValid = true;
        joints[i].position = {1, 2, 3};
    }
    result = kitvr::trackedHandFromJoints(joints);
    if (!result.active || result.trigger != 1 || result.grip != 1 || result.buttons != 3) return 6;

    joints[kitvr::MiddleTip].position = {2, 2, 3};
    joints[kitvr::RingTip].position = {2, 2, 3};
    joints[kitvr::LittleTip].position = {2, 2, 3};
    result = kitvr::trackedHandFromJoints(joints);
    if (result.trigger != 1 || result.grip != 0 || result.buttons != 1) return 11;
    joints[kitvr::MiddleTip].position = {1, 2, 3};
    joints[kitvr::RingTip].position = {1, 2, 3};
    joints[kitvr::LittleTip].position = {1, 2, 3};

    joints[kitvr::ThumbTip].positionValid = false;
    result = kitvr::trackedHandFromJoints(joints);
    if (!result.active || result.trigger != 0 || result.grip != 1 || result.buttons != 2) return 7;

    joints[kitvr::IndexTip].position[0] = std::numeric_limits<float>::quiet_NaN();
    result = kitvr::trackedHandFromJoints(joints);
    if (!result.active || result.trigger != 0 || result.grip != 0 || result.buttons != 0) return 8;

    joints[kitvr::Palm].orientation = {0, 0, 0, 0};
    if (kitvr::trackedHandFromJoints(joints).active) return 9;
    joints[kitvr::Palm].orientation = {0, 0, 0, 1};
    joints[kitvr::Palm].position[0] = std::numeric_limits<float>::infinity();
    if (kitvr::trackedHandFromJoints(joints).active) return 10;
}
