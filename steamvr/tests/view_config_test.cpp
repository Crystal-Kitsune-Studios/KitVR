#include "view_config.h"
#include "../../quest/app/src/main/cpp/proto.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {
void putFloat(std::array<uint8_t, kitvr::kViewConfigPacketLength>& packet, size_t index, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    for (size_t i = 0; i < 4; ++i) packet[4 + index * 4 + i] = static_cast<uint8_t>(bits >> (i * 8));
}
}

int main() {
    kitvr::EyeViewConfig clientViews[2]{};
    for (size_t eye = 0; eye < 2; ++eye) {
        const float values[] = {-1.0f, 1.1f, -0.9f, 1.0f, eye ? 0.032f : -0.032f,
                                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f};
        std::copy(values, values + 4, clientViews[eye].projection);
        std::copy(values + 4, values + 11, clientViews[eye].eyeToHead);
    }
    const auto encoded = kitvr::encodeViewConfig(clientViews);
    if (encoded.size() != kitvr::kViewConfigPacketLength) return 1;
    std::array<uint8_t, kitvr::kViewConfigPacketLength> packet{};
    std::copy(encoded.begin(), encoded.end(), packet.begin());
    std::array<kitvr::EyeView, 2> views{};
    if (!kitvr::parseViewConfig(packet.data(), packet.size(), views) ||
        views[0].eyeToHead[0] != -0.032f || views[1].eyeToHead[0] != 0.032f ||
        views[0].eyeToHead[6] != 1.0f || views[1].projection[2] != -0.9f) return 1;
    if (kitvr::parseViewConfig(packet.data(), packet.size() - 1, views)) return 2;
    putFloat(packet, 0, std::numeric_limits<float>::quiet_NaN());
    if (kitvr::parseViewConfig(packet.data(), packet.size(), views)) return 3;
    putFloat(packet, 0, 1.2f);
    if (kitvr::parseViewConfig(packet.data(), packet.size(), views)) return 4;
    putFloat(packet, 0, -1.0f);
    putFloat(packet, 10, 0.0f);
    if (kitvr::parseViewConfig(packet.data(), packet.size(), views)) return 5;
    return 0;
}
