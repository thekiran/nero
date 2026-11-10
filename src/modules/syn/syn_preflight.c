/* Read-only preflight checks for the -Syn engine.
 *
 * These checks decide whether the run may start; they never spawn workers,
 * open the flooding sockets or touch engine state. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "nero/diagnostic_process.h"

#include "syn_internal.h"

/* Probe the two raw sockets the engine needs. A permission failure here is
 * reported before any worker starts, so the caller does not block on an
 * animation thread whose workers all exit immediately. */
static int raw_sockets_available(void)
{
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (fd < 0)
        return 0;
    close(fd);
    fd = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

int nero_syn_preflight_privileges(void)
{
#if defined(__APPLE__)
    /* Raw IPv4 sockets require root on macOS. Report the requirement before
     * starting any worker so the failure is actionable instead of a per-thread
     * "Operation not permitted". */
    if (geteuid() != 0) {
        nero_syn_report_root_required();
        return 0;
    }
#endif
    if (!raw_sockets_available()) {
        nero_syn_report_raw_sockets_unavailable();
        return 0;
    }
    return 1;
}

int nero_syn_target_is_online(const char *ip,
                              const volatile sig_atomic_t *cancel)
{
    return nero_ping_ipv4(ip, cancel);
}
