#define _GNU_SOURCE
/* Ring buffer tests: order, drop-when-full, and a two-thread stress run. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "bbw.h"

static cmd_ring_t R;

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
    printf("test_ring: ok (stress received %llu, dropped %llu)\n",
           (unsigned long long)got, (unsigned long long)atomic_load(&R.dropped));
    return 0;
}
