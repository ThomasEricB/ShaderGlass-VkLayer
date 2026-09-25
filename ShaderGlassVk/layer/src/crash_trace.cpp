/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0
*/

#include "crash_trace.h"

#include "log.h"

#include <execinfo.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <cstdlib>

namespace shaderglass {

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
struct sigaction g_previous[sizeof(kSignals) / sizeof(kSignals[0])];

// Only what is safe from a signal handler: write(2) and backtrace_symbols_fd, no malloc, no stdio.
// The frames are written to the log's own descriptor so they land in the file the user already has.
void Handler(int sig, siginfo_t* info, void* context) {
    const int fd = fileno(LogSink());
    if (fd >= 0) {
        char head[128];
        const int n = snprintf(head, sizeof(head),
                               "\n[shaderglass] *** fatal signal %d in this process; frames below "
                               "***\n",
                               sig);
        if (n > 0) {
            ssize_t ignored = write(fd, head, size_t(n));
            (void) ignored;
        }

        void* frames[64];
        const int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, fd);
        fsync(fd);
    }

    // Put back whatever was there and let it happen: the game, Proton or the runtime may have its
    // own reporting, and swallowing the signal would break it.
    for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i) {
        if (kSignals[i] != sig) continue;
        sigaction(sig, &g_previous[i], nullptr);
        break;
    }
    raise(sig);
}

}  // namespace

void InstallCrashTrace() {
    static bool done = false;
    if (done) return;
    done = true;

    const char* want = getenv("SHADERGLASS_CRASH_TRACE");
    if (!want || want[0] != '1') return;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = Handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i)
        sigaction(kSignals[i], &sa, &g_previous[i]);

    Log("[layer] crash tracing armed (SHADERGLASS_CRASH_TRACE=1)");
}

}  // namespace shaderglass
