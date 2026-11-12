#pragma once

#include <cmath>
#include <cstddef>

namespace kitvr {

inline bool normalizeTrackingPose(float (&pose)[7]) {
    for (float value : pose) {
        if (!std::isfinite(value)) return false;
    }
    const double norm = std::sqrt(static_cast<double>(pose[3]) * pose[3] +
                                  static_cast<double>(pose[4]) * pose[4] +
                                  static_cast<double>(pose[5]) * pose[5] +
                                  static_cast<double>(pose[6]) * pose[6]);
    if (!std::isfinite(norm) || norm <= 1e-6) return false;
    for (std::size_t i = 3; i < 7; ++i) pose[i] = static_cast<float>(pose[i] / norm);
    return true;
}

}  // namespace kitvr
