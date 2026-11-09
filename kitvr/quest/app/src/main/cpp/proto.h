// C++ port of crates/proto. Keep in sync with docs/PROTOCOL.md.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <time.h>
#include <vector>

namespace kitvr {

constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kControlPort = 9943;  // TCP
constexpr uint16_t kMediaPort = 9944;    // UDP

constexpr size_t kVideoHeaderLen = 24;
constexpr size_t kTrackingLen = 136;
constexpr uint8_t kPacketVideo = 1;
constexpr uint8_t kPacketTracking = 2;
constexpr uint8_t kPacketAudioDown = 3;  // PC audio to the headset
constexpr uint8_t kPacketAudioUp = 4;    // microphone to the PC
constexpr size_t kAudioHeaderLen = 16;
constexpr uint8_t kFlagKeyframe = 1;
constexpr uint8_t kFlagLastSlice = 2;

enum Tag : uint8_t {
    kHello = 1,
    kStreamConfig = 2,
    kKeyframeRequest = 3,
    kBitrateRequest = 4,
    kPing = 5,
    kPong = 6,
    kBye = 7,
    kAudioConfig = 8,
};

enum Codec : uint8_t { kH264 = 1, kHevc = 2, kAv1 = 3 };

inline uint64_t nowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

// ---- little-endian helpers ----
inline void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xff));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xff));
}
inline void put64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; i++) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xff));
}
inline void putF32(std::vector<uint8_t>& v, float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    put32(v, x);
}
inline uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t get64(const uint8_t* p) {
    return static_cast<uint64_t>(get32(p)) | (static_cast<uint64_t>(get32(p + 4)) << 32);
}

// ---- control channel ----
inline std::vector<uint8_t> frameMessage(uint8_t tag, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    put32(out, static_cast<uint32_t>(body.size() + 1));
    out.push_back(tag);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

inline std::vector<uint8_t> encodeHello(const std::string& name) {
    std::vector<uint8_t> b;
    put16(b, kProtocolVersion);
    size_t n = std::min<size_t>(name.size(), 255);
    b.push_back(static_cast<uint8_t>(n));
    b.insert(b.end(), name.begin(), name.begin() + static_cast<long>(n));
    return frameMessage(kHello, b);
}
inline std::vector<uint8_t> encodePing(uint64_t tsUs) {
    std::vector<uint8_t> b;
    put64(b, tsUs);
    return frameMessage(kPing, b);
}
inline std::vector<uint8_t> encodeKeyframeRequest(uint32_t lastGoodFrame) {
    std::vector<uint8_t> b;
    put32(b, lastGoodFrame);
    return frameMessage(kKeyframeRequest, b);
}
inline std::vector<uint8_t> encodeBye() { return frameMessage(kBye, {}); }

struct StreamConfig {
    uint8_t codec = kHevc;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t fps = 0;
    uint32_t bitrateKbps = 0;
};

struct AudioConfig {
    uint32_t sampleRate = 0;
    uint8_t channels = 0;     // PC to headset, 0 = none
    uint16_t frameSamples = 0;
    uint8_t micChannels = 0;  // headset to PC, 0 = none
};

struct Message {
    uint8_t tag = 0;
    uint16_t version = 0;  // Hello
    std::string name;      // Hello
    StreamConfig cfg;      // StreamConfig
    AudioConfig audio;     // AudioConfig
    uint64_t tsUs = 0;     // Ping, Pong
};

class ControlReader {
public:
    void feed(const uint8_t* data, size_t n) { buf_.insert(buf_.end(), data, data + n); }

    // Returns 1 when a message was parsed, 0 when more bytes are needed, -1 on a protocol error.
    int next(Message& m) {
        if (buf_.size() < 4) return 0;
        uint32_t len = get32(buf_.data());
        if (len == 0 || len > 64 * 1024) return -1;
        if (buf_.size() < 4 + static_cast<size_t>(len)) return 0;

        const uint8_t* p = buf_.data() + 5;
        size_t n = len - 1;
        m = Message{};
        m.tag = buf_[4];
        bool ok = true;

        if (m.tag == kHello) {
            if (n < 3) {
                ok = false;
            } else {
                m.version = get16(p);
                size_t nameLen = p[2];
                if (n < 3 + nameLen) ok = false;
                else m.name.assign(reinterpret_cast<const char*>(p + 3), nameLen);
            }
        } else if (m.tag == kStreamConfig) {
            if (n < 11) {
                ok = false;
            } else {
                m.cfg.codec = p[0];
                m.cfg.width = get16(p + 1);
                m.cfg.height = get16(p + 3);
                m.cfg.fps = get16(p + 5);
                m.cfg.bitrateKbps = get32(p + 7);
            }
        } else if (m.tag == kAudioConfig) {
            if (n < 8) {
                ok = false;
            } else {
                m.audio.sampleRate = get32(p);
                m.audio.channels = p[4];
                m.audio.frameSamples = get16(p + 5);
                m.audio.micChannels = p[7];
            }
        } else if (m.tag == kPing || m.tag == kPong) {
            if (n < 8) ok = false;
            else m.tsUs = get64(p);
        }  // other tags carry nothing we need, unknown tags are skipped

        buf_.erase(buf_.begin(), buf_.begin() + 4 + static_cast<long>(len));
        return ok ? 1 : -1;
    }

private:
    std::vector<uint8_t> buf_;
};

// ---- video packets ----
struct VideoHeader {
    uint8_t flags = 0;
    uint8_t sliceIdx = 0;
    uint8_t sliceCount = 0;
    uint32_t frameId = 0;
    uint64_t poseTimestampUs = 0;
    uint16_t packetIdx = 0;
    uint16_t packetCount = 0;
    uint16_t payloadLen = 0;
};

inline bool parseVideo(const uint8_t* d, size_t n, VideoHeader& h, const uint8_t*& payload) {
    if (n < kVideoHeaderLen || d[0] != kPacketVideo) return false;
    h.flags = d[1];
    h.sliceIdx = d[2];
    h.sliceCount = d[3];
    h.frameId = get32(d + 4);
    h.poseTimestampUs = get64(d + 8);
    h.packetIdx = get16(d + 16);
    h.packetCount = get16(d + 18);
    h.payloadLen = get16(d + 20);
    if (n < kVideoHeaderLen + h.payloadLen) return false;
    payload = d + kVideoHeaderLen;
    return true;
}

// ---- audio packets ----
// Payload is signed 16 bit little-endian PCM, interleaved.
inline void encodeAudioUp(std::vector<uint8_t>& v, uint16_t seq, uint64_t tsUs, const int16_t* pcm, size_t count) {
    v.clear();
    v.push_back(kPacketAudioUp);
    v.push_back(0);
    put16(v, seq);
    put64(v, tsUs);
    put16(v, static_cast<uint16_t>(count * 2));
    put16(v, 0);
    for (size_t i = 0; i < count; i++) put16(v, static_cast<uint16_t>(pcm[i]));
}

inline bool parseAudioDown(const uint8_t* d, size_t n, const uint8_t*& payload, size_t& payloadLen) {
    if (n < kAudioHeaderLen || d[0] != kPacketAudioDown) return false;
    size_t len = get16(d + 12);
    if (n < kAudioHeaderLen + len) return false;
    payload = d + kAudioHeaderLen;
    payloadLen = len;
    return true;
}

// ---- tracking packets ----
// head = {px, py, pz, qx, qy, qz, qw}. Hands are sent as inactive for now.
inline std::vector<uint8_t> encodeTracking(uint16_t seq, uint64_t tsUs, const float head[7]) {
    std::vector<uint8_t> v;
    v.reserve(kTrackingLen);
    v.push_back(kPacketTracking);
    v.push_back(0);
    put16(v, seq);
    put64(v, tsUs);
    for (int i = 0; i < 7; i++) putF32(v, head[i]);
    v.resize(kTrackingLen, 0);
    return v;
}

}  // namespace kitvr
