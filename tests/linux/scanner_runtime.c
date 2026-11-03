/* Actual scanner worker, restricted to loopback and pty fixtures.
 * Also covers the port menu lifecycle (EOF -> -1, valid selection, termios
 * restore) and partial pthread_create failure propagation. */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Fault injection for the partial thread-creation regression: once armed,
 * the Nth and later create calls inside portscanner.c fail with EAGAIN. */
static int create_fail_at = -1;
static int create_count = 0;
static int injected_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                   void *(*fn)(void *), void *arg)
{
    if (create_fail_at >= 0 && create_count++ >= create_fail_at)
        return EAGAIN;
    return pthread_create(thread, attr, fn, arg);
}
#define pthread_create injected_pthread_create
#include "../../src/modules/portscanner.c"
#undef pthread_create

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

static int descriptors(void) {
    DIR *d = opendir("/proc/self/fd");
    assert(d);
    int n = 0;
    while (readdir(d)) ++n;
    closedir(d);
    return n;
}

static void test_worker_loopback(int server, struct sockaddr_in addr) {
    target_ip_global = "127.0.0.1";
    thread_args args = {ntohs(addr.sin_port), ntohs(addr.sin_port)};
    for (int i = 0; i < 10; ++i) {
        pthread_t worker;
        assert(pthread_create(&worker, NULL, scan_ports_thread, &args) == 0);
        assert(pthread_join(worker, NULL) == 0);
        assert(port_counter == i + 1);
        int peer = accept(server, NULL, NULL);
        assert(peer >= 0);
        char byte;
        assert(recv(peer, &byte, 1, 0) == 0);
        close(peer);
    }
    close(server);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, scan_ports_thread, &args) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(port_counter == 10);
    target_ip_global = "invalid";
    assert(pthread_create(&worker, NULL, scan_ports_thread, &args) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(port_counter == 10);
    free(open_ports_list); open_ports_list = NULL;
}

static void test_menu_eof_pipe(void) {
    int fds[2];
    assert(pipe(fds) == 0);
    close(fds[1]);
    assert(dup2(fds[0], STDIN_FILENO) >= 0);
    close(fds[0]);
    int single[] = {22};
    /* EOF before any selection must be -1, never a guessed "first port". */
    assert(select_port_menu(single, 1) == -1);
    puts("PASS menu EOF on pipe -> -1 (no selection guessed)");
}

static void test_menu_valid_and_termios_restore(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    assert(master >= 0);
    assert(grantpt(master) == 0);
    assert(unlockpt(master) == 0);
    int slave = open(ptsname(master), O_RDWR);
    assert(slave >= 0);
    /* Zero-init so memcmp never compares indeterminate struct padding. */
    struct termios before = {0};
    struct termios after = {0};
    assert(tcgetattr(slave, &before) == 0);
    assert(dup2(slave, STDIN_FILENO) >= 0);

    int unsorted[] = {443, 80};
    assert(write(master, "\n", 1) == 1);
    assert(select_port_menu(unsorted, 2) == 80); /* sorted, first highlighted */

    assert(write(master, "\x1b[B\n", 4) == 4);
    assert(select_port_menu(unsorted, 2) == 443); /* one Down then Enter */

    assert(tcgetattr(slave, &after) == 0);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    close(master);
    close(slave);
    puts("PASS menu Enter/arrow on pty -> expected ports, slave termios restored");
}

static void test_partial_thread_create_fails(void) {
    create_count = 0;
    create_fail_at = 3;
    int rc = portscanner("127.0.0.1"); /* loopback only; 3 workers then EAGAIN */
    create_fail_at = -1;
    assert(rc == 1);
    assert(open_ports_list == NULL);
    assert(port_counter == 0);
    puts("PASS partial pthread_create failure -> portscanner returns 1, state cleaned");
}

int main(void) {
    int baseline = descriptors();
    int server = socket(AF_INET, SOCK_STREAM, 0);
    assert(server >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(server, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(server, (struct sockaddr *)&addr, &len) == 0);
    assert(listen(server, 1) == 0);
    assert(pthread_mutex_init(&ports_mutex, NULL) == 0);

    test_worker_loopback(server, addr);
    test_menu_eof_pipe();
    test_menu_valid_and_termios_restore();
    test_partial_thread_create_fails();

    assert(pthread_mutex_destroy(&ports_mutex) == 0);
    assert(descriptors() == baseline);
    puts("PASS actual scanner worker: 10 connects/EOF, closed port, invalid IPv4, FD baseline restored");
    return 0;
}
