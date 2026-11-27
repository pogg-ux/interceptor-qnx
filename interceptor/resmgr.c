/*
 * QNX resource manager: publishes the Interceptor's views as read-only files
 *   /dev/interceptor/live     I/O + tap table
 *   /dev/interceptor/events   event timeline (newest 300)
 *   /dev/interceptor/status   link health and counters
 * Any QNX tool can read them:  cat /dev/interceptor/status
 *
 * Runs in its own thread with a standard dispatch loop, so a slow reader can
 * never delay the passthrough in the main thread. Each open() + read from
 * offset 0 takes a fresh snapshot of the view.
 */
#include "views.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifndef __QNX__
int resmgr_start(char *why, size_t why_len)
{
    snprintf(why, why_len, "not QNX: /dev/interceptor not available (use the /tmp files)");
    return -1;
}
#else

#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;
static iofunc_attr_t attrs[VIEW_N];
static char snap[VIEW_N][VIEW_MAX];
static size_t snap_len[VIEW_N];
static dispatch_t *dpp;

static int which_view(iofunc_attr_t *a)
{
    for (int i = 0; i < VIEW_N; i++) if (a == &attrs[i]) return i;
    return -1;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

    int v = which_view((iofunc_attr_t *)ocb->attr);
    if (v < 0) return EBADF;

    if (ocb->offset == 0) {                       /* new read: fresh snapshot */
        snap_len[v] = views_get((enum view_id)v, snap[v], VIEW_MAX);
        attrs[v].nbytes = (off_t)snap_len[v];
    }

    size_t off = (size_t)ocb->offset;
    size_t left = off < snap_len[v] ? snap_len[v] - off : 0;
    size_t want = _IO_READ_GET_NBYTES(msg);
    size_t n = left < want ? left : want;

    _IO_SET_READ_NBYTES(ctp, n);
    if (n == 0) return _RESMGR_NPARTS(0);
    SETIOV(ctp->iov, snap[v] + off, n);
    ocb->offset += n;
    ocb->attr->flags |= IOFUNC_ATTR_ATIME;
    return _RESMGR_NPARTS(1);
}

static void *resmgr_thread(void *arg)
{
    (void)arg;
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    if (!ctp) return NULL;
    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL) continue;
        dispatch_handler(ctp);
    }
    return NULL;
}

int resmgr_start(char *why, size_t why_len)
{
    if ((dpp = dispatch_create()) == NULL) {
        snprintf(why, why_len, "dispatch_create failed: %s", strerror(errno));
        return -1;
    }
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = io_read;

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 4096;

    for (int i = 0; i < VIEW_N; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/interceptor/%s", VIEW_NAMES[i]);
        iofunc_attr_init(&attrs[i], S_IFREG | 0444, NULL, NULL);
        if (resmgr_attach(dpp, &rattr, path, _FTYPE_ANY, 0,
                          &connect_funcs, &io_funcs, &attrs[i]) == -1) {
            snprintf(why, why_len, "can't create %s (%s)%s", path, strerror(errno),
                     getuid() != 0 ? " - run as root" : "");
            return -1;
        }
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, resmgr_thread, NULL) != 0) {
        snprintf(why, why_len, "pthread_create failed");
        return -1;
    }
    snprintf(why, why_len, "/dev/interceptor/{live,events,status} ready");
    return 0;
}
#endif
