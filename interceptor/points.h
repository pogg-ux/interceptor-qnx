/* Current value of any point the Interceptor sees, by name.
 * Digital: PLC inputs/outputs and hardwired taps. Analog: PLC analog
 * inputs/outputs and tapped analog sensors. Return 1 if known. */
#ifndef POINTS_H
#define POINTS_H
int point_digital(const char *name, int *v);
int point_analog(const char *name, double *v);
#endif
