#include "audio.h"

#include <algorithm>
#include <cstring>

#include "log.h"

namespace kitvr {

size_t Audio::Ring::push(const int16_t* d, size_t n) {
    size_t h = head.load(std::memory_order_relaxed);
    size_t t = tail.load(std::memory_order_acquire);
    size_t freeSpace = buf.size() - (h - t);
    n = std::min(n, freeSpace);
    for (size_t i = 0; i < n; i++) buf[(h + i) % buf.size()] = d[i];
    head.store(h + n, std::memory_order_release);
    return n;
}

size_t Audio::Ring::pop(int16_t* d, size_t n) {
    size_t t = tail.load(std::memory_order_relaxed);
    size_t h = head.load(std::memory_order_acquire);
    n = std::min(n, h - t);
    for (size_t i = 0; i < n; i++) d[i] = buf[(t + i) % buf.size()];
    tail.store(t + n, std::memory_order_release);
    return n;
}

bool Audio::startPlayback(int sampleRate, int channels) {
    if (playback_) return true;
    channels_ = channels;
    const size_t perMs = static_cast<size_t>(sampleRate) * static_cast<size_t>(channels) / 1000;
    ring_.init(perMs * 400);  // 400 ms of headroom
    primeSamples_ = perMs * 30;
    targetSamples_ = perMs * 30;
    highWater_ = perMs * 80;
    primed_ = false;

    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, sampleRate);
    AAudioStreamBuilder_setChannelCount(b, channels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(b, &Audio::playbackCallback, this);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &playback_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("audio playback open failed: %s", AAudio_convertResultToText(r));
        playback_ = nullptr;
        return false;
    }
    AAudioStream_setBufferSizeInFrames(playback_, AAudioStream_getFramesPerBurst(playback_) * 2);
    r = AAudioStream_requestStart(playback_);
    if (r != AAUDIO_OK) {
        LOGE("audio playback start failed: %s", AAudio_convertResultToText(r));
        AAudioStream_close(playback_);
        playback_ = nullptr;
        return false;
    }
    LOGI("audio playback started: %d Hz, %d ch", sampleRate, channels);
    return true;
}

void Audio::pushPlayback(const int16_t* samples, size_t count) {
    if (!playback_) return;
    ring_.push(samples, count);
}

aaudio_data_callback_result_t Audio::playbackCallback(AAudioStream*, void* user, void* data, int32_t frames) {
    auto* a = static_cast<Audio*>(user);
    auto* out = static_cast<int16_t*>(data);
    const size_t want = static_cast<size_t>(frames) * static_cast<size_t>(a->channels_);

    size_t avail = a->ring_.available();
    if (!a->primed_) {
        if (avail < a->primeSamples_) {
            std::memset(out, 0, want * sizeof(int16_t));
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        a->primed_ = true;
    }
    if (avail > a->highWater_) {
        // Fell behind (a burst or a clock difference). Drop the oldest audio, whole frames only.
        size_t drop = avail - a->targetSamples_;
        drop -= drop % static_cast<size_t>(a->channels_);
        a->ring_.skip(drop);
    }
    size_t got = a->ring_.pop(out, want);
    if (got < want) {
        std::memset(out + got, 0, (want - got) * sizeof(int16_t));
        a->primed_ = false;  // underrun: refill before resuming
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool Audio::startCapture(int sampleRate, std::function<void(const int16_t*, size_t)> onFrame) {
    if (capture_) return true;
    onMic_ = std::move(onFrame);
    micAccCount_ = 0;

    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(b, sampleRate);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setDataCallback(b, &Audio::captureCallback, this);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &capture_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("microphone open failed: %s", AAudio_convertResultToText(r));
        capture_ = nullptr;
        return false;
    }
    r = AAudioStream_requestStart(capture_);
    if (r != AAUDIO_OK) {
        LOGE("microphone start failed: %s", AAudio_convertResultToText(r));
        AAudioStream_close(capture_);
        capture_ = nullptr;
        return false;
    }
    LOGI("microphone started: %d Hz mono", sampleRate);
    return true;
}

aaudio_data_callback_result_t Audio::captureCallback(AAudioStream*, void* user, void* data, int32_t frames) {
    auto* a = static_cast<Audio*>(user);
    const auto* in = static_cast<const int16_t*>(data);
    size_t left = static_cast<size_t>(frames);
    while (left > 0) {
        size_t n = std::min(left, kMicFrameSamples - a->micAccCount_);
        std::memcpy(a->micAcc_ + a->micAccCount_, in, n * sizeof(int16_t));
        a->micAccCount_ += n;
        in += n;
        left -= n;
        if (a->micAccCount_ == kMicFrameSamples) {
            if (a->onMic_) a->onMic_(a->micAcc_, kMicFrameSamples);
            a->micAccCount_ = 0;
        }
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void Audio::stop() {
    if (playback_) {
        AAudioStream_requestStop(playback_);
        AAudioStream_close(playback_);
        playback_ = nullptr;
    }
    if (capture_) {
        AAudioStream_requestStop(capture_);
        AAudioStream_close(capture_);
        capture_ = nullptr;
    }
}

}  // namespace kitvr
