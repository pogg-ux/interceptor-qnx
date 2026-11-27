/*
 * Passive tap receiver. The taps are the signals the PLC can't see:
 * relay coils and contacts in the hardwired logic, both safety channels,
 * plus owner-added analog sensors. They arrive as UDP JSON lines:
 *   digital, on change:   {"t": 12.345678, "tap": "T_CR2_COIL", "v": 1}
 *   analog, every 20 ms:  {"t": 12.34, "a": {"AMP_CMD_V": 7.5, ...}}
 * On real hardware these would be opto-isolated inputs; the logic is the same.
 */
#ifndef TAPS_H
#define TAPS_H

#define TAP_MAX     48
#define TAP_ANA_MAX 32

struct tap_state {
    int  n;                          /* digital taps seen so far */
    char name[TAP_MAX][24];
    int  value[TAP_MAX];
    int  na;                         /* analog channels seen so far */
    char aname[TAP_ANA_MAX][24];
    double avalue[TAP_ANA_MAX];
    unsigned long packets, digital_msgs, analog_frames, bad;
};

void taps_init(struct tap_state *t);
/* Parse one UDP packet (one or more JSON lines). Digital changes -> ev_push(). */
void taps_feed(struct tap_state *t, const char *buf, int len);

#endif
