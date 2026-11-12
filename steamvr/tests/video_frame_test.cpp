#include "../../quest/app/src/main/cpp/video_frame.h"

#include <cstdint>
#include <vector>

int main() {
    kitvr::VideoHeader header{};
    header.flags = kitvr::kFlagKeyframe | kitvr::kFlagLastSlice;
    header.sliceCount = 1;
    header.frameId = 42;
    header.poseTimestampUs = 1234;
    header.packetCount = 2;
    header.payloadLen = 1;
    const uint8_t first[] = {7};
    const uint8_t second[] = {9};

    kitvr::VideoFrame frame(header);
    header.packetIdx = 1;
    if (frame.add(header, second) != kitvr::VideoFrame::Result::Incomplete) return 1;
    if (frame.add(header, second) != kitvr::VideoFrame::Result::Incomplete) return 2;
    header.packetIdx = 0;
    if (frame.add(header, first) != kitvr::VideoFrame::Result::Complete ||
        frame.assemble() != std::vector<uint8_t>({7, 9})) return 3;

    kitvr::VideoFrame mismatchedPose(header);
    if (mismatchedPose.add(header, first) != kitvr::VideoFrame::Result::Incomplete) return 4;
    header.packetIdx = 1;
    ++header.poseTimestampUs;
    if (mismatchedPose.add(header, second) != kitvr::VideoFrame::Result::Invalid) return 5;
    --header.poseTimestampUs;
    if (mismatchedPose.add(header, second) != kitvr::VideoFrame::Result::Invalid) return 6;

    kitvr::VideoFrame mismatchedKey(header);
    header.flags &= ~kitvr::kFlagKeyframe;
    if (mismatchedKey.add(header, second) != kitvr::VideoFrame::Result::Invalid) return 7;

    header.flags |= kitvr::kFlagKeyframe;
    kitvr::VideoFrame conflictingDuplicate(header);
    if (conflictingDuplicate.add(header, second) != kitvr::VideoFrame::Result::Incomplete) return 8;
    if (conflictingDuplicate.add(header, first) != kitvr::VideoFrame::Result::Invalid) return 9;
    kitvr::VideoFrame mismatchedCount(header);
    ++header.packetCount;
    if (mismatchedCount.add(header, second) != kitvr::VideoFrame::Result::Invalid) return 10;
    return 0;
}
