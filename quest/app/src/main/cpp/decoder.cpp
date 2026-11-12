#include "decoder.h"

#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>

#include "log.h"

namespace kitvr {

namespace {
constexpr size_t kMaxQueue = 8;
constexpr uint32_t kBufferFlagKeyFrame = 1;  // MediaCodec.BUFFER_FLAG_KEY_FRAME
}  // namespace

bool VideoDecoder::start(ANativeWindow* window, const char* mime, int width, int height, int fps) {
    stop();
    firstFrameRendered_ = false;
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) {
        LOGE("no decoder for %s", mime);
        return false;
    }

    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, height);
    // Literal keys, because the named constants only exist on newer API levels.
    AMediaFormat_setInt32(f, "low-latency", 1);
    AMediaFormat_setInt32(f, "priority", 0);  // realtime
    AMediaFormat_setInt32(f, "operating-rate", fps);

    media_status_t st = AMediaCodec_configure(codec_, f, window, nullptr, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK) {
        LOGE("AMediaCodec_configure failed: %d", static_cast<int>(st));
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }
    st = AMediaCodec_start(codec_);
    if (st != AMEDIA_OK) {
        LOGE("AMediaCodec_start failed: %d", static_cast<int>(st));
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }

    running_ = true;
    thread_ = std::thread(&VideoDecoder::loop, this);
    LOGI("decoder started: %s %dx%d @%d", mime, width, height, fps);
    return true;
}

bool VideoDecoder::submit(std::vector<uint8_t>&& data, bool keyframe, int64_t ptsUs) {
    if (!running_) return false;
    std::lock_guard<std::mutex> lk(m_);
    if (recoveryNeeded_ && !keyframe) return false;
    if (queue_.size() >= kMaxQueue) {
        queue_.clear();
        recoveryNeeded_ = true;
        LOGW("decoder queue overflow, dropping frames");
        return false;
    }
    if (keyframe) recoveryNeeded_ = false;
    queue_.push_back(Au{std::move(data), keyframe, ptsUs});
    cv_.notify_one();
    return true;
}

void VideoDecoder::loop() {
    std::optional<Au> cur;
    while (running_) {
        if (!cur) {
            std::unique_lock<std::mutex> lk(m_);
            if (queue_.empty()) cv_.wait_for(lk, std::chrono::milliseconds(2));
            if (!queue_.empty()) {
                cur = std::move(queue_.front());
                queue_.pop_front();
            }
        }

        if (cur) {
            ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 0);
            if (idx >= 0) {
                size_t cap = 0;
                uint8_t* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
                const bool valid = buf && cur->data.size() <= cap;
                if (valid) std::memcpy(buf, cur->data.data(), cur->data.size());
                const media_status_t status = AMediaCodec_queueInputBuffer(
                    codec_, static_cast<size_t>(idx), 0, valid ? cur->data.size() : 0,
                    static_cast<uint64_t>(cur->pts), valid && cur->key ? kBufferFlagKeyFrame : 0);
                if (!valid || status != AMEDIA_OK) {
                    LOGW("decoder input failed or was too small (%zu bytes, capacity %zu, status %d)",
                         cur->data.size(), cap, static_cast<int>(status));
                    std::lock_guard<std::mutex> lk(m_);
                    queue_.clear();
                    recoveryNeeded_ = true;
                }
                cur.reset();
            }
        }

        // Render every finished frame immediately. The surface belongs to the compositor.
        for (;;) {
            AMediaCodecBufferInfo info;
            ssize_t o = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
            if (o >= 0) {
                const media_status_t status =
                    AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(o), true);
                if (status != AMEDIA_OK) {
                    LOGE("decoder output release failed: %d", static_cast<int>(status));
                } else if (info.size > 0) {
                    firstFrameRendered_ = true;
                }
                continue;
            }
            if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
                o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            break;
        }

        if (cur) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void VideoDecoder::stop() {
    if (running_.exchange(false)) {
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    } else if (thread_.joinable()) {
        thread_.join();
    }
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    std::lock_guard<std::mutex> lk(m_);
    queue_.clear();
    recoveryNeeded_ = false;
}

}  // namespace kitvr
