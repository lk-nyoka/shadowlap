/*
 * spin - a misbehaving program for the codelab: burns one CPU.
 *
 *   spin [seconds]            spin in this process
 *   spin [seconds] --child    start a child process that spins, while this
 *                             process sits idle (the trick that fools a
 *                             watchdog that only watches one process ID)
 *
 * Builds on QNX 8.0 (qcc) and on Linux (cc).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void burn(double seconds) {
    double end = now_s() + seconds;
    volatile unsigned long x = 0;
    while (now_s() < end) x++;
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 30.0;
    int child = argc > 2 && strcmp(argv[2], "--child") == 0;
    printf("spin: pid %d, burning a CPU for %.0f s%s\n", (int)getpid(), secs,
           child ? " in a child process" : "");
    fflush(stdout);
    if (child) {
        pid_t c = fork();
        if (c == 0) { burn(secs); _exit(0); }
        if (c < 0) { perror("fork"); return 1; }
        printf("spin: child %d spins, parent %d stays idle\n", (int)c, (int)getpid());
        fflush(stdout);
        waitpid(c, NULL, 0);
    } else {
        burn(secs);
    }
    printf("spin: finished without being stopped\n");
    return 0;
}
