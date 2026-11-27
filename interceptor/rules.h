/*
 * Drift and fault rules. Plain, explainable checks; no machine learning.
 * The tech can see exactly why each alert fired.
 *
 *   F1-type  TIMING WARN   a learned motion is slower than baseline
 *                          mean + 3 x spread, 3 times in a row
 *            TIMING TREND  20-sample average is 25% over baseline
 *   F2-type  MISMATCH      a relay coil and its contact disagree > 100 ms
 *   F3-type  DISCREPANCY   two safety channels disagree > 100 ms
 *   F4-type  SAFETY SLOW   safety contacts open > 3 x baseline after a trip
 *   F8-type  BROKEN CHAIN  output switched, a result that always followed
 *                          never came (shows which link broke)
 *
 * Baselines: from Teach Mode when available; otherwise frozen from the
 * first 10 samples of normal running.
 */
#ifndef RULES_H
#define RULES_H
#include <stddef.h>
#include <stdint.h>
#include "events.h"

#ifndef RULE_BASELINE_N
#define RULE_BASELINE_N   10
#endif
#define RULE_WARN_SIGMA   3.0
#define RULE_WARN_RUN     3
#ifndef RULE_TREND_N
#define RULE_TREND_N      20
#endif
#define RULE_TREND_PCT    0.25
#define RULE_PAIR_MS      100.0
#define RULE_SAFETY_X     3.0

void rules_init(void);
/* Feed every event (taps for mismatch/discrepancy/safety timing). */
void rules_on_event(const struct event *e);
/* Check timers and pull new mapper samples (call often). */
void rules_tick(uint64_t now_ns);
/* Alerts so far, newest last, plus baselines in use. */
size_t rules_format(char *out, size_t max);
unsigned long rules_alert_count(void);
/* Teach Mode: set/replace a motion's baseline explicitly. */
void rules_set_baseline(const char *out, int out_v, const char *res, int res_v,
                        double mean_ms, double sd_ms, unsigned long n);
/* Teach Mode: healthy safety response time (ms), for one trip source or all. */
void rules_set_safety_baseline_for(const char *trip, double ms);
void rules_set_safety_baseline(double ms);
/* Safety baselines in use: fills names/ms, returns count. */
int rules_safety_baselines(const char **names, double *ms, int max);
/* Forget learned baselines (start of Teach Mode). */
void rules_reset_baselines(void);
/* While teaching: learn, but don't raise timing/safety alerts. */
void rules_set_teaching(int on);
/* End of teaching: freeze safety baselines; returns the slowest (ms, 0 if none). */
double rules_finish_teach(void);

#endif
