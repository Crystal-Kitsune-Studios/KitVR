#pragma once
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

struct AMediaCodec {};
struct AMediaFormat;
struct ANativeWindow;
using media_status_t = int32_t;
constexpr media_status_t AMEDIA_OK = 0;
constexpr ssize_t AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2;
constexpr ssize_t AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3;
struct AMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};
AMediaCodec* AMediaCodec_createDecoderByType(const char*);
media_status_t AMediaCodec_configure(AMediaCodec*, const AMediaFormat*, ANativeWindow*, void*, uint32_t);
media_status_t AMediaCodec_start(AMediaCodec*);
media_status_t AMediaCodec_stop(AMediaCodec*);
media_status_t AMediaCodec_delete(AMediaCodec*);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t);
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t, size_t*);
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec*, size_t, off_t, size_t, uint64_t, uint32_t);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec*, AMediaCodecBufferInfo*, int64_t);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t, bool);
