#pragma once

#include "proto.h"

namespace kitvr {

class VideoFrame {
public:
    enum class Result { Incomplete, Complete, Invalid };

    explicit VideoFrame(const VideoHeader& header)
        : id(header.frameId), count(header.packetCount),
          key((header.flags & kFlagKeyframe) != 0), poseTs(header.poseTimestampUs),
          parts(count), have(count, false) {}

    Result add(const VideoHeader& header, const uint8_t* payload) {
        if (rejected) return Result::Invalid;
        if (header.frameId != id || header.packetCount != count ||
            header.packetIdx >= count || header.sliceCount != 1 || header.sliceIdx != 0 ||
            ((header.flags & kFlagKeyframe) != 0) != key || header.poseTimestampUs != poseTs) {
            rejected = true;
            return Result::Invalid;
        }
        if (have[header.packetIdx]) {
            const auto& part = parts[header.packetIdx];
            if (part.size() != header.payloadLen ||
                !std::equal(part.begin(), part.end(), payload)) {
                rejected = true;
                return Result::Invalid;
            }
        } else {
            parts[header.packetIdx].assign(payload, payload + header.payloadLen);
            have[header.packetIdx] = true;
            ++got;
        }
        return got == count ? Result::Complete : Result::Incomplete;
    }

    std::vector<uint8_t> assemble() const {
        std::vector<uint8_t> result;
        size_t size = 0;
        for (const auto& part : parts) size += part.size();
        result.reserve(size);
        for (const auto& part : parts) result.insert(result.end(), part.begin(), part.end());
        return result;
    }

    bool isRejected() const { return rejected; }

    uint32_t id;
    uint16_t count;
    bool key;
    uint64_t poseTs;
    bool delivered = false;

private:
    uint16_t got = 0;
    bool rejected = false;
    std::vector<std::vector<uint8_t>> parts;
    std::vector<bool> have;
};

}  // namespace kitvr
