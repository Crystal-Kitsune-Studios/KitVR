#pragma once
#include <cstdint>
struct AMediaFormat {};
constexpr const char* AMEDIAFORMAT_KEY_MIME = "mime";
constexpr const char* AMEDIAFORMAT_KEY_WIDTH = "width";
constexpr const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
AMediaFormat* AMediaFormat_new();
void AMediaFormat_delete(AMediaFormat*);
void AMediaFormat_setString(AMediaFormat*, const char*, const char*);
void AMediaFormat_setInt32(AMediaFormat*, const char*, int32_t);
