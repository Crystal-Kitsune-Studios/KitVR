// Networking: control channel (TCP), media channel (UDP), frame reassembly.
#pragma once
#include <android/native_window.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "decoder.h"
#include "proto.h"

namespace kitvr {

class Session {
public:
    Session() = default;
    ~Session() { shutdown(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Connects (and keeps reconnecting) to the server's control port on a background thread.
    void startControl(const std::string& host);

    // True once for each StreamConfig received from the server.
    bool takeConfig(StreamConfig& out);

    // Starts the UDP receiver and the decoder, which renders into `window`.
    bool startMedia(const StreamConfig& cfg, ANativeWindow* window);
    void stopMedia();
    bool mediaRunning() const { return mediaRunning_; }

    // head = {px, py, pz, qx, qy, qz, qw}. The first packet tells the server where to send video.
    void sendTracking(uint64_t tsUs, const float head[7]);

    // Call once per frame from the main thread. Starts audio streams when they are due.
    void tick();
    // True when the server wants microphone audio, so the app should hold the permission.
    bool wantsMic();
    void setMicPermission(bool granted) { micAllowed_ = granted; }

    void shutdown();

private:
    void controlLoop();
    void mediaLoop();
    bool handleMessage(const Message& m);  // false means close the connection
    void sendControl(const std::vector<uint8_t>& data);

    std::string host_;
    std::atomic<bool> running_{false};
    std::thread controlThread_;
    std::mutex sendMutex_;
    int controlFd_ = -1;  // guarded by sendMutex_

    std::mutex cfgMutex_;
    std::optional<StreamConfig> pendingCfg_;
    std::optional<AudioConfig> audioCfg_;
    std::atomic<uint32_t> epoch_{0};  // bumped on every new control session

    int udpFd_ = -1;
    std::atomic<bool> mediaRunning_{false};
    std::atomic<bool> mediaStop_{false};
    std::thread mediaThread_;
    VideoDecoder decoder_;
    std::atomic<uint16_t> trackSeq_{0};

    Audio audio_;
    std::atomic<bool> micAllowed_{false};
    std::atomic<uint16_t> micSeq_{0};
    uint64_t lastAudioTryUs_ = 0;
    void onMicFrame(const int16_t* samples, size_t count);
};

}  // namespace kitvr
