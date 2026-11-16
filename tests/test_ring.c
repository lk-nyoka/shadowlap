#define _GNU_SOURCE
/* Ring buffer tests: order, drop-when-full, and a two-thread stress run. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "bbw.h"

static cmd_ring_t R;
static bcast_ring_t B;
static volatile int b_done;

static void *bproducer(void *a) {
    (void)a;
    for (uint64_t i = 0; i < 3000000; i++) {
        bbw_frame_t f = { .seq = i, .t_lap = (double)i * 2.0, .v = (double)i + 0.5 };
        bcast_publish(&B, &f);
    }
    b_done = 1;
    return NULL;
}

static void *producer(void *a) {
    (void)a;
    for (uint64_t i = 0; i < 2000000; i++) {
        bbw_cmd_t c = { .seq = i };
        cmd_ring_push(&R, &c);            /* may drop: that is allowed */
    }
    return NULL;
}

int main(void) {
    static cmd_ring_t r;
    bbw_cmd_t c, o;

    /* order */
    for (uint64_t i = 0; i < 10; i++) { c.seq = i; assert(cmd_ring_push(&r, &c)); }
    for (uint64_t i = 0; i < 10; i++) { assert(cmd_ring_pop(&r, &o)); assert(o.seq == i); }
    assert(!cmd_ring_pop(&r, &o));

    /* full ring drops new items and counts them, never blocks */
    for (uint64_t i = 0; i < BBW_RING_N; i++) { c.seq = i; assert(cmd_ring_push(&r, &c)); }
    c.seq = 99999;
    assert(!cmd_ring_push(&r, &c));
    assert(atomic_load(&r.dropped) == 1);
    cmd_ring_skip_to_head(&r);
    assert(!cmd_ring_pop(&r, &o));

    /* stress: consumer must see strictly increasing seq, no torn values */
    pthread_t t;
    pthread_create(&t, NULL, producer, NULL);
    uint64_t last = 0, got = 0;
    int first = 1, prod_done = 0;
    while (!prod_done || cmd_ring_pop(&R, &o)) {
        if (cmd_ring_pop(&R, &o)) {
            if (!first) assert(o.seq > last);
            last = o.seq; first = 0; got++;
        }
        if (!prod_done && pthread_tryjoin_np(t, NULL) == 0) prod_done = 1;
    }
    printf("test_ring: spsc ok (stress received %llu, dropped %llu)\n",
           (unsigned long long)got, (unsigned long long)atomic_load(&R.dropped));

    /* broadcast ring (plant -> v2, v2 read-only) */
    static bcast_ring_t b;
    bbw_frame_t f, g;
    assert(bcast_read(&b, 0, &g) == 0);                   /* nothing yet */
    for (uint64_t i = 0; i < 5; i++) { f.seq = i; bcast_publish(&b, &f); }
    for (uint64_t i = 0; i < 5; i++) { assert(bcast_read(&b, i, &g) == 1 && g.seq == i); }
    assert(bcast_read(&b, 5, &g) == 0);
    /* the writer never waits: a slow reader finds old frames gone (-1), not stale data */
    for (uint64_t i = 5; i < 5 + 2 * BBW_RING_N; i++) { f.seq = i; bcast_publish(&b, &f); }
    assert(bcast_read(&b, 0, &g) == -1);
    assert(bcast_read(&b, 5 + BBW_RING_N, &g) == 1 && g.seq == 5 + BBW_RING_N);

    /* stress: a reader racing the writer gets either a whole, correct frame or "lost" */
    pthread_create(&t, NULL, bproducer, NULL);
    uint64_t next = 0, okc = 0, lostc = 0;
    while (!b_done || next < atomic_load(&B.head)) {
        int r = bcast_read(&B, next, &g);
        if (r == 1) {
            assert(g.seq == next && g.t_lap == (double)next * 2.0 && g.v == (double)next + 0.5);
            okc++; next++;
        } else if (r == -1) { lostc++; next++; }
    }
    pthread_join(t, NULL);
    printf("test_ring: broadcast ok (read %llu whole frames, %llu reported lost, 0 torn)\n",
           (unsigned long long)okc, (unsigned long long)lostc);
    return 0;
}
