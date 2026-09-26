#pragma once

// In-process crash detector for AImGui.
//
// Installs handlers for the common fatal signals and writes a plain-text
// crash report (signal, fault address, register dump, raw backtrace PC
// list) to a file on local storage before re-raising so the OS still
// emits a normal tombstone.
//
// Usage:
//     #include "core/crash_handler.h"
//
//     int main() {
//         aimgui::crash::Install();                        // auto path
//         // or: aimgui::crash::Install("/data/local/tmp/foo.log");
//         ...
//     }
//
// The handler only calls async-signal-safe primitives (open/write/fsync/
// signal/raise). No malloc, no stdio, no C++ exceptions, no locks — so
// it works even when the crashing thread held a heap or runtime lock.
//
// Header-only by design: dropping this file into jni/src/core/ is
// enough; CMakeLists.txt stays untouched.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ucontext.h>
#include <execinfo.h>

namespace aimgui::crash {

namespace detail {

// Process-wide state that the signal handler reads. Static storage so
// its lifetime outlives any call and it is safe to touch from a signal
// context.
struct State {
    char path[512] = {0};
    int  fd        = -1;
    bool installed = false;
};

inline State& S() {
    static State s;
    return s;
}

// ── async-signal-safe output primitives ────────────────────────────────
// Every write goes straight to fd via write(2). No buffering, no format
// strings, no allocation.

inline void W(const char* s, size_t n) {
    int fd = S().fd;
    if (fd < 0) return;
    while (n > 0) {
        ssize_t r = ::write(fd, s, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += r;
        n -= (size_t)r;
    }
}

inline void WS(const char* s) { W(s, std::strlen(s)); }

inline void WHex(uintptr_t v) {
    static const char* d = "0123456789abcdef";
    char b[19];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; ++i)
        b[2 + i] = d[(v >> ((15 - i) * 4)) & 0xF];
    b[18] = '\0';
    WS(b);
}

inline void WDec(long long v) {
    char b[24];
    int  n = 0;
    if (v == 0) {
        b[n++] = '0';
    } else {
        bool neg = v < 0;
        unsigned long long u = neg ? (unsigned long long)(-v)
                                   : (unsigned long long)v;
        while (u) { b[n++] = char('0' + (u % 10)); u /= 10; }
        if (neg) b[n++] = '-';
        for (int i = 0; i < n / 2; ++i) {
            char t = b[i]; b[i] = b[n - 1 - i]; b[n - 1 - i] = t;
        }
    }
    b[n] = '\0';
    WS(b);
}

// ── path resolution (non-signal path, used during Install) ────────────
inline int OpenReport(char* out_path, size_t out_cap) {
    // 1) Directory of the running executable.
    char exe[512] = {0};
    ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k > 0) {
        exe[k] = '\0';
        char* slash = std::strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            std::snprintf(out_path, out_cap, "%s/AImGui_crash.log", exe);
            int fd = ::open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0) return fd;
        }
    }

    // 2) /data/local/tmp — usual home for a root-run native ELF.
    std::snprintf(out_path, out_cap, "/data/local/tmp/AImGui_crash.log");
    int fd = ::open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) return fd;

    // 3) /sdcard — visible from a file manager, no root needed to read.
    std::snprintf(out_path, out_cap, "/sdcard/AImGui_crash.log");
    fd = ::open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) return fd;

    out_path[0] = '\0';
    return -1;
}

// ── signal handler ────────────────────────────────────────────────────
inline const char* SigName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGTRAP: return "SIGTRAP";
        default:      return "SIG?";
    }
}

inline void Handler(int sig, siginfo_t* info, void* uctx) {
    State& s = S();
    if (s.fd < 0) {
        // No file — still re-raise so the OS produces a tombstone.
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    WS("\n======================= AImGui CRASH =======================\n");
    WS("signal: ");   WS(SigName(sig)); WS(" ("); WDec(sig); WS(")\n");
    WS("pid: ");      WDec((long long)getpid());
    WS("  tid: ");    WDec((long long)gettid());
    WS("\n");
    if (info) {
        WS("fault addr: "); WHex((uintptr_t)info->si_addr); WS("\n");
        WS("si_code: ");    WDec((long long)info->si_code); WS("\n");
    }

    if (uctx) {
        const ucontext_t* uc = static_cast<const ucontext_t*>(uctx);
        const mcontext_t& mc = uc->uc_mcontext;
        WS("pc:  "); WHex((uintptr_t)mc.pc);        WS("\n");
        WS("sp:  "); WHex((uintptr_t)mc.sp);        WS("\n");
        WS("lr:  "); WHex((uintptr_t)mc.regs[30]);  WS("\n");
        for (int i = 0; i < 30; ++i) {
            WS("x"); WDec(i); WS(":  "); WHex((uintptr_t)mc.regs[i]); WS("\n");
        }
    }

    WS("\nbacktrace (raw PCs):\n");
    void* frames[64];
    int   n = backtrace(frames, 64);
    for (int i = 0; i < n; ++i) {
        WS("  #"); WDec(i); WS(" "); WHex((uintptr_t)frames[i]); WS("\n");
    }

    WS("=========================== END ===========================\n");
    fsync(s.fd);

    // Hand off to the OS: reset to default action and re-raise. This
    // lets debuggerd write a normal tombstone into /data/tombstones.
    signal(sig, SIG_DFL);
    raise(sig);
}

} // namespace detail

// ── public API ────────────────────────────────────────────────────────

// Install the crash handlers. If log_path is null (default) the report
// goes to the first writable candidate among:
//     <executable dir>/AImGui_crash.log
//     /data/local/tmp/AImGui_crash.log
//     /sdcard/AImGui_crash.log
// Safe to call more than once; the previous handler is replaced.
inline void Install(const char* log_path = nullptr) {
    detail::State& s = detail::S();

    if (s.fd >= 0) {
        ::close(s.fd);
        s.fd = -1;
    }

    if (log_path && log_path[0]) {
        std::strncpy(s.path, log_path, sizeof(s.path) - 1);
        s.path[sizeof(s.path) - 1] = '\0';
        s.fd = ::open(s.path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    } else {
        s.fd = detail::OpenReport(s.path, sizeof(s.path));
    }

    if (s.fd < 0) {
        // Last resort so the handler code path stays uniform.
        s.fd = ::open("/dev/null", O_WRONLY);
    }

    // Dedicated signal stack so a stack-overflow SIGSEGV still fires.
    static char alt_stack[64 * 1024];
    stack_t ss{};
    ss.ss_sp    = alt_stack;
    ss.ss_size  = sizeof(alt_stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = detail::Handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);

    s.installed = true;

    // Write a one-line header so the file's very existence tells us the
    // handler was armed, even if no crash has happened yet.
    char line[640];
    int  k = std::snprintf(line, sizeof(line),
                           "[crash] handler installed; report path = %s\n",
                           s.path);
    if (k > 0) (void)!::write(s.fd, line, (size_t)k);
    fsync(s.fd);
}

// Path of the currently configured crash-log file, or "" if Install()
// hasn't run. Handy for logging on startup.
inline const char* ReportPath() {
    return detail::S().path;
}

} // namespace aimgui::crash
