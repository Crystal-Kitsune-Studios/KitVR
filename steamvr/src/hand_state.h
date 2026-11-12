#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "tracking_pose.h"

namespace kitvr {

struct HandState {
    uint16_t buttons = 0;
    bool active = false;
    float trigger = 0.0f;
    float grip = 0.0f;
    float stickX = 0.0f;
    float stickY = 0.0f;
    float pose[7] = {0, 0, 0, 0, 0, 0, 1};
};

inline bool validateHandState(HandState& hand) {
    if (!hand.active) {
        hand = HandState{};
        return true;
    }
    bool valid = std::isfinite(hand.trigger) && std::isfinite(hand.grip) &&
                 std::isfinite(hand.stickX) && std::isfinite(hand.stickY);
    if (!valid || !normalizeTrackingPose(hand.pose)) {
        hand = HandState{};
        return false;
    }
    hand.trigger = std::clamp(hand.trigger, 0.0f, 1.0f);
    hand.grip = std::clamp(hand.grip, 0.0f, 1.0f);
    hand.stickX = std::clamp(hand.stickX, -1.0f, 1.0f);
    hand.stickY = std::clamp(hand.stickY, -1.0f, 1.0f);
    return true;
}

}  // namespace kitvr
