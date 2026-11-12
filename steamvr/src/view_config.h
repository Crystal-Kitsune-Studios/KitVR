#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kitvr {

constexpr uint8_t kViewConfigPacketType = 6;
constexpr size_t kViewConfigPacketLength = 92;

struct EyeView {
    std::array<float, 4> projection;
    std::array<float, 7> eyeToHead;
};

inline bool parseViewConfig(const uint8_t* packet, size_t size, std::array<EyeView, 2>& out) {
    if (size != kViewConfigPacketLength || packet[0] != kViewConfigPacketType || packet[1] != 1) {
        return false;
    }
    std::array<EyeView, 2> eyes{};
    for (size_t eye = 0; eye < eyes.size(); ++eye) {
        const auto readFloat = [&](size_t index) {
            const size_t p = 4 + eye * 44 + index * 4;
            const uint32_t bits = static_cast<uint32_t>(packet[p]) |
                (static_cast<uint32_t>(packet[p + 1]) << 8) |
                (static_cast<uint32_t>(packet[p + 2]) << 16) |
                (static_cast<uint32_t>(packet[p + 3]) << 24);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        };
        for (size_t i = 0; i < 4; ++i) {
            eyes[eye].projection[i] = readFloat(i);
            if (!std::isfinite(eyes[eye].projection[i]) || std::abs(eyes[eye].projection[i]) > 10.0f) return false;
        }
        if (eyes[eye].projection[0] >= eyes[eye].projection[1] ||
            eyes[eye].projection[2] >= eyes[eye].projection[3]) return false;
        for (size_t i = 0; i < 7; ++i) {
            eyes[eye].eyeToHead[i] = readFloat(i + 4);
            if (!std::isfinite(eyes[eye].eyeToHead[i])) return false;
            if (i < 3 && std::abs(eyes[eye].eyeToHead[i]) > 1.0f) return false;
        }
        const auto& p = eyes[eye].eyeToHead;
        const float norm = std::sqrt(p[3] * p[3] + p[4] * p[4] + p[5] * p[5] + p[6] * p[6]);
        if (!std::isfinite(norm) || norm < 0.5f || norm > 2.0f) return false;
        for (size_t i = 3; i < 7; ++i) eyes[eye].eyeToHead[i] /= norm;
    }
    out = eyes;
    return true;
}

}  // namespace kitvr
