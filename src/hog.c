/*
 * bbw_hog - burns CPU at the shadow's priority, to show that a greedy
 * shadow side cannot starve the production path.
 *   bbw_hog --threads N --seconds S [--cpu C] [--prio P]
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include "bbw.h"

static int g_cpu, g_prio;
static volatile int g_run = 1;

static void *spin(void *a) {
    (void)a;
    bbw_rt_setup("hog", g_prio, g_cpu);
    volatile unsigned long x = 0;
    while (g_run) x++;
    return NULL;
}

int main(int argc, char **argv) {
    int n = bbw_arg_int(argc, argv, "--threads", 2);
    int secs = bbw_arg_int(argc, argv, "--seconds", 10);
    g_cpu = bbw_arg_int(argc, argv, "--cpu", -1);
    g_prio = bbw_arg_int(argc, argv, "--prio", BBW_PRIO_HOG);
    if (n > 64) n = 64;
    pthread_t t[64];
    fprintf(stderr, "[hog] %d threads for %d s at priority %d\n", n, secs, g_prio);
    for (int i = 0; i < n; i++) pthread_create(&t[i], NULL, spin, NULL);
    sleep((unsigned)secs);
    g_run = 0;
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    return 0;
}
