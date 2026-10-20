#include "cycle_stats.hpp"
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#else
#include <cerrno>
#include <ctime>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif

namespace icl {
#ifdef _WIN32
static bool timer_period_set = false;
#endif

RtOutcome apply_realtime_tuning() {
    RtOutcome r;
#ifdef _WIN32
    timer_period_set = timeBeginPeriod(1) == TIMERR_NOERROR;
    const bool prio = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
    r.elevated = timer_period_set && prio;
    r.detail = r.elevated ? "windows: 1 ms timer + TIME_CRITICAL thread" : "windows: partial tuning";
#else
    mlockall(MCL_CURRENT | MCL_FUTURE);  // avoid page faults in the loop when permitted
    sched_param sp{};
    sp.sched_priority = 80;
    r.elevated = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    r.detail = r.elevated ? "linux: SCHED_FIFO prio 80 + mlockall" : "linux: SCHED_FIFO denied (needs CAP_SYS_NICE), SCHED_OTHER";
#endif
    return r;
}

void release_realtime_tuning() {
#ifdef _WIN32
    if (timer_period_set) timeEndPeriod(1);
    timer_period_set = false;
#endif
}

void sleep_until_precise(std::chrono::steady_clock::time_point deadline) {
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) return;
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
            return;
    }
    std::this_thread::sleep_until(deadline);
#else
    // libstdc++ steady_clock is CLOCK_MONOTONIC, so its epoch matches clock_nanosleep's.
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
    timespec ts{static_cast<time_t>(ns / 1000000000LL), static_cast<long>(ns % 1000000000LL)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
#endif
}
}
