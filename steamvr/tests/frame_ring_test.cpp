// Windows only: publishes frames through kitvr::FrameRing and reads them back through a second,
// read-only mapping, the way kitvr-server does.
#include "frame_capture_win.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {
uint32_t u32(const uint8_t* base, size_t offset) {
    uint32_t value;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}
uint64_t u64(const uint8_t* base, size_t offset) {
    uint64_t value;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}
}  // namespace

int main() {
    _putenv_s("KITVR_STEAMVR_SHM", "kitvr-frame-ring-test");
    kitvr::FrameRing ring;
    if (!ring.open()) return 1;

    HANDLE reader = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\kitvr-frame-ring-test");
    if (!reader) return 2;
    const auto* base = static_cast<const uint8_t*>(MapViewOfFile(reader, FILE_MAP_READ, 0, 0, 0));
    if (!base) return 3;
    if (u32(base, 0) != kitvr::kRingMagic || u32(base, 4) != kitvr::kRingVersion) return 4;
    if (u64(base, 32) != 0) return 5;

    // Tightly packed rows.
    std::vector<uint8_t> pixels(4 * 2 * 4);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i);
    if (!ring.publish(4, 2, kitvr::RingFormatRgba8, pixels.data(), 16)) return 6;
    if (u32(base, 8) != 4 || u32(base, 12) != 2 || u32(base, 16) != kitvr::RingFormatRgba8 ||
        u32(base, 20) != 16 || u32(base, 24) != 32 || u64(base, 32) != 1 || u64(base, 40) == 0) return 7;
    if (std::memcmp(base + kitvr::kRingHeaderBytes, pixels.data(), pixels.size()) != 0) return 8;

    // Padded rows, as D3D11 Map returns them, and a larger frame that commits more pages.
    const uint32_t width = 1832, height = 1920, pitch = width * 4 + 64;
    std::vector<uint8_t> padded(static_cast<size_t>(pitch) * height, 0xEE);
    for (uint32_t row = 0; row < height; ++row) {
        std::memset(padded.data() + static_cast<size_t>(row) * pitch, static_cast<int>(row & 0xFF), width * 4);
    }
    if (!ring.publish(width, height, kitvr::RingFormatBgra8, padded.data(), pitch)) return 9;
    if (u64(base, 32) != 2 || u32(base, 20) != width * 4) return 10;
    for (uint32_t row = 0; row < height; row += 97) {
        const uint8_t* line = base + kitvr::kRingHeaderBytes + static_cast<size_t>(row) * width * 4;
        if (line[0] != (row & 0xFF) || line[width * 4 - 1] != (row & 0xFF)) return 11;
    }

    // Formats the readback supports, and one it rejects.
    if (kitvr::ringFormatForDxgi(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) != kitvr::RingFormatRgba8 ||
        kitvr::ringFormatForDxgi(DXGI_FORMAT_B8G8R8A8_UNORM) != kitvr::RingFormatBgra8 ||
        kitvr::ringFormatForDxgi(DXGI_FORMAT_R10G10B10A2_UNORM) != kitvr::RingFormatX2Bgr10 ||
        kitvr::ringFormatForDxgi(DXGI_FORMAT_R16G16B16A16_FLOAT) != kitvr::RingFormatUnsupported) return 12;

    UnmapViewOfFile(base);
    CloseHandle(reader);
    return 0;
}
