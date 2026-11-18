/*
 * bbw_runas USER COMMAND [ARGS...] - drop to USER, then BECOME the command.
 *
 * The trusted launcher for the candidate. It must be started as root. It
 * switches group and user irreversibly, checks it cannot switch back, then
 * exec()s the command in its own process, so the process ID the demo script
 * gets is the candidate itself (needed to stop it, signal it and budget it).
 * QNX's `on -u` and Linux's `runuser` both start the command as a CHILD
 * process instead, so the script would otherwise be watching the launcher.
 *
 * Before dropping privileges it also sets hard resource limits the candidate
 * cannot raise: no new processes (so it cannot hide CPU use in a child the
 * guard is not watching), at most 256 MB of address space, 64 open files.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/resource.h>
#include <string.h>
#include <unistd.h>

static int cap(int res, rlim_t v, const char *what) {
    struct rlimit r = { v, v };
    if (setrlimit(res, &r) != 0) { fprintf(stderr, "[runas] cannot limit %s: %s\n", what, strerror(errno)); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: bbw_runas USER COMMAND [ARGS...]\n"); return 2; }
    struct passwd *pw = getpwnam(argv[1]);
    if (!pw) { fprintf(stderr, "[runas] no such user %s\n", argv[1]); return 2; }
    if (pw->pw_uid == 0) { fprintf(stderr, "[runas] refusing to run the candidate as root\n"); return 2; }
    int lim = 0;
#if defined(RLIMIT_NPROC) && !defined(__QNX__)
    /* Linux: 0 = no fork for a non-root user; exec below reuses this process.
     * Not on QNX: there exec itself counts against RLIMIT_NPROC, so a limit of
     * 0 stops the candidate from starting at all (EAGAIN, measured 18 Nov).
     * On QNX child processes are left to the guard, which watches every process
     * the candidate's user owns (src/guard.c --user). */
    lim |= cap(RLIMIT_NPROC, 0, "new processes");
#endif
    lim |= cap(RLIMIT_AS, (rlim_t)256 << 20, "memory");
    lim |= cap(RLIMIT_NOFILE, 64, "open files");
    if (lim) { fprintf(stderr, "[runas] not starting the candidate without its limits\n"); return 2; }
    if (initgroups(pw->pw_name, pw->pw_gid) != 0 || setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0) {
        fprintf(stderr, "[runas] cannot become %s: %s (must be started as root)\n", argv[1], strerror(errno));
        return 2;
    }
    if (setuid(0) == 0 || getuid() == 0 || geteuid() == 0) {
        fprintf(stderr, "[runas] privilege drop did not stick; not starting the candidate\n");
        return 2;
    }
    execv(argv[2], argv + 2);
    fprintf(stderr, "[runas] cannot start %s: %s\n", argv[2], strerror(errno));
    return 127;
}
