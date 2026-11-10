#ifndef NERO_DIAGNOSTIC_PROCESS_H
#define NERO_DIAGNOSTIC_PROCESS_H
/* POSIX-only bounded ICMP reachability helper. Callers never supply an
 * executable, options, environment or shell program. Tests replace spawn/wait
 * with inert fixtures; no diagnostic packets are emitted in the test suite. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static inline int nero_ping_ipv4(const char *ip, const volatile sig_atomic_t *cancel) {
    struct in_addr address;
    if (!ip || inet_pton(AF_INET, ip, &address) != 1 || (cancel && *cancel)) return 0;
    char target[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &address, target, sizeof(target)) || strcmp(ip, target) != 0) return 0;
#ifdef __APPLE__
    const char *executable = "/sbin/ping";
    char wait_time[] = "1000"; /* Darwin uses milliseconds. */
#else
    const char *executable = "/bin/ping";
    char wait_time[] = "1"; /* Linux uses seconds. */
#endif
    char *const arguments[] = {(char *)executable, "-n", "-c", "1", "-W", wait_time, target, NULL};
    char *const environment[] = {"PATH=/usr/bin:/bin", "LANG=C", NULL};
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions) != 0) return 0;
    if (posix_spawnattr_init(&attributes) != 0) { posix_spawn_file_actions_destroy(&actions); return 0; }
    int error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_RESETIDS;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
    /* Supported by the project's Ubuntu/glibc baseline. Close everything
     * beyond stdio, including descriptors not originally opened by NERO. */
    if (!error) error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
    sigset_t empty, defaults;
    sigemptyset(&empty); sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGTERM);
    if (!error) error = posix_spawnattr_setsigmask(&attributes, &empty);
    if (!error) error = posix_spawnattr_setsigdefault(&attributes, &defaults);
    if (!error) error = posix_spawnattr_setflags(&attributes, flags);
    struct timespec start;
    if (!error && clock_gettime(CLOCK_MONOTONIC, &start) != 0) error = errno;
    pid_t child = -1;
    if (!error) error = posix_spawn(&child, executable, &actions, &attributes, arguments, environment);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (error) return 0;
    for (;;) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);
        int wait_error = result < 0 ? errno : 0;
        if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno == ECHILD) return 0;
        struct timespec now;
        int clock_error = clock_gettime(CLOCK_MONOTONIC, &now) != 0;
        int expired = !clock_error && ((double)(now.tv_sec - start.tv_sec) +
                      (double)(now.tv_nsec - start.tv_nsec) / 1000000000.0 >= 3.0);
        if ((result < 0 && wait_error != EINTR) || clock_error || expired || (cancel && *cancel)) {
            (void)kill(child, SIGKILL);
            do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
            return 0;
        }
        struct timespec pause = {0, 10000000};
        (void)nanosleep(&pause, NULL);
    }
}
#endif
