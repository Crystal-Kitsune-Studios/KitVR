#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>

namespace {
std::atomic<int> sendFailurePhase{1};
}

extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* data, size_t size, int flags) {
    if ((flags & MSG_DONTWAIT) && sendFailurePhase == 1) {
        sendFailurePhase = 2;
        return __real_send(fd, data, std::min(size, size_t{1}), flags);
    }
    if ((flags & MSG_DONTWAIT) && sendFailurePhase == 2) {
        sendFailurePhase = 0;
        errno = EAGAIN;
        return -1;
    }
    return __real_send(fd, data, size, flags);
}

namespace kitvr {
bool VideoDecoder::start(ANativeWindow*, const char*, int, int, int) {
    running_ = true;
    return true;
}
bool VideoDecoder::submit(std::vector<uint8_t>&&, bool, int64_t) { return true; }
void VideoDecoder::stop() { running_ = false; }
bool Audio::startPlayback(int, int) { return false; }
bool Audio::startCapture(int, std::function<void(const int16_t*, size_t)>) { return false; }
void Audio::pushPlayback(const int16_t*, size_t) {}
void Audio::stop() {}
}

namespace {
int makeSocket(int type, uint16_t port) {
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

bool ready(int fd, int timeoutMs) {
    pollfd p{fd, POLLIN, 0};
    return poll(&p, 1, timeoutMs) > 0;
}

bool closed(int fd) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    char buf[2048];
    while (std::chrono::steady_clock::now() < deadline) {
        if (!ready(fd, 100)) continue;
        const ssize_t count = recv(fd, buf, sizeof(buf), 0);
        if (count == 0) return true;
        if (count < 0 && errno != EINTR) return false;
    }
    return false;
}

bool waitForReady(kitvr::Session& session, bool expected, int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (session.controlReady() == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool waitForConfig(kitvr::Session& session, kitvr::StreamConfig& config, uint32_t& epoch) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (session.takeConfig(config, epoch)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::vector<uint8_t> streamConfig(uint16_t width) {
    std::vector<uint8_t> body{kitvr::kHevc};
    kitvr::put16(body, width);
    kitvr::put16(body, 1080);
    kitvr::put16(body, 60);
    kitvr::put32(body, 10000);
    return kitvr::frameMessage(kitvr::kStreamConfig, body);
}

bool answerPings(int fd, kitvr::Session& session) {
    kitvr::ControlReader reader;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5500);
    uint8_t bytes[2048];
    while (std::chrono::steady_clock::now() < deadline) {
        if (!session.controlReady()) return false;
        if (!ready(fd, 100)) continue;
        const ssize_t count = recv(fd, bytes, sizeof(bytes), 0);
        if (count <= 0) return false;
        reader.feed(bytes, static_cast<size_t>(count));
        kitvr::Message message;
        int result = 0;
        while ((result = reader.next(message)) == 1) {
            if (message.tag == kitvr::kPing) {
                std::vector<uint8_t> body;
                kitvr::put64(body, message.tsUs);
                const auto pong = kitvr::frameMessage(kitvr::kPong, body);
                if (send(fd, pong.data(), pong.size(), MSG_NOSIGNAL) !=
                    static_cast<ssize_t>(pong.size())) return false;
            }
        }
        if (result < 0) return false;
    }
    return session.controlReady();
}
}

int main() {
    int listener = makeSocket(SOCK_STREAM, kitvr::kControlPort);
    if (listener < 0) {
        if (errno == EADDRINUSE) return 77;
        std::perror("bind control");
        return 1;
    }
    int udp = makeSocket(SOCK_DGRAM, kitvr::kMediaPort);
    if (udp < 0) {
        const int error = errno;
        close(listener);
        if (error == EADDRINUSE) return 77;
        errno = error;
        std::perror("bind media");
        return 1;
    }
    if (listen(listener, 4) < 0) return 1;
    kitvr::Session session;
    session.startControl("127.0.0.1");
    if (!ready(listener, 3000)) return 2;
    int first = accept(listener, nullptr, nullptr);
    if (first < 0 || !closed(first) || sendFailurePhase != 0 || session.controlReady()) return 35;
    close(first);
    if (!ready(listener, 3000)) return 36;
    first = accept(listener, nullptr, nullptr);
    char buf[2048];
    if (first < 0 || !ready(first, 2000) || recv(first, buf, sizeof(buf), 0) <= 0) return 3;
    if (session.controlReady()) return 13;
    auto hello = kitvr::encodeHello("server");
    if (send(first, hello.data(), hello.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(hello.size())) return 4;
    if (!waitForReady(session, true)) return 14;
    auto config = streamConfig(1920);
    if (send(first, config.data(), config.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(config.size())) return 25;
    kitvr::StreamConfig cfg{};
    uint32_t configEpoch = 0;
    if (!waitForConfig(session, cfg, configEpoch) || cfg.width != 1920 ||
        configEpoch != session.controlEpoch()) return 26;
    auto stale = streamConfig(1280);
    stale.insert(stale.end(), hello.begin(), hello.end());
    if (send(first, stale.data(), stale.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(stale.size())) return 27;
    const auto epochDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (session.controlEpoch() == configEpoch && std::chrono::steady_clock::now() < epochDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (session.controlEpoch() == configEpoch || session.takeConfig(cfg, configEpoch)) return 28;
    config = streamConfig(1920);
    if (send(first, config.data(), config.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(config.size()) ||
        !waitForConfig(session, cfg, configEpoch) || configEpoch != session.controlEpoch()) return 29;
    if (!session.startMedia(cfg, nullptr)) return 5;
    const float head[7] = {0, 0, 0, 0, 0, 0, 1};
    session.sendTracking(1, head, {}, {});
    if (!ready(udp, 2000) || recv(udp, buf, sizeof(buf), 0) <= 0) return 6;
    if (shutdown(first, SHUT_RDWR) != 0 || !waitForReady(session, false)) return 15;
    close(first);
    if (!ready(listener, 3000)) return 16;
    first = accept(listener, nullptr, nullptr);
    if (first < 0 || !ready(first, 2000) || recv(first, buf, sizeof(buf), 0) <= 0) return 17;
    if (session.controlReady()) return 18;
    if (send(first, hello.data(), hello.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(hello.size()) ||
        !waitForReady(session, true)) return 19;
    if (session.controlEpoch() == configEpoch || session.takeConfig(cfg, configEpoch)) return 30;
    session.stopMedia();
    if (session.controlReady()) return 20;
    if (!closed(first)) return 7;
    close(first);
    if (!ready(listener, 3000)) return 8;
    int second = accept(listener, nullptr, nullptr);
    if (second < 0 || !ready(second, 2000) || recv(second, buf, sizeof(buf), 0) <= 0) return 9;
    if (send(second, hello.data(), hello.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(hello.size())) return 10;
    if (!waitForReady(session, true)) return 21;
    config = streamConfig(1920);
    if (send(second, config.data(), config.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(config.size()) ||
        !waitForConfig(session, cfg, configEpoch) || configEpoch != session.controlEpoch()) return 31;
    if (!session.startMedia(cfg, nullptr)) return 11;
    session.sendTracking(2, head, {}, {});
    if (!ready(udp, 2000) || recv(udp, buf, sizeof(buf), 0) <= 0) return 12;
    if (!answerPings(second, session)) return 24;
    if (!waitForReady(session, false, 7000)) return 23;
    close(second);
    if (!ready(listener, 3000)) return 32;
    int third = accept(listener, nullptr, nullptr);
    if (third < 0 || !ready(third, 2000) || recv(third, buf, sizeof(buf), 0) <= 0) return 33;
    const uint32_t previousEpoch = session.controlEpoch();
    auto messages = hello;
    const auto bye = kitvr::frameMessage(kitvr::kBye, {});
    messages.insert(messages.end(), bye.begin(), bye.end());
    messages.insert(messages.end(), hello.begin(), hello.end());
    if (send(third, messages.data(), messages.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(messages.size()) ||
        !closed(third) || session.controlEpoch() != previousEpoch + 1 || session.controlReady()) return 34;
    session.shutdown();
    if (session.controlReady()) return 22;
    close(third);
    close(listener);
    close(udp);
    return 0;
}
