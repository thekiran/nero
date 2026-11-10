/* User-facing status and error messages for the -Syn engine.
 *
 * Centralising the wording keeps the engine lifecycle readable and makes every
 * message free of engine state: callers pass only the values to format. Each
 * terminal message is mirrored into the existing operational log so a
 * non-interactive run can still be audited. */
#include <stdio.h>

#include "nero/log.h"

#include "syn_internal.h"

#define NERO_SYN_WARNING "\n\033[49m[!]\033[0m\033[1;31m "
#define NERO_SYN_ADVISORY "\n\033[49m[!]\033[0m\033[1;33m "
#define NERO_SYN_RESET "\033[0m\n"

void nero_syn_report_root_required(void)
{
    fprintf(stderr, NERO_SYN_WARNING
                    "Raw packet sockets require root on macOS: run with "
                    "sudo." NERO_SYN_RESET);
    nero_log_write(NERO_LOG_ERROR, "syn",
                   "raw packet sockets require root on macOS");
}

void nero_syn_report_raw_sockets_unavailable(void)
{
    fprintf(stderr, NERO_SYN_WARNING
                    "Cannot open raw IPv4 sockets; root or CAP_NET_RAW is "
                    "required." NERO_SYN_RESET);
    nero_log_write(NERO_LOG_ERROR, "syn",
                   "raw IPv4 sockets unavailable; root or CAP_NET_RAW "
                   "required");
}

void nero_syn_report_cancelled(void)
{
    fprintf(stderr,
            NERO_SYN_WARNING "Cancelled; -Syn not started." NERO_SYN_RESET);
    nero_log_write(NERO_LOG_INFO, "syn",
                   "settings cancelled; engine not started");
}

void nero_syn_report_target_unreachable(const char *ip)
{
    fprintf(stderr, NERO_SYN_ADVISORY
                    "Target %s did not answer ping; continuing anyway."
                        NERO_SYN_RESET,
            ip);
    nero_log_write(NERO_LOG_WARN, "syn",
                   "target %s did not answer ping; continuing", ip);
}

void nero_syn_report_thread_alloc_failed(long requested)
{
    fprintf(stderr, "Failed to allocate handles for %ld worker threads\n",
            requested);
    nero_log_write(NERO_LOG_ERROR, "syn",
                   "failed to allocate %ld worker handles", requested);
}

void nero_syn_report_animation_failed(void)
{
    fprintf(stderr, "Failed to create animation thread\n");
    nero_log_write(NERO_LOG_ERROR, "syn", "failed to create animation thread");
}

void nero_syn_report_thread_start_failed(int index)
{
    fprintf(stderr, "Failed to create thread %d\n", index);
    nero_log_write(NERO_LOG_ERROR, "syn", "failed to create thread %d",
                   index);
}

void nero_syn_report_incomplete(int created, long requested)
{
    fprintf(stderr, "Failed to start all flood threads (%d of %ld)\n", created,
            requested);
    nero_log_write(NERO_LOG_ERROR, "syn",
                   "started %d of %ld flood threads", created, requested);
}

void nero_syn_report_stopped(void)
{
    printf("[-]Thread\n");
}

void nero_syn_report_worker_summary(const void *worker, unsigned long tcp_sent,
                                    unsigned long udp_sent,
                                    unsigned long tcp_errors,
                                    unsigned long udp_errors)
{
    fprintf(stderr,
            "[worker %p] sent tcp=%lu udp=%lu | errors tcp=%lu udp=%lu\n",
            worker, tcp_sent, udp_sent, tcp_errors, udp_errors);
    nero_log_write(NERO_LOG_DEBUG, "syn",
                   "worker %p sent tcp=%lu udp=%lu errors tcp=%lu udp=%lu",
                   worker, tcp_sent, udp_sent, tcp_errors, udp_errors);
}

void nero_syn_report_port_aborted(void)
{
    fprintf(stderr,
            NERO_SYN_WARNING "Port selection aborted." NERO_SYN_RESET);
    nero_log_write(NERO_LOG_WARN, "syn", "port selection aborted");
}

void nero_syn_report_no_open_port(const char *ip)
{
    fprintf(stderr,
            NERO_SYN_WARNING
            "No open port found on %s; flooding default port 80."
                NERO_SYN_RESET,
            ip);
    nero_log_write(NERO_LOG_WARN, "syn",
                   "no open port on %s; using default port 80", ip);
}
