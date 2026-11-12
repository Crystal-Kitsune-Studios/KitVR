#pragma once
#include <cstdio>
#define ANDROID_LOG_INFO 1
#define ANDROID_LOG_WARN 2
#define ANDROID_LOG_ERROR 3
#define __android_log_print(level, tag, ...) std::fprintf(stderr, __VA_ARGS__)
