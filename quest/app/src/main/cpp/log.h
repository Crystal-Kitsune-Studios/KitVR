#pragma once
#include <android/log.h>

#define KITVR_TAG "kitvr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, KITVR_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, KITVR_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, KITVR_TAG, __VA_ARGS__)
