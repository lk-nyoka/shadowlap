#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __QNX__
#include <sys/neutrino.h>
#endif
#include "bbw.h"

int64_t bbw_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void bbw_sleep_until_ns(int64_t t_ns) {
    struct timespec ts = { .tv_sec = t_ns / 1000000000LL, .tv_nsec = t_ns % 1000000000LL };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }
}

/* Real-time setup. Failure is reported; with BBW_REQUIRE_RT=1 it is fatal,
 * so a timing test can never quietly run without real-time scheduling. */
static int g_rt_prio = 0, g_rt_cpu = -1;

static int rt_fail(const char *who, const char *what, int err) {
    const char *req = getenv("BBW_REQUIRE_RT");
    int required = req && req[0] == '1';
    fprintf(stderr, "[%s] %s: %s not set (%s)\n", who, required ? "ERROR" : "warning", what, strerror(err));
    if (required) {
        fprintf(stderr, "[%s] BBW_REQUIRE_RT=1: refusing to run without real-time scheduling\n", who);
        exit(3);
    }
    return 0;
}

static int rt_thread(const char *who, int prio, int cpu) {
    int ok = 1;
    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != 0) ok = rt_fail(who, "SCHED_FIFO priority", rc);
        else {                                   /* verify, do not assume */
            int pol = 0; struct sched_param got;
            if (pthread_getschedparam(pthread_self(), &pol, &got) != 0 || pol != SCHED_FIFO ||
                got.sched_priority != prio)
                ok = rt_fail(who, "SCHED_FIFO priority (verify)", EINVAL);
        }
    }
    if (cpu >= 0) {
#ifdef __QNX__
        /* _NTO_TCTL_RUNMASK pins the CALLING thread only; every thread that
         * matters calls this (see bbw_rt_setup_thread), so none is left floating */
        unsigned runmask = 1u << cpu;
        if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)runmask) == -1)
            ok = rt_fail(who, "CPU runmask", errno);
#elif defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0)     /* 0 = calling thread */
            ok = rt_fail(who, "CPU affinity", errno);
#endif
    }
    return ok;
}

int bbw_rt_setup(const char *who, int prio, int cpu) {
    g_rt_prio = prio;
    g_rt_cpu = cpu;
    int ok = rt_thread(who, prio, cpu);
    /* Lock memory only when real-time priority was granted. Without it the run's
     * timing is already marked invalid, and locking future memory under a normal
     * user's small RLIMIT_MEMLOCK makes later thread stacks fail to allocate. */
    if (prio > 0 && ok && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        ok = rt_fail(who, "mlockall (memory lock)", errno) && ok;
    if (ok && (prio > 0 || cpu >= 0))
        fprintf(stderr, "[%s] real-time: SCHED_FIFO %d, cpu %d, memory locked\n", who, prio, cpu);
    return ok;
}

int bbw_rt_setup_thread(const char *who) { return rt_thread(who, g_rt_prio, g_rt_cpu); }

void *bbw_shm_create(const char *name, size_t size, int mode) {
    shm_unlink(name);
    mode_t old = umask(0);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, mode);
    umask(old);
    if (fd < 0) { perror(name); exit(1); }
    if (ftruncate(fd, (off_t)size) != 0) { perror("ftruncate"); exit(1); }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    memset(p, 0, size);
    return p;
}

/* Open an existing region, retrying until the plant has created it.
 * Returns NULL (errno set) if it never appears or access is denied. */
void *bbw_shm_open(const char *name, size_t size, int wait_ms) {
    int fd = -1;
    for (int waited = 0;; waited += 50) {
        fd = shm_open(name, O_RDWR, 0);
        if (fd >= 0) break;
        if (errno != ENOENT || waited >= wait_ms) return NULL;
        usleep(50000);
    }
    struct stat st;
    for (int i = 0; i < 100; i++) {                 /* wait for ftruncate */
        if (fstat(fd, &st) == 0 && (size_t)st.st_size >= size) break;
        usleep(10000);
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int e = errno;
    close(fd);
    if (p == MAP_FAILED) { errno = e; return NULL; }
    /* wait until the creator has finished initialising */
    for (int i = 0; i < 200 && *(volatile uint32_t *)p != BBW_MAGIC; i++) usleep(10000);
    return p;
}

/* Open a region the caller may only READ: O_RDONLY + PROT_READ, so even a
 * buggy or hostile reader cannot change it (and the OS refuses O_RDWR to
 * other users because of the 0644 mode). */
const void *bbw_shm_open_ro(const char *name, size_t size, int wait_ms) {
    int fd = -1;
    for (int waited = 0;; waited += 50) {
        fd = shm_open(name, O_RDONLY, 0);
        if (fd >= 0) break;
        if (errno != ENOENT || waited >= wait_ms) return NULL;
        usleep(50000);
    }
    struct stat st;
    for (int i = 0; i < 100; i++) {
        if (fstat(fd, &st) == 0 && (size_t)st.st_size >= size) break;
        usleep(10000);
    }
    void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    int e = errno;
    close(fd);
    if (p == MAP_FAILED) { errno = e; return NULL; }
    for (int i = 0; i < 200 && *(volatile const uint32_t *)p != BBW_MAGIC; i++) usleep(10000);
    return p;
}

void bbw_shm_unlink_all(void) {
    shm_unlink(BBW_SHM_PROD);
    shm_unlink(BBW_SHM_SHIN);
    shm_unlink(BBW_SHM_SHOUT);
    shm_unlink(BBW_SHM_REC);
}

void bbw_seqlock_write(bbw_prod_t *p, const bbw_frame_t *f) {
    uint64_t s = atomic_load_explicit(&p->seqlock, memory_order_relaxed);
    atomic_store_explicit(&p->seqlock, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    p->latest = *f;
    atomic_store_explicit(&p->seqlock, s + 2, memory_order_release);
}

void bbw_seqlock_read(bbw_prod_t *p, bbw_frame_t *out) {
    for (;;) {
        uint64_t s1 = atomic_load_explicit(&p->seqlock, memory_order_acquire);
        if (s1 & 1) continue;
        *out = p->latest;
        atomic_thread_fence(memory_order_acquire);
        uint64_t s2 = atomic_load_explicit(&p->seqlock, memory_order_relaxed);
        if (s1 == s2) return;
    }
}

/* tiny "--key value" parser */
static const char *find(int argc, char **argv, const char *key) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], key) == 0) return argv[i + 1];
    return NULL;
}
int bbw_arg_int(int argc, char **argv, const char *key, int def) {
    const char *v = find(argc, argv, key);
    return v ? atoi(v) : def;
}
double bbw_arg_double(int argc, char **argv, const char *key, double def) {
    const char *v = find(argc, argv, key);
    return v ? atof(v) : def;
}
const char *bbw_arg_str(int argc, char **argv, const char *key, const char *def) {
    const char *v = find(argc, argv, key);
    return v ? v : def;
}
int bbw_arg_flag(int argc, char **argv, const char *key) {
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], key) == 0) return 1;
    return 0;
}
