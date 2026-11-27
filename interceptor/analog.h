/*
 * Analog / count rules (F5-F7, plus F10, F16, F18). Each metric is one
 * number per machine cycle or stroke, compared with its baseline:
 *
 *   press_peak_psi   peak pressure while the press holds      (low = pump wear)
 *   press_rise_ms    time to 90% of normal peak pressure      (high = pump wear)
 *   pump_amps        pump current during the press stroke     (high = pump wear)
 *   vibration        pump vibration during the stroke         (high = pump wear)
 *   coil_ma          valve coil current during the stroke     (low = weak coil/amp)
 *   stroke_oil_l     oil used per press stroke                (high = seal bypass)
 *   index_counts     encoder counts per index                 (off = miscount)
 *   heater_duty      heater duty needed to hold temperature   (high = weak heater)
 *   filter_dp_psi    hydraulic filter pressure drop           (high = clogging)
 *
 * Alert: outside the band in the bad direction 3 times in a row, or the
 * 10-sample average drifts past the trend limit.
 */
#ifndef ANALOG_H
#define ANALOG_H
#include <stddef.h>
#include <stdio.h>
#include "events.h"

void analog_init(void);
void analog_on_event(const struct event *e);
/* Called after every tapped-analog frame (every 20 ms). */
void analog_on_frame(void);
void analog_set_teaching(int on);
/* End of teaching: freeze baselines from what teaching measured. */
int analog_finish_teach(void);
void analog_reset_baselines(void);
size_t analog_format(char *out, size_t max);
/* Save/load baselines in the taught map. */
void analog_save(FILE *f);
void analog_load_line(const char *line);

#endif
