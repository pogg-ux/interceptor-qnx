/*
 * High-resolution monotonic clock for the event timeline.
 * On QNX: ClockCycles() (CPU cycle counter) scaled by the system page's
 * cycles_per_sec. Elsewhere (for testing on Linux): CLOCK_MONOTONIC.
 */
#ifndef HRCLOCK_H
#define HRCLOCK_H
#include <stdint.h>
#include <time.h>

#ifdef __QNX__
#include <sys/neutrino.h>
#include <sys/syspage.h>
static inline uint64_t hr_now_ns(void)
{
    static uint64_t cps;
    if (!cps) cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    uint64_t c = ClockCycles();
    return (c / cps) * 1000000000ULL + (c % cps) * 1000000000ULL / cps;
}
static inline const char *hr_source(void) { return "ClockCycles()"; }
#else
static inline uint64_t hr_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static inline const char *hr_source(void) { return "CLOCK_MONOTONIC"; }
#endif
#endif
