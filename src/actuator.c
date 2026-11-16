/*
 * The actuator: the only way to move the (simulated) brakes.
 *
 * Controllers use plain open()/write() on BBW_ACTUATOR_PATH.
 *   QNX:   the plant is a resource manager owning /dev/bbw/actuator.
 *          Each write() is a QNX message to the plant, so v1's priority is
 *          inherited by the plant thread that handles it. A security policy
 *          (see docs/security-policy.md) can deny v2 from opening the path.
 *   Linux: a FIFO stands in, so the same client code can be tested off-target.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bbw.h"
#include "control.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bbw_cmd_t g_latest;
static int64_t   g_recv_ns;
static int       g_have;
static uint64_t  g_rejects;
static pthread_t g_thread;
static volatile int g_stop;

/* Start the service thread with the caller's scheduling policy and priority,
 * then (inside the thread) pin it to the same CPU explicitly: on QNX the
 * runmask belongs to a thread and is not copied to new threads by default. */
static void *(*g_fn)(void *);
static void *thread_main(void *arg) {
    bbw_rt_setup_thread("plant-actuator");
    return g_fn(arg);
}
static int start_thread(void *(*fn)(void *)) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_INHERIT_SCHED);
    g_fn = fn;
    int rc = pthread_create(&g_thread, &a, thread_main, NULL);
    pthread_attr_destroy(&a);
    return rc == 0 ? 0 : -1;
}

/* The actuator does not trust whoever writes to it: a command must be a sane
 * number and must not go back in time. A bad command is counted and dropped;
 * it never replaces the last good one (so the plant falls back if needed). */
static void store(const bbw_cmd_t *c) {
    int64_t now = bbw_now_ns();
    pthread_mutex_lock(&g_lock);
    if (!cmd_plausible(c) || (g_have && c->seq < g_latest.seq)) {
        g_rejects++;
    } else {
        g_latest = *c;
        g_recv_ns = now;
        g_have = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

uint64_t bbw_actuator_rejects(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t r = g_rejects;
    pthread_mutex_unlock(&g_lock);
    return r;
}

int bbw_actuator_latest(bbw_cmd_t *out, int64_t *recv_ns) {
    pthread_mutex_lock(&g_lock);
    int have = g_have;
    if (have) { *out = g_latest; *recv_ns = g_recv_ns; }
    pthread_mutex_unlock(&g_lock);
    return have;
}

#ifdef __QNX__
/* ------------------------- QNX resource manager ------------------------- */
#include <sys/iofunc.h>
#include <sys/dispatch.h>

static resmgr_connect_funcs_t g_cfuncs;
static resmgr_io_funcs_t      g_iofuncs;
static iofunc_attr_t          g_attr;
static dispatch_t            *g_dpp;

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

    size_t nbytes = _IO_WRITE_GET_NBYTES(msg);
    if (nbytes != sizeof(bbw_cmd_t)) return EINVAL;

    bbw_cmd_t c;
    if (resmgr_msgget(ctp, &c, sizeof c, sizeof(msg->i)) != (ssize_t)sizeof c) return EIO;
    store(&c);

    _IO_SET_WRITE_NBYTES(ctp, nbytes);
    if (nbytes) ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
    return _RESMGR_NPARTS(0);
}

static void *serve(void *arg) {
    (void)arg;
    dispatch_context_t *ctp = dispatch_context_alloc(g_dpp);
    while (!g_stop) {
        dispatch_context_t *n = dispatch_block(ctp);
        if (n == NULL) continue;
        ctp = n;
        dispatch_handler(ctp);
    }
    return NULL;
}

int bbw_actuator_serve_start(void) {
    g_dpp = dispatch_create();
    if (!g_dpp) { perror("dispatch_create"); return -1; }
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_cfuncs, _RESMGR_IO_NFUNCS, &g_iofuncs);
    g_iofuncs.write = io_write;
    /* owner root, group may write, others nothing: v2 runs as another user */
    iofunc_attr_init(&g_attr, S_IFCHR | 0620, NULL, NULL);

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    if (resmgr_attach(g_dpp, &rattr, BBW_ACTUATOR_PATH, _FTYPE_ANY, 0,
                      &g_cfuncs, &g_iofuncs, &g_attr) == -1) {
        perror("resmgr_attach " BBW_ACTUATOR_PATH);
        return -1;
    }
    return start_thread(serve);
}

void bbw_actuator_serve_stop(void) { g_stop = 1; }

#else
/* ------------------------------ Linux FIFO ------------------------------ */
static int g_fd = -1;

static void *serve(void *arg) {
    (void)arg;
    bbw_cmd_t c;
    while (!g_stop) {
        ssize_t n = read(g_fd, &c, sizeof c);       /* FIFO writes <= PIPE_BUF are atomic */
        if (n == (ssize_t)sizeof c) store(&c);
        else if (n < 0 && errno != EINTR) break;
    }
    return NULL;
}

int bbw_actuator_serve_start(void) {
    unlink(BBW_ACTUATOR_PATH);
    mode_t old = umask(0);
    int rc = mkfifo(BBW_ACTUATOR_PATH, 0620);
    umask(old);
    if (rc != 0) { perror("mkfifo"); return -1; }
    g_fd = open(BBW_ACTUATOR_PATH, O_RDWR);       /* RDWR: never sees EOF */
    if (g_fd < 0) { perror("open actuator"); return -1; }
    return start_thread(serve);
}

void bbw_actuator_serve_stop(void) {
    g_stop = 1;
    if (g_fd >= 0) { pthread_cancel(g_thread); close(g_fd); }
    unlink(BBW_ACTUATOR_PATH);
}
#endif

/* ------------------------------ client side ----------------------------- */
int bbw_actuator_open(void) {
    for (int i = 0; i < 100; i++) {
        int fd = open(BBW_ACTUATOR_PATH, O_WRONLY);
        if (fd >= 0) return fd;
        if (errno != ENOENT) return -1;      /* e.g. EACCES: blocked */
        usleep(50000);
    }
    return -1;
}

int bbw_actuator_send(int fd, const bbw_cmd_t *c) {
    return write(fd, c, sizeof *c) == (ssize_t)sizeof *c ? 0 : -1;
}
