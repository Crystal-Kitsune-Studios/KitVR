#include "decoder.h"
#include <media/NdkMediaFormat.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {
std::atomic<int> pendingOutput{0};
std::atomic<int> releaseStatus{0};
std::atomic<int> releaseCount{0};
AMediaCodec codec;
AMediaFormat format;

bool waitFor(const std::atomic<int>& value, int expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (value.load() < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load() >= expected;
}
}

AMediaCodec* AMediaCodec_createDecoderByType(const char*) { return &codec; }
media_status_t AMediaCodec_configure(AMediaCodec*, const AMediaFormat*, ANativeWindow*, void*, uint32_t) { return 0; }
media_status_t AMediaCodec_start(AMediaCodec*) { return 0; }
media_status_t AMediaCodec_stop(AMediaCodec*) { return 0; }
media_status_t AMediaCodec_delete(AMediaCodec*) { return 0; }
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t) { return -1; }
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t, size_t*) { return nullptr; }
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec*, size_t, off_t, size_t, uint64_t, uint32_t) { return 0; }
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec*, AMediaCodecBufferInfo* info, int64_t) {
    if (!pendingOutput.exchange(0)) return -1;
    *info = {};
    info->size = 1;
    return 0;
}
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t, bool) {
    const auto status = releaseStatus.load();
    ++releaseCount;
    return status;
}
AMediaFormat* AMediaFormat_new() { return &format; }
void AMediaFormat_delete(AMediaFormat*) {}
void AMediaFormat_setString(AMediaFormat*, const char*, const char*) {}
void AMediaFormat_setInt32(AMediaFormat*, const char*, int32_t) {}

int main() {
    kitvr::VideoDecoder decoder;
    if (!decoder.start(nullptr, "video/hevc", 1920, 1080, 60)) return 1;
    releaseStatus = -1;
    pendingOutput = 1;
    if (!waitFor(releaseCount, 1)) return 2;
    if (decoder.hasRenderedFrame()) return 3;
    releaseStatus = 0;
    pendingOutput = 1;
    if (!waitFor(releaseCount, 2)) return 4;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!decoder.hasRenderedFrame() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!decoder.hasRenderedFrame()) return 5;
    decoder.stop();
    return 0;
}
