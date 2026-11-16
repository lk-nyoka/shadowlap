/*
 * shadowlap - shadow-mode testing for a brake-by-wire controller on QNX.
 *
 * Shared definitions used by every process:
 *   bbw_plant    simulated car + lap replay + actuator (owns the clock)
 *   bbw_v1       production brake controller (in charge)
 *   bbw_v2       shadow brake controller (watches only)
 *   bbw_comparer joins v1 and v2 decisions and logs disagreements
 *   bbw_hog      CPU load generator for the "flood" scenario
 *
 * All physical values are a deliberately small, illustrative model.
 * They are NOT real F1 data or real F1 control laws.
 */
#ifndef BBW_H
#define BBW_H

#include <stdint.h>
#include <stdatomic.h>
#include <semaphore.h>

/* ---------- timing ---------- */
#define BBW_HZ            100
#define BBW_PERIOD_NS     (1000000000LL / BBW_HZ)   /* 10 ms */
#define BBW_DEADLINE_NS   8000000LL                 /* v1 must answer within 8 ms */
#define BBW_DT            (1.0 / BBW_HZ)

/* ---------- shared-memory object names ---------- */
#define BBW_SHM_PROD    "/bbw_prod"    /* plant <-> v1 only                     (mode 0600) */
#define BBW_SHM_SHIN    "/bbw_shin"    /* plant -> v2: v2 maps it READ-ONLY     (mode 0644) */
#define BBW_SHM_SHOUT   "/bbw_shout"   /* v2 -> comparer: untrusted, validated  (mode 0666) */
#define BBW_SHM_REC     "/bbw_rec"     /* plant -> comparer                     (mode 0600) */
#define BBW_MAGIC       0x42425731u    /* "BBW1" */

/* ---------- actuator path ---------- */
#ifdef __QNX__
#define BBW_ACTUATOR_PATH "/dev/bbw/actuator"   /* QNX resource manager */
#else
#define BBW_ACTUATOR_PATH "/tmp/bbw_actuator"   /* Linux stand-in: a FIFO */
#endif

/* ---------- priorities (SCHED_FIFO) ----------
 * On QNX, priorities 64-255 are privileged (PROCMGR_AID_PRIORITY): an
 * unprivileged process can reach at most 63. The car and v1 sit above that
 * line, so a hostile v2 running as another user can never out-rank them,
 * whatever it does to its own priority or CPU. (Linux: unprivileged users
 * get no real-time priority at all by default, RLIMIT_RTPRIO = 0.) */
#ifdef __QNX__
#define BBW_PRIO_PLANT     70
#define BBW_PRIO_V1        65
#else
#define BBW_PRIO_PLANT     50
#define BBW_PRIO_V1        45
#endif
#define BBW_PRIO_COMPARER  20
#define BBW_PRIO_V2        10
#define BBW_PRIO_HOG       10

/* ---------- messages ---------- */

/* What the plant tells the controllers every cycle. */
typedef struct {
    uint64_t seq;          /* cycle number */
    int64_t  t_ns;         /* CLOCK_MONOTONIC when published */
    double   t_lap;        /* seconds into the lap */
    double   dist_m;       /* distance into the lap */
    double   v;            /* speed, m/s */
    double   a_req;        /* driver deceleration demand, m/s^2 (>= 0) */
    double   throttle;     /* 0..1 */
    int32_t  brake;        /* 0/1 */
    int32_t  zone;         /* braking-zone id, -1 if none */
    double   soc;          /* battery state of charge, 0..1 of usable window */
    double   lap_harvest_j;/* energy harvested so far this lap, J */
    double   f_rear_req;   /* rear braking force the driver is asking for, N */
    double   f_rear_grip;  /* rear tyre grip limit right now, N (speed, wings, track) */
    double   motor_temp_c; /* motor temperature, deg C */
    double   wing_closed;  /* 0 = straight-line mode (low downforce), 1 = corner mode */
} bbw_frame_t;

/* What a controller decides. */
typedef struct {
    uint64_t seq;          /* frame this answers */
    int64_t  t_done_ns;    /* when the decision was made */
    double   f_regen;      /* rear braking from the motor (harvesting), N */
    double   f_friction;   /* rear braking from the discs, N */
} bbw_cmd_t;

/* One row per cycle, written by the plant for the comparer. */
typedef struct {
    bbw_frame_t f;
    bbw_cmd_t   cmd;           /* command actually applied */
    int32_t     applied;       /* 1 = v1 answered by the deadline */
    int32_t     clamped;       /* 1 = plant had to clamp v1's command */
    int64_t     latency_ns;    /* v1 answer arrival - frame publish */
    double      p_regen_w;     /* regen power actually applied */
    uint64_t    shadow_dropped;/* cumulative frames v2 missed (ring full) */
    int64_t     plant_lag_ns;  /* how late the plant itself published this frame */
} bbw_record_t;

/* the plant counts as late (an OS/host problem, not v1's) beyond this */
#define BBW_PLANT_LATE_NS 2000000LL

/* ---------- single-producer / single-consumer ring ----------
 * The producer never waits. If the ring is full the new item is dropped
 * and counted. This is what keeps v1 independent of v2. */
#define BBW_RING_N 4096u   /* must be a power of two */

#define BBW_DEFINE_RING(NAME, T)                                              \
typedef struct {                                                              \
    _Atomic uint64_t head;      /* written by producer */                     \
    _Atomic uint64_t tail;      /* written by consumer */                     \
    _Atomic uint64_t dropped;   /* items producer had to drop */              \
    T buf[BBW_RING_N];                                                        \
} NAME##_t;                                                                   \
static inline int NAME##_push(NAME##_t *r, const T *v) {                      \
    uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);        \
    uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);        \
    if (h - t >= BBW_RING_N) {                                                \
        atomic_fetch_add_explicit(&r->dropped, 1, memory_order_relaxed);      \
        return 0;                                                             \
    }                                                                         \
    r->buf[h & (BBW_RING_N - 1)] = *v;                                        \
    atomic_store_explicit(&r->head, h + 1, memory_order_release);             \
    return 1;                                                                 \
}                                                                             \
static inline int NAME##_pop(NAME##_t *r, T *out) {                           \
    uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);        \
    uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);        \
    if (t == h) return 0;                                                     \
    *out = r->buf[t & (BBW_RING_N - 1)];                                      \
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);             \
    return 1;                                                                 \
}                                                                             \
/* consumer-only: throw away a stale backlog (used when v2 restarts) */       \
static inline void NAME##_skip_to_head(NAME##_t *r) {                         \
    uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);        \
    atomic_store_explicit(&r->tail, h, memory_order_release);                 \
}

BBW_DEFINE_RING(cmd_ring, bbw_cmd_t)

/* ---------- broadcast ring: plant -> v2, v2 can only READ it ----------
 * The plant overwrites old slots and never learns whether v2 kept up, so
 * nothing v2 does (slow, frozen, malicious) can reach back to the plant.
 * Each slot carries its own sequence lock so a reader can tell when a slot
 * was overwritten while it was copying (it then counts the frame as lost). */
typedef struct {
    _Atomic uint64_t lock;      /* even = stable, odd = being written */
    bbw_frame_t      f;
} bbw_bslot_t;

typedef struct {
    _Atomic uint64_t head;      /* frames published so far */
    bbw_bslot_t      slot[BBW_RING_N];
} bcast_ring_t;

static inline void bcast_publish(bcast_ring_t *r, const bbw_frame_t *f) {
    uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    bbw_bslot_t *s = &r->slot[h & (BBW_RING_N - 1)];
    uint64_t l = atomic_load_explicit(&s->lock, memory_order_relaxed);
    atomic_store_explicit(&s->lock, l + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    s->f = *f;
    atomic_store_explicit(&s->lock, l + 2, memory_order_release);
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
}

/* Read frame number idx. Returns 1 ok, 0 not published yet, -1 lost
 * (overwritten before or while it was read). */
static inline int bcast_read(const bcast_ring_t *r, uint64_t idx, bbw_frame_t *out) {
    uint64_t h = atomic_load_explicit(&((bcast_ring_t *)r)->head, memory_order_acquire);
    if (idx >= h) return 0;
    if (h - idx > BBW_RING_N) return -1;
    const bbw_bslot_t *s = &r->slot[idx & (BBW_RING_N - 1)];
    uint64_t l1 = atomic_load_explicit(&((bbw_bslot_t *)s)->lock, memory_order_acquire);
    if (l1 & 1) return -1;
    *out = s->f;
    atomic_thread_fence(memory_order_acquire);
    uint64_t l2 = atomic_load_explicit(&((bbw_bslot_t *)s)->lock, memory_order_relaxed);
    if (l1 != l2 || out->seq != idx) return -1;
    return 1;
}
BBW_DEFINE_RING(rec_ring, bbw_record_t)

/* ---------- shared regions ---------- */

/* Production region: only the plant and v1 may open this. */
typedef struct {
    uint32_t         magic;
    sem_t            frame_ready;    /* plant posts once per cycle */
    _Atomic uint64_t seqlock;        /* even = stable, odd = being written */
    bbw_frame_t      latest;
    _Atomic int32_t  plant_running;
    _Atomic int32_t  v1_rt_ok;       /* 0 unknown, 1 real-time set up, -1 failed */
} bbw_prod_t;

/* Shadow input: written by the plant, mapped read-only by v2. No semaphore:
 * v2 polls, because waiting on a semaphore would need write access. */
typedef struct {
    uint32_t         magic;
    _Atomic int32_t  plant_running;
    bcast_ring_t     in;
} bbw_shin_t;

/* Shadow output: written by v2, read by the comparer. Everything in here is
 * untrusted: the comparer checks it before using it. */
typedef struct {
    uint32_t         magic;
    _Atomic int64_t  v2_heartbeat_ns;
    _Atomic uint64_t v2_lost;        /* frames v2 says it lost (informational only) */
    cmd_ring_t       out;
} bbw_shout_t;

/* Record region: plant -> comparer. */
typedef struct {
    uint32_t         magic;
    _Atomic int32_t  plant_running;
    _Atomic int32_t  run_done;
    int32_t          plant_rt_ok;    /* filled in before run_done */
    int32_t          v1_rt_ok;
    uint64_t         actuator_rejects;
    uint64_t         fallbacks;
    rec_ring_t       cycles;
} bbw_rec_t;

/* ---------- common helpers (common.c) ---------- */
int64_t bbw_now_ns(void);
void    bbw_sleep_until_ns(int64_t t_ns);
/* Real-time setup for the calling thread (+ memory lock once per process).
 * Returns 1 if everything asked for was set, 0 if not. With BBW_REQUIRE_RT=1
 * in the environment a failure ends the process (exit 3): a timing run that
 * did not get real-time scheduling must not produce a result. */
int     bbw_rt_setup(const char *who, int prio, int cpu);
/* Re-apply the same priority and CPU to a thread the process created later. */
int     bbw_rt_setup_thread(const char *who);
void   *bbw_shm_create(const char *name, size_t size, int mode);
void   *bbw_shm_open(const char *name, size_t size, int wait_ms);
const void *bbw_shm_open_ro(const char *name, size_t size, int wait_ms);
void    bbw_shm_unlink_all(void);
void    bbw_seqlock_write(bbw_prod_t *p, const bbw_frame_t *f);
void    bbw_seqlock_read(bbw_prod_t *p, bbw_frame_t *out);
int     bbw_arg_int(int argc, char **argv, const char *key, int def);
double  bbw_arg_double(int argc, char **argv, const char *key, double def);
const char *bbw_arg_str(int argc, char **argv, const char *key, const char *def);
int     bbw_arg_flag(int argc, char **argv, const char *key);

/* ---------- actuator (actuator.c) ---------- */
/* plant side: start the actuator service; incoming commands land in *slot */
int  bbw_actuator_serve_start(void);
int  bbw_actuator_latest(bbw_cmd_t *out, int64_t *recv_ns);
uint64_t bbw_actuator_rejects(void);          /* commands refused as invalid */
void bbw_actuator_serve_stop(void);
/* controller side */
int  bbw_actuator_open(void);                 /* returns fd or -1 (errno set) */
int  bbw_actuator_send(int fd, const bbw_cmd_t *c);

#endif
