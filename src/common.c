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

/* Give this thread a fixed real-time priority and optionally pin it to one CPU.
 * Failure is reported but not fatal, so the demo still runs as a normal user. */
void bbw_rt_setup(const char *who, int prio, int cpu) {
    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != 0)
            fprintf(stderr, "[%s] warning: SCHED_FIFO %d not set (%s); running at normal priority\n",
                    who, prio, strerror(rc));
    }
    if (cpu >= 0) {
#ifdef __QNX__
        unsigned runmask = 1u << cpu;
        if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)runmask) == -1)
            fprintf(stderr, "[%s] warning: runmask cpu %d not set (%s)\n", who, cpu, strerror(errno));
#elif defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0)
            fprintf(stderr, "[%s] warning: affinity cpu %d not set (%s)\n", who, cpu, strerror(errno));
#endif
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0 && prio > 0)
        fprintf(stderr, "[%s] note: mlockall failed (%s)\n", who, strerror(errno));
}

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

void bbw_shm_unlink_all(void) {
    shm_unlink(BBW_SHM_PROD);
    shm_unlink(BBW_SHM_SHADOW);
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
