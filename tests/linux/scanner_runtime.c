/* Actual scanner worker, restricted to one loopback port per invocation. */
#include "../../src/modules/portscanner.c"
#include <assert.h>
#include <dirent.h>
static int descriptors(void) {
    DIR *d = opendir("/proc/self/fd");
    assert(d);
    int n = 0;
    while (readdir(d)) ++n;
    closedir(d);
    return n;
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
    assert(pthread_mutex_destroy(&ports_mutex) == 0);
    assert(descriptors() == baseline);
    puts("PASS actual scanner worker: 10 connects/EOF, closed port, invalid IPv4, FD baseline restored");
    return 0;
}
