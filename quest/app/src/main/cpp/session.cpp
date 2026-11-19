#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#ifdef __ANDROID__
#include <sys/random.h>
#endif
#include <unistd.h>

#include <array>
#include <chrono>

#include "control_send.h"
#include "log.h"
#include "video_frame.h"

namespace kitvr {

namespace {

constexpr uint64_t kControlTimeoutUs = 5000000;
#ifndef KITVR_CONNECT_TIMEOUT_MS
#define KITVR_CONNECT_TIMEOUT_MS 5000
#endif
constexpr auto kConnectTimeout = std::chrono::milliseconds(KITVR_CONNECT_TIMEOUT_MS);
#ifdef __ANDROID__
constexpr size_t kSecureHandshakeLen = 68;
#endif

bool connectControl(int fd, const sockaddr* addr, socklen_t len,
                    const std::atomic<bool>& running) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    int result = connect(fd, addr, len);
    if (result < 0 && errno == EINPROGRESS) {
        const auto deadline = std::chrono::steady_clock::now() + kConnectTimeout;
        while (running && std::chrono::steady_clock::now() < deadline) {
            pollfd pfd{fd, POLLOUT, 0};
            result = poll(&pfd, 1, 100);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0) return false;
            if (result == 0) continue;
            int error = 0;
            socklen_t errorLen = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLen) < 0) return false;
            if (error != 0) {
                errno = error;
                return false;
            }
            return fcntl(fd, F_SETFL, flags) == 0;
        }
        errno = running ? ETIMEDOUT : ECANCELED;
        return false;
    }
    if (result < 0) return false;
    return fcntl(fd, F_SETFL, flags) == 0;
}

int connectSocket(const std::string& host, uint16_t port, int socktype,
                  const std::atomic<bool>* running = nullptr) {
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
        if (running ? connectControl(fd, p->ai_addr, p->ai_addrlen, *running)
                    : connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        const int error = errno;
        close(fd);
        fd = -1;
        errno = error;
        if (running && !*running) break;
    }
    freeaddrinfo(res);
    return fd;
}

#ifdef __ANDROID__
bool decodePairingKey(const std::string& hex, std::array<uint8_t, 32>& key) {
    if (hex.size() != 64) return false;
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < key.size(); ++i) {
        const int hi = digit(hex[i * 2]);
        const int lo = digit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        key[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool fillRandom(uint8_t* output, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t count = getrandom(output + written, size - written, 0);
        if (count > 0) {
            written += static_cast<size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool readExactWithTimeout(int fd, uint8_t* data, size_t size) {
    size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + kConnectTimeout;
    while (received < size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            errno = ETIMEDOUT;
            return false;
        }
        pollfd pfd{fd, POLLIN, 0};
        const int result = poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) {
            if (result == 0) errno = ETIMEDOUT;
            return false;
        }
        const ssize_t count = recv(fd, data + received, size - received, 0);
        if (count > 0) {
            received += static_cast<size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            if (count == 0) errno = ECONNRESET;
            return false;
        }
    }
    return true;
}

bool authenticateControl(int fd, JavaVM* vm, jobject activity,
                         const std::array<uint8_t, 32>& pairingKey,
                         SecureChannel& secureChannel) {
    std::array<uint8_t, 32> clientNonce{};
    std::array<uint8_t, 32> clientProof{};
    if (!fillRandom(clientNonce.data(), clientNonce.size()) ||
        !SecureChannel::clientAuthenticator(vm, activity, pairingKey.data(),
                                            clientNonce.data(), clientProof.data())) {
        LOGE("cannot initialize secure control handshake");
        return false;
    }

    std::array<uint8_t, kSecureHandshakeLen> request{};
    std::memcpy(request.data(), "KVH2", 4);
    std::memcpy(request.data() + 4, clientNonce.data(), clientNonce.size());
    std::memcpy(request.data() + 36, clientProof.data(), clientProof.size());
    if (!sendControlPacket(fd, request.data(), request.size())) return false;

    std::array<uint8_t, kSecureHandshakeLen> response{};
    if (!readExactWithTimeout(fd, response.data(), response.size())) return false;
    if (std::memcmp(response.data(), "KVS2", 4) != 0) {
        LOGE("server does not support secure KitVR v2 pairing");
        return false;
    }
    std::array<uint8_t, 32> serverNonce{};
    std::memcpy(serverNonce.data(), response.data() + 4, serverNonce.size());
    if (!SecureChannel::verifyServerAuthenticator(vm, activity, pairingKey.data(),
            clientNonce.data(), serverNonce.data(), response.data() + 36)) {
        LOGE("pairing key rejected by server");
        return false;
    }
    if (!secureChannel.initialize(vm, activity, pairingKey.data(),
                                 clientNonce.data(), serverNonce.data())) {
        LOGE("cannot initialize encrypted KitVR channel");
        return false;
    }
    return true;
}
#endif

void sleepWhile(const std::atomic<bool>& flag, int ms) {
    for (int waited = 0; waited < ms && flag; waited += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace

#ifndef __ANDROID__
void Session::startControl(const std::string& host) {
    if (running_) return;
    host_ = host;
    running_ = true;
    controlThread_ = std::thread(&Session::controlLoop, this);
}
#endif

#ifdef __ANDROID__
void Session::startControl(const std::string& host, const std::string& pairingKeyHex,
                           JavaVM* vm, jobject activity) {
    if (running_) return;
    if (!decodePairingKey(pairingKeyHex, pairingKey_)) {
        LOGE("invalid KitVR pairing key");
        return;
    }
    if (!vm || !activity) {
        LOGE("Android secure channel context is unavailable");
        return;
    }
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGE("cannot attach to Android runtime for secure channel");
        return;
    }
    javaVm_ = vm;
    activity_ = env->NewGlobalRef(activity);
    if (!activity_) {
        javaVm_ = nullptr;
        LOGE("cannot retain Android activity for secure channel");
        return;
    }
    host_ = host;
    secureEnabled_ = true;
    running_ = true;
    controlThread_ = std::thread(&Session::controlLoop, this);
}
#endif

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
    bool sent = false;
#ifdef __ANDROID__
    if (secureEnabled_) {
        std::vector<uint8_t> ciphertext;
        const uint64_t sequence = controlSendSeq_;
        if (secureChannel_.sealControl(sequence, data.data(), data.size(), ciphertext) &&
            ciphertext.size() <= 64 * 1024 + 16) {
            std::vector<uint8_t> record;
            record.reserve(12 + ciphertext.size());
            put32(record, static_cast<uint32_t>(8 + ciphertext.size()));
            put64(record, sequence);
            record.insert(record.end(), ciphertext.begin(), ciphertext.end());
            sent = sendControlPacket(controlFd_, record.data(), record.size());
            if (sent) ++controlSendSeq_;
        }
    } else
#endif
    {
        sent = sendControlPacket(controlFd_, data.data(), data.size());
    }
    if (!sent) {
        LOGW("control send failed: %d; reconnecting", errno);
        controlStopping_ = true;
        controlReady_ = false;
        if (::shutdown(controlFd_, SHUT_RDWR) != 0) {
            LOGW("cannot shut down failed control connection: %d", errno);
        }
    }
}

void Session::sendMediaPacket(const std::vector<uint8_t>& data) {
    if (!mediaRunning_ || udpFd_ < 0) return;
    std::vector<uint8_t> packet;
#ifdef __ANDROID__
    if (secureEnabled_) {
        if (!controlReady_) return;
        uint64_t sequence = mediaSendSeq_.load(std::memory_order_relaxed);
        for (;;) {
            if (sequence == UINT64_MAX) return;
            if (mediaSendSeq_.compare_exchange_weak(sequence, sequence + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed)) break;
        }
        packet = {'K', 'V', 'E', '2'};
        put64(packet, sequence);
        std::vector<uint8_t> ciphertext;
        if (!secureChannel_.sealMedia(sequence, packet.data(), packet.size(),
                                      data.data(), data.size(), ciphertext)) {
            LOGW("could not encrypt media packet");
            return;
        }
        packet.insert(packet.end(), ciphertext.begin(), ciphertext.end());
    } else
#endif
    {
        packet = data;
    }
    if (send(udpFd_, packet.data(), packet.size(), 0) < 0) {
        LOGW("media packet send failed: %d", errno);
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
        int fd = connectSocket(host_, kControlPort, SOCK_STREAM, &running_);
        if (fd < 0) {
            if (running_) LOGW("cannot reach %s:%u, retrying", host_.c_str(), kControlPort);
            sleepWhile(running_, 1000);
            continue;
        }
#ifdef __ANDROID__
        if (secureEnabled_ &&
            !authenticateControl(fd, javaVm_, activity_, pairingKey_, secureChannel_)) {
            close(fd);
            if (running_) sleepWhile(running_, 1000);
            continue;
        }
        mediaSendSeq_ = 0;
#endif
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        {
            std::lock_guard<std::mutex> lk(sendMutex_);
            controlFd_ = fd;
            controlStopping_ = false;
            controlSendSeq_ = 0;
        }
        LOGI("control connected to %s", host_.c_str());
        sendControl(encodeHello("Quest"));

        ControlReader reader;
        std::vector<uint8_t> encryptedBuf;
        uint64_t controlRecvSeq = 0;
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
                bool secureRecordError = false;
#ifdef __ANDROID__
                if (secureEnabled_) {
                    encryptedBuf.insert(encryptedBuf.end(), buf, buf + n);
                    while (encryptedBuf.size() >= 4) {
                        const uint32_t length = get32(encryptedBuf.data());
                        if (length < 24 || length > 64 * 1024 + 24) {
                            secureRecordError = true;
                            break;
                        }
                        if (encryptedBuf.size() < 4 + static_cast<size_t>(length)) break;
                        const uint64_t sequence = get64(encryptedBuf.data() + 4);
                        if (sequence != controlRecvSeq) {
                            secureRecordError = true;
                            break;
                        }
                        std::vector<uint8_t> plaintext;
                        if (!secureChannel_.openControl(sequence, encryptedBuf.data() + 12,
                                                        length - 8, plaintext)) {
                            secureRecordError = true;
                            break;
                        }
                        reader.feed(plaintext.data(), plaintext.size());
                        encryptedBuf.erase(encryptedBuf.begin(),
                                encryptedBuf.begin() + static_cast<long>(4 + length));
                        ++controlRecvSeq;
                    }
                } else
#endif
                {
                    reader.feed(buf, static_cast<size_t>(n));
                }
                if (secureRecordError) {
                    LOGE("invalid encrypted control record; disconnecting");
                    alive = false;
                    break;
                }
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
    sendMediaPacket(pkt);
}

void Session::sendTracking(uint64_t tsUs, const float head[7], const TrackingHand& left,
                           const TrackingHand& right) {
    if (!mediaRunning_ || udpFd_ < 0) return;
    std::vector<uint8_t> pkt = encodeTracking(trackSeq_++, tsUs, head, left, right);
    sendMediaPacket(pkt);
}

void Session::sendBodyTracking(uint64_t tsUs, uint16_t activeMask,
                               const float joints[kBodyTrackingJointCount][7]) {
    if (!mediaRunning_ || udpFd_ < 0 || activeMask == 0) return;
    const std::vector<uint8_t> pkt = encodeBodyTracking(bodySeq_++, tsUs, activeMask, joints);
    sendMediaPacket(pkt);
}

void Session::sendViewConfig(const EyeViewConfig eyes[2]) {
    if (!mediaRunning_ || udpFd_ < 0) return;
    const auto pkt = encodeViewConfig(eyes);
    sendMediaPacket(pkt);
}

void Session::mediaLoop() {
    std::vector<uint8_t> buf(2048);
    std::optional<VideoFrame> cur;
    bool waitingForKey = true;  // join the stream at a keyframe
    uint32_t lastGood = 0;
    uint32_t seenEpoch = epoch_;
    uint64_t lastKeyReq = 0;
    uint64_t newestMediaSequence = 0;
    uint64_t mediaReplayBits = 0;
    bool haveMediaSequence = false;

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
            newestMediaSequence = 0;
            mediaReplayBits = 0;
            haveMediaSequence = false;
        }

        const uint8_t* packetData = buf.data();
        size_t packetSize = static_cast<size_t>(n);
        std::vector<uint8_t> plaintext;
#ifdef __ANDROID__
        if (secureEnabled_) {
            if (packetSize < 28 || std::memcmp(packetData, "KVE2", 4) != 0) continue;
            const uint64_t sequence = get64(packetData + 4);
            if (!secureChannel_.openMedia(sequence, packetData, 12, packetData + 12,
                                          packetSize - 12, plaintext)) continue;
            if (!haveMediaSequence) {
                newestMediaSequence = sequence;
                mediaReplayBits = 1;
                haveMediaSequence = true;
            } else if (sequence > newestMediaSequence) {
                const uint64_t shift = sequence - newestMediaSequence;
                mediaReplayBits = shift >= 64 ? 1 : (mediaReplayBits << shift) | 1;
                newestMediaSequence = sequence;
            } else {
                const uint64_t age = newestMediaSequence - sequence;
                if (age >= 64) continue;
                const uint64_t mask = uint64_t{1} << age;
                if (mediaReplayBits & mask) continue;
                mediaReplayBits |= mask;
            }
            packetData = plaintext.data();
            packetSize = plaintext.size();
        }
#endif
        if (packetSize == 0) continue;

        if (packetData[0] == kPacketAudioDown) {
            const uint8_t* audioPayload = nullptr;
            size_t audioLen = 0;
            if (parseAudioDown(packetData, packetSize, audioPayload, audioLen)) {
                audio_.pushPlayback(reinterpret_cast<const int16_t*>(audioPayload), audioLen / 2);
            }
            continue;
        }

        VideoHeader h;
        const uint8_t* payload = nullptr;
        if (!parseVideo(packetData, packetSize, h, payload)) continue;
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
#ifdef __ANDROID__
    if (activity_ && javaVm_) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (javaVm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED &&
            javaVm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        }
        if (env) env->DeleteGlobalRef(activity_);
        if (attached) javaVm_->DetachCurrentThread();
        activity_ = nullptr;
        javaVm_ = nullptr;
    }
#endif
}

}  // namespace kitvr
