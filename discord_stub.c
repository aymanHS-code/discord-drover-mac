/*
 * Replacement Discord executable. Loads drover_direct.dylib as a linked
 * dependency (no DYLD_INSERT_LIBRARIES), then starts Electron.
 *
 * Matches Discord's original stub: fix invalid stdio from Launch Services,
 * then ElectronMain (or run-as-node when that fuse is on).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

extern int ElectronMain(int argc, char **argv);
extern int ElectronInitializeICUandStartNode(int argc, char **argv);
extern unsigned char _ZN8electron5fuses18IsRunAsNodeEnabledEv(void);

static void reopen_stdio_if_needed(void) {
    struct stat info;

    if (fstat(STDIN_FILENO, &info) < 0 && errno == EBADF) {
        (void)freopen("/dev/null", "r", stdin);
    }
    if (fstat(STDOUT_FILENO, &info) < 0 && errno == EBADF) {
        (void)freopen("/dev/null", "w", stdout);
    }
    if (fstat(STDERR_FILENO, &info) < 0 && errno == EBADF) {
        (void)freopen("/dev/null", "w", stderr);
    }
}

int main(int argc, char **argv) {
    reopen_stdio_if_needed();
    if (_ZN8electron5fuses18IsRunAsNodeEnabledEv() != 0) {
        const char *run_as_node = getenv("ELECTRON_RUN_AS_NODE");
        if (run_as_node != NULL && run_as_node[0] != '\0') {
            return ElectronInitializeICUandStartNode(argc, argv);
        }
    }
    return ElectronMain(argc, argv);
}
