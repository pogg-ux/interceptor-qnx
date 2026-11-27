#include "modbus.h"
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

void mb_init(struct mb_state *s, mb_change_cb cb)
{
    memset(s, 0, sizeof *s);
    s->on_change = cb;
}

void mb_reset_streams(struct mb_state *s)
{
    s->to_field.len = s->to_plc.len = 0;
    memset(s->pend, 0, sizeof s->pend);
}

static void set_point(struct mb_state *s, enum mb_table t, int i, int v)
{
    if (i < 0 || i >= MB_TABLE_SIZE) return;
    int old = s->value[t][i];
    int was_known = s->known[t][i];
    s->value[t][i] = v;
    s->known[t][i] = 1;
    /* Report digital changes and HR writes; analog inputs are too noisy. */
    if (s->on_change && t != MB_IR && (!was_known || old != v))
        s->on_change(t, i, was_known ? old : -1, v);
}

static void set_bits(struct mb_state *s, enum mb_table t, uint16_t addr,
                     uint16_t count, const uint8_t *data, size_t nbytes)
{
    for (uint16_t k = 0; k < count; k++) {
        if ((size_t)(k / 8) >= nbytes) break;
        set_point(s, t, addr + k, (data[k / 8] >> (k % 8)) & 1);
    }
}

static void set_regs(struct mb_state *s, enum mb_table t, uint16_t addr,
                     uint16_t count, const uint8_t *data, size_t nbytes)
{
    for (uint16_t k = 0; k < count; k++) {
        if ((size_t)(2 * k + 1) >= nbytes) break;
        set_point(s, t, addr + k, be16(data + 2 * k));
    }
}

static void remember(struct mb_state *s, uint16_t tid, uint8_t fc, uint16_t addr, uint16_t count)
{
    int slot = 0;
    for (int i = 0; i < MB_PENDING; i++) {
        if (!s->pend[i].used || s->pend[i].tid == tid) { slot = i; break; }
        slot = (int)(tid % MB_PENDING);   /* all busy: overwrite one */
    }
    s->pend[slot] = (struct mb_pending){ tid, fc, addr, count, 1 };
}

static struct mb_pending *lookup(struct mb_state *s, uint16_t tid)
{
    for (int i = 0; i < MB_PENDING; i++)
        if (s->pend[i].used && s->pend[i].tid == tid) return &s->pend[i];
    return NULL;
}

/* One complete request frame (MBAP header + PDU). */
static void on_request(struct mb_state *s, const uint8_t *f, size_t len)
{
    s->frames_req++;
    if (len < 8) { s->bad_frames++; return; }
    uint16_t tid = be16(f);
    const uint8_t *pdu = f + 7;
    size_t plen = len - 7;
    uint8_t fc = pdu[0];
    if (plen < 5) { s->bad_frames++; return; }
    uint16_t addr = be16(pdu + 1), val = be16(pdu + 3);

    switch (fc) {
    case 1: case 2: case 3: case 4:
        remember(s, tid, fc, addr, val);   /* val = count */
        break;
    case 5:   /* write single coil: the PLC's output command */
        set_point(s, MB_COIL, addr, val == 0xFF00);
        remember(s, tid, fc, addr, 1);
        break;
    case 6:   /* write single register */
        set_point(s, MB_HR, addr, val);
        remember(s, tid, fc, addr, 1);
        break;
    case 15:
        if (plen >= 6) set_bits(s, MB_COIL, addr, val, pdu + 6, plen - 6);
        remember(s, tid, fc, addr, val);
        break;
    case 16:
        if (plen >= 6) set_regs(s, MB_HR, addr, val, pdu + 6, plen - 6);
        remember(s, tid, fc, addr, val);
        break;
    default:
        remember(s, tid, fc, addr, val);
        break;
    }
}

/* One complete response frame. */
static void on_response(struct mb_state *s, const uint8_t *f, size_t len)
{
    s->frames_rsp++;
    if (len < 9) { s->bad_frames++; return; }
    uint16_t tid = be16(f);
    const uint8_t *pdu = f + 7;
    size_t plen = len - 7;
    uint8_t fc = pdu[0];
    struct mb_pending *p = lookup(s, tid);
    if (!p) return;                         /* reply we never saw asked */
    p->used = 0;
    if (fc & 0x80) { s->exceptions++; return; }
    if (fc != p->fc) { s->bad_frames++; return; }

    size_t nbytes = pdu[1];
    if (nbytes > plen - 2) nbytes = plen - 2;
    switch (fc) {
    case 1: set_bits(s, MB_COIL, p->addr, p->count, pdu + 2, nbytes); break;
    case 2: set_bits(s, MB_DI,   p->addr, p->count, pdu + 2, nbytes); break;
    case 3: set_regs(s, MB_HR,   p->addr, p->count, pdu + 2, nbytes); break;
    case 4: set_regs(s, MB_IR,   p->addr, p->count, pdu + 2, nbytes); break;
    default: break;                         /* write echoes: nothing new */
    }
}

/* Append bytes to a stream and pull out every complete MBAP frame. */
static void feed(struct mb_state *s, struct mb_stream *st, const uint8_t *p, size_t n,
                 void (*on_frame)(struct mb_state *, const uint8_t *, size_t))
{
    while (n > 0) {
        size_t room = sizeof st->buf - st->len;
        size_t take = n < room ? n : room;
        memcpy(st->buf + st->len, p, take);
        st->len += take; p += take; n -= take;

        size_t off = 0;
        while (st->len - off >= 7) {
            const uint8_t *h = st->buf + off;
            uint16_t proto = be16(h + 2), length = be16(h + 4);
            if (proto != 0 || length < 2 || length > 254) {
                /* Lost sync: drop everything and wait for clean data. */
                s->bad_frames++;
                off = st->len;
                break;
            }
            size_t flen = 6 + (size_t)length;
            if (st->len - off < flen) break;
            on_frame(s, h, flen);
            off += flen;
        }
        memmove(st->buf, st->buf + off, st->len - off);
        st->len -= off;
    }
}

void mb_feed_request(struct mb_state *s, const uint8_t *p, size_t n)
{
    feed(s, &s->to_field, p, n, on_request);
}

void mb_feed_response(struct mb_state *s, const uint8_t *p, size_t n)
{
    feed(s, &s->to_plc, p, n, on_response);
}
