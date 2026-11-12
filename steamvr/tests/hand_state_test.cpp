#include "hand_state.h"

#include <cmath>
#include <limits>

int main() {
    kitvr::HandState hand;
    hand.buttons = 1;
    hand.trigger = 1.0f;
    hand.pose[0] = 4.0f;
    if (!kitvr::validateHandState(hand)) return 1;
    if (hand.active || hand.buttons != 0 || hand.trigger != 0.0f ||
        hand.pose[0] != 0.0f || hand.pose[6] != 1.0f) return 1;

    hand.active = true;
    hand.buttons = 1;
    hand.trigger = std::numeric_limits<float>::quiet_NaN();
    if (kitvr::validateHandState(hand)) return 2;
    if (hand.active || hand.buttons != 0 || hand.trigger != 0.0f) return 2;

    hand.active = true;
    hand.buttons = 2;
    hand.trigger = 2.0f;
    hand.stickX = -2.0f;
    hand.pose[6] = 2.0f;
    if (!kitvr::validateHandState(hand)) return 3;
    if (!hand.active || hand.buttons != 2 || hand.trigger != 1.0f || hand.stickX != -1.0f ||
        std::abs(hand.pose[6] - 1.0f) >= 1e-6f) return 3;

    hand.pose[6] = 0.0f;
    if (kitvr::validateHandState(hand)) return 4;
    if (hand.active || hand.buttons != 0 || hand.pose[6] != 1.0f) return 4;

    float pose[7] = {0, 0, 0, 0, 0, 0, 1e30f};
    if (!kitvr::normalizeTrackingPose(pose) || std::abs(pose[6] - 1.0f) >= 1e-6f) return 5;
    pose[0] = std::numeric_limits<float>::infinity();
    if (kitvr::normalizeTrackingPose(pose)) return 6;
    pose[0] = 0;
    pose[6] = 0;
    if (kitvr::normalizeTrackingPose(pose)) return 7;
}
