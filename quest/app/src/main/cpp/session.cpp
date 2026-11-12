#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>

#include "control_send.h"
#include "log.h"
#include "video_frame.h"

namespace kitvr {

namespace {

constexpr uint64_t kControlTimeoutUs = 5000000;

int connectSocket(const std::string& host, uint16_t port, int socktype) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) return -1;

    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

void sleepWhile(const std::atomic<bool>& flag, int ms) {
    for (int waited = 0; waited < ms && flag; waited += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace

void Session::startControl(const std::string& host) {
    if (running_) return;
    host_ = host;
    running_ = true;
    controlThread_ = std::thread(&Session::controlLoop, this);
}

bool Session::takeConfig(StreamConfig& out, uint32_t& epoch) {
    std::lock_guard<std::mutex> lk(cfgMutex_);
    if (!pendingCfg_) return false;
    out = pendingCfg_->config;
    epoch = pendingCfg_->epoch;
    pendingCfg_.reset();
    return true;
}

void Session::sendControl(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lk(sendMutex_);
    if (controlFd_ < 0) return;
    if (!sendControlPacket(controlFd_, data.data(), data.size())) {
        LOGW("control send failed: %d; reconnecting", errno);
        controlStopping_ = true;
        controlReady_ = false;
        if (::shutdown(controlFd_, SHUT_RDWR) != 0) {
            LOGW("cannot shut down failed control connection: %d", errno);
        }
    }
}

bool Session::handleMessage(const Message& m) {
    switch (m.tag) {
        case kHello:
            LOGI("server hello: %s (protocol %u)", m.name.c_str(), m.version);
            if (m.version != kProtocolVersion) {
                LOGE("protocol version mismatch");
                return false;
            }
            {
                std::lock_guard<std::mutex> lk(sendMutex_);
                if (!running_ || controlStopping_) return false;
                {
                    std::lock_guard<std::mutex> configLock(cfgMutex_);
                    pendingCfg_.reset();
                    audioCfg_.reset();
                }
                epoch_++;
                controlReady_ = true;
            }
            break;
        case kStreamConfig: {
            LOGI("stream config: codec %u %ux%u @%u, %u kbps", m.cfg.codec, m.cfg.width,
                 m.cfg.height, m.cfg.fps, m.cfg.bitrateKbps);
            std::lock_guard<std::mutex> lk(cfgMutex_);
            pendingCfg_ = PendingConfig{m.cfg, epoch_};
            break;
        }
        case kAudioConfig: {
            LOGI("audio config: %u Hz, %u ch to headset, %u ch microphone", m.audio.sampleRate,
                 m.audio.channels, m.audio.micChannels);
            std::lock_guard<std::mutex> lk(cfgMutex_);
            audioCfg_ = m.audio;
            break;
        }
        case kPong: {
            static int count = 0;
            if (++count % 10 == 0) {
                LOGI("control rtt %.1f ms", static_cast<double>(nowUs() - m.tsUs) / 1000.0);
            }
            break;
        }
        case kBye:
            return false;
        default:
            break;
    }
    return true;
}

void Session::controlLoop() {
    while (running_) {
        int fd = connectSocket(host_, kControlPort, SOCK_STREAM);
        if (fd < 0) {
            LOGW("cannot reach %s:%u, retrying", host_.c_str(), kControlPort);
            sleepWhile(running_, 1000);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        {
            std::lock_guard<std::mutex> lk(sendMutex_);
            controlFd_ = fd;
            controlStopping_ = false;
        }
        LOGI("control connected to %s", host_.c_str());
        sendControl(encodeHello("Quest"));

        ControlReader reader;
        uint8_t buf[2048];
        uint64_t lastPing = nowUs();
        uint64_t lastReceived = lastPing;
        bool alive = true;
        while (running_ && alive) {
            pollfd pfd{fd, POLLIN, 0};
            int r = poll(&pfd, 1, 250);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r > 0) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                lastReceived = nowUs();
                reader.feed(buf, static_cast<size_t>(n));
                Message m;
                int s;
                while ((s = reader.next(m)) == 1) {
                    if (!handleMessage(m)) {
                        alive = false;
                        break;
                    }
                }
                if (s < 0) alive = false;
            }
            const uint64_t now = nowUs();
            if (now - lastReceived > kControlTimeoutUs) {
                LOGW("control connection timed out, reconnecting");
                break;
            }
            if (now - lastPing > 1000000) {
                sendControl(encodePing(now));
                lastPing = now;
            }
        }

        controlReady_ = false;
        {
            std::lock_guard<std::mutex> lk(sendMutex_);
            controlFd_ = -1;
        }
        close(fd);
        if (running_) {
            LOGW("control connection lost, reconnecting");
            sleepWhile(running_, 500);
        }
    }
}

bool Session::startMedia(const StreamConfig& cfg, ANativeWindow* window) {
    if (mediaRunning_) return true;
    const char* mime = cfg.codec == kHevc ? "video/hevc" : (cfg.codec == kAv1 ? "video/av01" : "video/avc");

    udpFd_ = connectSocket(host_, kMediaPort, SOCK_DGRAM);
    if (udpFd_ < 0) {
        LOGE("cannot open UDP socket to %s", host_.c_str());
        return false;
    }
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(udpFd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    if (!decoder_.start(window, mime, cfg.width, cfg.height, cfg.fps ? cfg.fps : 60)) {
        close(udpFd_);
        udpFd_ = -1;
        return false;
    }

    mediaStop_ = false;
    mediaRunning_ = true;
    mediaThread_ = std::thread(&Session::mediaLoop, this);
    return true;
}

void Session::stopMedia() {
    audio_.stop();
    if (!mediaRunning_) return;
    mediaStop_ = true;
    if (mediaThread_.joinable()) mediaThread_.join();
    decoder_.stop();
    if (udpFd_ >= 0) {
        close(udpFd_);
        udpFd_ = -1;
    }
    mediaRunning_ = false;
    if (running_) {
        std::lock_guard<std::mutex> lk(sendMutex_);
        if (controlFd_ >= 0) {
            controlStopping_ = true;
            controlReady_ = false;
            if (::shutdown(controlFd_, SHUT_RDWR) != 0) {
                LOGW("cannot restart control connection after media stops: %d", errno);
            }
        }
    }
}

bool Session::wantsMic() {
    std::lock_guard<std::mutex> lk(cfgMutex_);
    return audioCfg_ && audioCfg_->micChannels > 0;
}

void Session::tick() {
    if (!mediaRunning_) return;
    uint64_t now = nowUs();
    if (now - lastAudioTryUs_ < 1000000) return;  // retry failed starts once a second
    std::optional<AudioConfig> cfg;
    {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        cfg = audioCfg_;
    }
    if (!cfg) return;
    bool failed = false;
    if (cfg->channels > 0 && !audio_.playbackRunning()) {
        failed |= !audio_.startPlayback(static_cast<int>(cfg->sampleRate), cfg->channels);
    }
    if (cfg->micChannels > 0 && micAllowed_ && !audio_.captureRunning()) {
        failed |= !audio_.startCapture(static_cast<int>(cfg->sampleRate),
                                       [this](const int16_t* s, size_t n) { onMicFrame(s, n); });
    }
    if (failed) lastAudioTryUs_ = now;
}

void Session::onMicFrame(const int16_t* samples, size_t count) {
    if (!mediaRunning_ || udpFd_ < 0) return;
    static thread_local std::vector<uint8_t> pkt;  // reused so the audio thread does not allocate
    encodeAudioUp(pkt, micSeq_++, nowUs(), samples, count);
    send(udpFd_, pkt.data(), pkt.size(), 0);
}

void Session::sendTracking(uint64_t tsUs, const float head[7], const TrackingHand& left,
                           const TrackingHand& right) {
    if (!mediaRunning_ || udpFd_ < 0) return;
    std::vector<uint8_t> pkt = encodeTracking(trackSeq_++, tsUs, head, left, right);
    send(udpFd_, pkt.data(), pkt.size(), 0);
}

void Session::sendBodyTracking(uint64_t tsUs, uint16_t activeMask,
                               const float joints[kBodyTrackingJointCount][7]) {
    if (!mediaRunning_ || udpFd_ < 0 || activeMask == 0) return;
    const std::vector<uint8_t> pkt = encodeBodyTracking(bodySeq_++, tsUs, activeMask, joints);
    send(udpFd_, pkt.data(), pkt.size(), 0);
}

void Session::sendViewConfig(const EyeViewConfig eyes[2]) {
    if (!mediaRunning_ || udpFd_ < 0) return;
    const auto pkt = encodeViewConfig(eyes);
    send(udpFd_, pkt.data(), pkt.size(), 0);
}

void Session::mediaLoop() {
    std::vector<uint8_t> buf(2048);
    std::optional<VideoFrame> cur;
    bool waitingForKey = true;  // join the stream at a keyframe
    uint32_t lastGood = 0;
    uint32_t seenEpoch = epoch_;
    uint64_t lastKeyReq = 0;

    auto requestKey = [&]() {
        uint64_t t = nowUs();
        if (t - lastKeyReq < 250000) return;
        lastKeyReq = t;
        sendControl(encodeKeyframeRequest(lastGood));
    };

    while (!mediaStop_) {
        pollfd pfd{udpFd_, POLLIN, 0};
        if (poll(&pfd, 1, 100) <= 0) continue;
        ssize_t n = recv(udpFd_, buf.data(), buf.size(), 0);
        if (n <= 0) continue;

        if (epoch_ != seenEpoch) {  // new control session means the server restarted its stream
            seenEpoch = epoch_;
            cur.reset();
            waitingForKey = true;
        }

        if (buf[0] == kPacketAudioDown) {
            const uint8_t* audioPayload = nullptr;
            size_t audioLen = 0;
            if (parseAudioDown(buf.data(), static_cast<size_t>(n), audioPayload, audioLen)) {
                audio_.pushPlayback(reinterpret_cast<const int16_t*>(audioPayload), audioLen / 2);
            }
            continue;
        }

        VideoHeader h;
        const uint8_t* payload = nullptr;
        if (!parseVideo(buf.data(), static_cast<size_t>(n), h, payload)) continue;
        // Version 1 of the client handles one slice per frame.
        if (h.sliceCount != 1 || h.sliceIdx != 0 || h.packetCount == 0 || h.packetIdx >= h.packetCount) continue;

        bool newer = !cur || static_cast<int32_t>(h.frameId - cur->id) > 0;
        if (newer) {
            if (cur && (!cur->delivered || h.frameId != cur->id + 1u)) {
                if (!waitingForKey) LOGW("video frame lost; waiting for a keyframe");
                waitingForKey = true;
                requestKey();
            }
            cur.emplace(h);
        } else if (h.frameId != cur->id) {
            continue;  // late packet from an older frame
        }

        if (cur->delivered) continue;
        const bool alreadyRejected = cur->isRejected();
        auto result = cur->add(h, payload);
        if (result == VideoFrame::Result::Invalid) {
            if (!alreadyRejected) LOGW("inconsistent video frame packets; waiting for a keyframe");
            waitingForKey = true;
            requestKey();
            continue;
        }
        if (result == VideoFrame::Result::Complete) {
            cur->delivered = true;
            if (waitingForKey && !cur->key) {
                requestKey();  // references are broken, skip until the next keyframe
            } else {
                waitingForKey = false;
                const auto pts = cur->poseTs ? cur->poseTs : nowUs();
                if (!decoder_.submit(cur->assemble(), cur->key, static_cast<int64_t>(pts))) {
                    waitingForKey = true;
                    requestKey();
                } else {
                    lastGood = cur->id;
                }
            }
        }
    }
}

void Session::shutdown() {
    running_ = false;
    controlReady_ = false;
    if (controlThread_.joinable()) controlThread_.join();
    stopMedia();
}

}  // namespace kitvr
