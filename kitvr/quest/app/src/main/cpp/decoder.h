// Hardware video decoder that renders straight into an Android Surface.
// The Surface comes from an OpenXR surface swapchain, so frames never touch our own GL code.
#pragma once
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace kitvr {

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder() { stop(); }
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool start(ANativeWindow* window, const char* mime, int width, int height, int fps);

    // Queues one access unit. Returns false if the queue overflowed and was cleared;
    // the caller must then wait for the next keyframe because references are broken.
    bool submit(std::vector<uint8_t>&& data, bool keyframe, int64_t ptsUs);

    void stop();

private:
    struct Au {
        std::vector<uint8_t> data;
        bool key = false;
        int64_t pts = 0;
    };

    void loop();

    AMediaCodec* codec_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Au> queue_;
};

}  // namespace kitvr
