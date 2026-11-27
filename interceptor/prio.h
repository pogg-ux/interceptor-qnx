/*
 * Real-time thread priorities (QNX). The passthrough must never wait on
 * anything else, so it runs highest; everything that only reads runs below.
 *   28  passthrough + Modbus copy + taps + rules  (main thread)
 *   16  resource manager  /dev/interceptor/...
 *   12  black box writer
 *    8  web dashboard
 * On other systems (testing on Linux) this does nothing.
 */
#ifndef PRIO_H
#define PRIO_H
#define PRIO_PASSTHROUGH 28
#define PRIO_RESMGR      16
#define PRIO_BLACKBOX    12
#define PRIO_WEB          8

#ifdef __QNX__
#include <pthread.h>
#include <sched.h>
/* Returns 0 on success. */
static inline int prio_set(int prio)
{
    struct sched_param sp;
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
}
#else
static inline int prio_set(int prio) { (void)prio; return 0; }
#endif
#endif
