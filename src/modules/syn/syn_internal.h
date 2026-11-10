#ifndef NERO_SYN_INTERNAL_H
#define NERO_SYN_INTERNAL_H

/* Private contracts shared by the -Syn engine and its support modules.
 *
 * These declarations are not part of the public CLI boundary. The engine
 * keeps ownership of the run state (stop flag, port, worker count); helper
 * modules receive it through arguments and never introduce new globals. */
#include <signal.h>

/* Settings: validates operator input and resolves the hardware suggestion.
 * Writes both outputs on success; returns -1 when the operator cancels. */
int nero_syn_prompt_settings(long *thread_count, long *packet_size);

/* Preflight: read-only privilege and target checks. Both print through the
 * report module and never modify engine state. */
int nero_syn_preflight_privileges(void);
int nero_syn_target_is_online(const char *ip,
                              const volatile sig_atomic_t *cancel);

/* UI: renders the run animation. The view borrows the engine-owned state for
 * the lifetime of the thread and publishes the cursor row used by the
 * interrupt handler. */
typedef struct {
    const char *target_ip;
    int port;
    long thread_count;
    const volatile sig_atomic_t *stop_flag;
    volatile sig_atomic_t *row;
} nero_syn_animation_view;

void *nero_syn_animation_main(void *view);

/* Report: user-facing status and error messages. No engine state is read. */
void nero_syn_report_root_required(void);
void nero_syn_report_raw_sockets_unavailable(void);
void nero_syn_report_cancelled(void);
void nero_syn_report_target_unreachable(const char *ip);
void nero_syn_report_thread_alloc_failed(long requested);
void nero_syn_report_animation_failed(void);
void nero_syn_report_thread_start_failed(int index);
void nero_syn_report_incomplete(int created, long requested);
void nero_syn_report_stopped(void);
void nero_syn_report_worker_summary(const void *worker, unsigned long tcp_sent,
                                    unsigned long udp_sent,
                                    unsigned long tcp_errors,
                                    unsigned long udp_errors);
void nero_syn_report_port_aborted(void);
void nero_syn_report_no_open_port(const char *ip);

#endif
