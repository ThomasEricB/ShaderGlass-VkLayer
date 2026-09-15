/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Derived from DLSS5VKLayer (relicensed to GPL-3.0, see RELICENSE.md).

One log sink for the whole layer. One mutex for the sink, not one per line: the shape this was taken
from allocated a fresh std::mutex on every call and leaked it, which at present rates is a leak per
frame.
*/

#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace shaderglass {

inline std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}

inline FILE* LogSink() {
    static FILE* f = [] {
        const char* p = getenv("SHADERGLASS_LOG");
        return p && *p ? fopen(p, "a") : stderr;
    }();
    return f ? f : stderr;
}

inline void Log(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> lk(LogMutex());
    fprintf(LogSink(), "[shaderglass] %s\n", buf);
    fflush(LogSink());
}

inline bool Verbose() {
    static const bool v = [] {
        const char* p = getenv("SHADERGLASS_VERBOSE");
        return p && p[0] == '1';
    }();
    return v;
}

inline bool TimeEnabled() {
    static const bool v = [] {
        const char* p = getenv("SHADERGLASS_TIME");
        return p && p[0] == '1';
    }();
    return v;
}

inline int TimeInterval() {
    static const int v = [] {
        const char* p = getenv("SHADERGLASS_TIME_EVERY");
        return p && *p ? atoi(p) : 60;
    }();
    return v > 0 ? v : 60;
}

inline double NowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace shaderglass
