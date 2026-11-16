/*
 * bbw_runas USER COMMAND [ARGS...] - drop to USER, then BECOME the command.
 *
 * The trusted launcher for the candidate. It must be started as root. It
 * switches group and user irreversibly, checks it cannot switch back, then
 * exec()s the command in its own process, so the process ID the demo script
 * gets is the candidate itself (needed to stop it, signal it and budget it).
 * QNX's `on -u` and Linux's `runuser` both start the command as a CHILD
 * process instead, so the script would otherwise be watching the launcher.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: bbw_runas USER COMMAND [ARGS...]\n"); return 2; }
    struct passwd *pw = getpwnam(argv[1]);
    if (!pw) { fprintf(stderr, "[runas] no such user %s\n", argv[1]); return 2; }
    if (pw->pw_uid == 0) { fprintf(stderr, "[runas] refusing to run the candidate as root\n"); return 2; }
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
