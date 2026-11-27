/*
 * Modbus TCP "copy side" parser.
 * Reads a COPY of the bytes the Interceptor already forwarded and keeps a
 * live table of every input and output. It never touches the real traffic.
 */
#ifndef MODBUS_H
#define MODBUS_H

#include <stdint.h>
#include <stddef.h>

#define MB_TABLE_SIZE 64      /* per table: DI, coils, IR, HR */
#define MB_STREAM_BUF 1024    /* holds one partial frame per direction */
#define MB_PENDING    16      /* outstanding requests remembered */

enum mb_table { MB_DI, MB_COIL, MB_IR, MB_HR, MB_NTABLES };

struct mb_pending {
    uint16_t tid;
    uint8_t  fc;
    uint16_t addr, count;
    int      used;
};

struct mb_stream {
    uint8_t buf[MB_STREAM_BUF];
    size_t  len;
};

/* Called when a digital point (DI or coil) changes, or an HR is written. */
typedef void (*mb_change_cb)(enum mb_table t, int index, int old_v, int new_v);

struct mb_state {
    struct mb_stream to_field, to_plc;
    struct mb_pending pend[MB_PENDING];
    int      value[MB_NTABLES][MB_TABLE_SIZE];
    int      known[MB_NTABLES][MB_TABLE_SIZE];   /* seen at least once */
    unsigned long frames_req, frames_rsp, exceptions, bad_frames;
    mb_change_cb on_change;
};

void mb_init(struct mb_state *s, mb_change_cb cb);
/* Feed a copy of bytes that went PLC -> field (requests). */
void mb_feed_request(struct mb_state *s, const uint8_t *p, size_t n);
/* Feed a copy of bytes that went field -> PLC (responses). */
void mb_feed_response(struct mb_state *s, const uint8_t *p, size_t n);
/* Forget partial frames and pending requests (new connection). */
void mb_reset_streams(struct mb_state *s);

#endif
