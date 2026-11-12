#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "proto.h"

namespace kitvr {

struct HandJoint {
    bool positionValid = false;
    bool orientationValid = false;
    std::array<float, 3> position{};
    std::array<float, 4> orientation{};
};

enum HandJointIndex : size_t { Palm, ThumbTip, IndexTip, MiddleTip, RingTip, LittleTip, HandJointCount };

inline TrackingHand trackedHandFromJoints(const std::array<HandJoint, HandJointCount>& joints) {
    TrackingHand hand{};
    const auto& palm = joints[Palm];
    if (!palm.positionValid || !palm.orientationValid) return hand;
    for (float value : palm.position) if (!std::isfinite(value)) return hand;
    double normSquared = 0;
    for (float value : palm.orientation) {
        if (!std::isfinite(value)) return hand;
        normSquared += static_cast<double>(value) * value;
    }
    if (normSquared <= 1e-12) return hand;
    const double norm = std::sqrt(normSquared);
    hand.active = true;
    for (size_t i = 0; i < 3; ++i) hand.pose[i] = palm.position[i];
    for (size_t i = 0; i < 4; ++i) hand.pose[3 + i] = static_cast<float>(palm.orientation[i] / norm);

    const auto validPosition = [](const HandJoint& joint) {
        return joint.positionValid && std::all_of(joint.position.begin(), joint.position.end(),
                                                   [](float value) { return std::isfinite(value); });
    };
    const auto distance = [](const HandJoint& a, const HandJoint& b) {
        const double x = static_cast<double>(a.position[0]) - b.position[0];
        const double y = static_cast<double>(a.position[1]) - b.position[1];
        const double z = static_cast<double>(a.position[2]) - b.position[2];
        return std::sqrt(x * x + y * y + z * z);
    };
    if (validPosition(joints[ThumbTip]) && validPosition(joints[IndexTip])) {
        hand.trigger = static_cast<float>(std::clamp(
            (0.045 - distance(joints[ThumbTip], joints[IndexTip])) / 0.03, 0.0, 1.0));
    }
    double averageDistance = 0;
    bool curlValid = true;
    for (size_t i = IndexTip; i <= LittleTip; ++i) {
        if (!validPosition(joints[i])) {
            curlValid = false;
            break;
        }
        averageDistance += distance(joints[i], palm) / 4.0;
    }
    if (curlValid) hand.grip = static_cast<float>(std::clamp((0.105 - averageDistance) / 0.065, 0.0, 1.0));
    if (hand.trigger > 0.75f) hand.buttons |= 1u << 0;
    if (hand.grip > 0.75f) hand.buttons |= 1u << 1;
    return hand;
}

template <typename ReadController, typename ReadTrackedHand>
TrackingHand readPreferredHand(bool actionsSynchronized, ReadController readController,
                               ReadTrackedHand readTrackedHand) {
    if (actionsSynchronized) {
        TrackingHand controller = readController();
        if (controller.active) return controller;
    }
    return readTrackedHand();
}

}  // namespace kitvr
