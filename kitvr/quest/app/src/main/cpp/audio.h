// Audio through AAudio: plays PC audio received from the server and captures the headset
// microphone. Both run in low-latency mode with 16 bit PCM at the stream's sample rate.
#pragma once
#include <aaudio/AAudio.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace kitvr {

class Audio {
public:
    Audio() = default;
    ~Audio() { stop(); }
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool startPlayback(int sampleRate, int channels);
    bool playbackRunning() const { return playback_ != nullptr; }
    // Called from the network thread with interleaved s16 samples.
    void pushPlayback(const int16_t* samples, size_t count);

    // Calls onFrame from the audio thread with mono s16 frames of kMicFrameSamples.
    bool startCapture(int sampleRate, std::function<void(const int16_t*, size_t)> onFrame);
    bool captureRunning() const { return capture_ != nullptr; }

    void stop();

    static constexpr size_t kMicFrameSamples = 480;  // 10 ms at 48 kHz

private:
    // Single producer, single consumer ring of samples.
    struct Ring {
        std::vector<int16_t> buf;
        std::atomic<size_t> head{0};
        std::atomic<size_t> tail{0};
        void init(size_t capacity) {
            buf.assign(capacity, 0);
            head = 0;
            tail = 0;
        }
        size_t available() const { return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire); }
        size_t push(const int16_t* d, size_t n);
        size_t pop(int16_t* d, size_t n);
        void skip(size_t n) { tail.store(tail.load(std::memory_order_relaxed) + n, std::memory_order_release); }
    };

    static aaudio_data_callback_result_t playbackCallback(AAudioStream*, void* user, void* data, int32_t frames);
    static aaudio_data_callback_result_t captureCallback(AAudioStream*, void* user, void* data, int32_t frames);

    AAudioStream* playback_ = nullptr;
    AAudioStream* capture_ = nullptr;

    Ring ring_;
    int channels_ = 2;
    size_t primeSamples_ = 0;   // buffered audio needed before playback starts
    size_t targetSamples_ = 0;  // where playback settles after trimming
    size_t highWater_ = 0;      // above this, old audio is dropped to bound latency
    std::atomic<bool> primed_{false};

    std::function<void(const int16_t*, size_t)> onMic_;
    int16_t micAcc_[kMicFrameSamples] = {};
    size_t micAccCount_ = 0;
};

}  // namespace kitvr
