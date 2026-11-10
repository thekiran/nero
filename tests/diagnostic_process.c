#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int spawns, waits, killed, mode, ticks;
static volatile sig_atomic_t cancelled;
static int fake_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                      const posix_spawnattr_t *attributes, char *const argv[], char *const env[]) {
    assert(actions && attributes);
#ifdef __APPLE__
    assert(strcmp(path, "/sbin/ping") == 0);
    assert(strcmp(argv[5], "1000") == 0);
#else
    assert(strcmp(path, "/bin/ping") == 0);
    assert(strcmp(argv[5], "1") == 0);
#endif
    assert(strcmp(argv[0], path) == 0 && strcmp(argv[1], "-n") == 0);
    assert(strcmp(argv[2], "-c") == 0 && strcmp(argv[3], "1") == 0);
    assert(strcmp(argv[4], "-W") == 0 && strcmp(argv[6], "192.0.2.1") == 0 && !argv[7]);
    assert(strcmp(env[0], "PATH=/usr/bin:/bin") == 0 && strcmp(env[1], "LANG=C") == 0 && !env[2]);
    ++spawns; *pid = 12345;
    return mode == 1 ? ENOENT : 0;
}
static pid_t fake_wait(pid_t pid, int *status, int flags) {
    assert(pid == 12345); ++waits;
    if (!flags) { assert(killed); if (waits == 2 && mode == 5) { errno = EINTR; return -1; } *status = SIGKILL; return pid; }
    if (mode == 2 || mode == 5) return 0;
    if (mode == 3) { cancelled = 1; return 0; }
    if (mode == 6 && waits == 1) { errno = EINTR; return -1; }
    *status = mode == 4 ? 256 : 0; return pid;
}
static int fake_kill(pid_t pid, int signal_number) {
    assert(pid == 12345 && signal_number == SIGKILL); ++killed; return 0;
}
static int fake_clock(clockid_t clock, struct timespec *time) {
    assert(clock == CLOCK_MONOTONIC); time->tv_sec = ticks++ * 4; time->tv_nsec = 0; return 0;
}
static int fake_sleep(const struct timespec *requested, struct timespec *remaining) {
    (void)requested; (void)remaining; return 0;
}
#define posix_spawn fake_spawn
#define waitpid fake_wait
#define kill fake_kill
#define clock_gettime fake_clock
#define nanosleep fake_sleep
#include "nero/diagnostic_process.h"
int main(void) {
    const char *invalid[] = {NULL, "", "127.0.0.1;echo inert", "$(echo inert)", "-c 9",
        "192.0.2.1\n", "999.0.0.1", "192.0.2.1 extra", "::1", "1.2.3", "01.2.3.4"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(nero_ping_ipv4(invalid[i], &cancelled) == 0);
    assert(spawns == 0);
    cancelled = 1; assert(nero_ping_ipv4("192.0.2.1", &cancelled) == 0); assert(!spawns);
    for (mode = 0; mode <= 6; ++mode) {
        cancelled = 0; killed = waits = ticks = spawns = 0;
        int result = nero_ping_ipv4("192.0.2.1", &cancelled);
        assert(result == (mode == 0));
        assert(spawns == 1);
        assert(killed == (mode == 2 || mode == 3 || mode == 5 || mode == 6));
        if (mode == 1) assert(waits == 0);
        if (killed) assert(waits >= 2);
    }
    puts("PASS bounded diagnostic process: fixed argv/environment, invalid inputs, spawn failure, timeout, cancellation and child reaping; no processes spawned");
    return 0;
}
