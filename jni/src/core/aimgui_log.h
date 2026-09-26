#pragma once

// AImGui unified logging.
//
// All diagnostic output goes to logcat under one tag so a full
// launch → crash sequence can be captured with:
//
//     adb logcat -s AImGui:V
//
// Every line carries the enclosing function name so the last log line
// before a tombstone can be correlated with the crashing frame.
//
// AIMGUI_LOG_LEVEL: 0=silent 1=error 2=warn 3=info (default) 4=debug
// Set to 0 for a shipped binary; leave at 3 while investigating crashes.
//
// This header deliberately has no .cpp companion so it can be dropped
// into every translation unit without touching CMakeLists.txt.
#ifndef AIMGUI_LOG_TAG
#define AIMGUI_LOG_TAG "AImGui"
#endif

#ifndef AIMGUI_LOG_LEVEL
#define AIMGUI_LOG_LEVEL 3
#endif

#include <android/log.h>

#define AILOGD(fmt, ...)                                                      \
    do { if (AIMGUI_LOG_LEVEL >= 4)                                           \
        __android_log_print(ANDROID_LOG_DEBUG, AIMGUI_LOG_TAG,                \
                            "[%s] " fmt, __func__, ##__VA_ARGS__); } while (0)

#define AILOGI(fmt, ...)                                                      \
    do { if (AIMGUI_LOG_LEVEL >= 3)                                           \
        __android_log_print(ANDROID_LOG_INFO, AIMGUI_LOG_TAG,                 \
                            "[%s] " fmt, __func__, ##__VA_ARGS__); } while (0)

#define AILOGW(fmt, ...)                                                      \
    do { if (AIMGUI_LOG_LEVEL >= 2)                                           \
        __android_log_print(ANDROID_LOG_WARN, AIMGUI_LOG_TAG,                 \
                            "[%s] " fmt, __func__, ##__VA_ARGS__); } while (0)

#define AILOGE(fmt, ...)                                                      \
    do { if (AIMGUI_LOG_LEVEL >= 1)                                           \
        __android_log_print(ANDROID_LOG_ERROR, AIMGUI_LOG_TAG,                \
                            "[%s] " fmt, __func__, ##__VA_ARGS__); } while (0)
