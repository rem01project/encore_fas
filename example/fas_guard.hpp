// SPDX-License-Identifier: BSD-2-Clause

#ifndef FAS_GUARD_HPP
#define FAS_GUARD_HPP

#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "fas_sysfs.hpp"

/**
 * Makes sure frequency limits are put back however the process ends.
 *
 * First SIGINT/SIGTERM/SIGHUP: ask the main loop to stop and start a watchdog.
 * Second signal or watchdog expiry: restore from the pre-built table and exit.
 * Fatal signals and exit(): restore, then die the way the signal says.
 * SIGKILL cannot be caught; the recovery file covers it on the next start.
 */
namespace fsched {
namespace guard {

constexpr unsigned kWatchdogSec = 3;

inline volatile sig_atomic_t g_stop = 0;

/** @return True once a stop was requested. */
inline bool stopping() {
    return g_stop != 0;
}

inline void say(const char *msg) {
    const ssize_t r = write(STDERR_FILENO, msg, strlen(msg));
    (void)r;
}

/**
 * Stop request: the first one asks the loop to finish, the second forces the exit.
 */
inline void on_stop(int sig) {
    if (g_stop) {
        say("fas_tail: second signal, restoring frequency limits now\n");
        emergency_restore(true);
        _exit(128 + sig);
    }
    g_stop = 1;
    alarm(kWatchdogSec);
}

/**
 * Watchdog: the loop did not finish in time.
 */
inline void on_alarm(int) {
    say("fas_tail: shutdown timed out, restoring frequency limits and exiting\n");
    emergency_restore(true);
    _exit(124);
}

/**
 * Crash: restore the limits, then let the default action (core dump) happen.
 */
inline void on_fatal(int sig) {
    emergency_restore(true);
    signal(sig, SIG_DFL);
    raise(sig);
}

inline void at_exit() {
    emergency_restore(false);
}

/**
 * Installs every handler. Call before any limit is changed.
 */
inline void install() {
    static char altstack[64 * 1024];
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    sigaltstack(&ss, nullptr);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = on_stop;
    sa.sa_flags = 0; // no SA_RESTART: poll() must return EINTR
    for (int sig : {SIGINT, SIGTERM, SIGHUP})
        sigaction(sig, &sa, nullptr);

    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, nullptr);

    sa.sa_handler = on_fatal;
    sa.sa_flags = SA_RESETHAND | SA_NODEFER | SA_ONSTACK;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        sigaction(sig, &sa, nullptr);

    atexit(at_exit);
}

/** Cancels the watchdog once shutdown finished. */
inline void finished() {
    alarm(0);
}

} // namespace guard
} // namespace fsched

#endif
