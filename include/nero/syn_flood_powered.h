#ifndef SYN_FLOOD_H
#define SYN_FLOOD_H

/* Fixed, compile-time worker count the experimental engine would attempt to
 * start (its internal THREAD_COUNT in src/modules/syn_flood_powered.c). Keep
 * this mirror in sync with that definition. It is a read-only description for
 * the `preflight` screen, not a configuration knob: nothing reads it back into
 * the engine, and the actual number of workers started is never guaranteed —
 * thread and raw-socket creation can fail, so the engine reports the real
 * count at run time. */
#define NERO_SYN_THREAD_COUNT 512

/* Legacy engine boundary; offline tests may substitute a mock implementation.
 * The dispatcher validates IPv4 input before transferring control here. */
int SYN_Flood(const char *target_ip);
/* Engine-owned interrupt handler; installed as part of its signal lifecycle. */
void sigint_handler(int sig);

#endif

