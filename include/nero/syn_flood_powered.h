#ifndef SYN_FLOOD_H
#define SYN_FLOOD_H

/* Fallback default and historical tuned value for the engine's run-time
 * worker-count prompt in src/modules/syn_flood_powered.c. The prompt suggests
 * a hardware-derived value from the local CPU/RAM probe and uses this value
 * only when that probe cannot recommend one. This is NOT a ceiling: the
 * operator may enter any count up to NERO_SYN_THREAD_HARD_MAX. The actual
 * number of workers started is never guaranteed — thread and raw-socket
 * creation can fail, so the engine reports the real count at run time. */
#define NERO_SYN_THREAD_COUNT 512

/* Absolute safety bound for the run-time worker-count prompt and for the
 * per-run worker array the engine allocates. It is deliberately far above the
 * hardware suggestion; it only keeps input and allocation finite. */
#define NERO_SYN_THREAD_HARD_MAX 65535

/* Bounds of the run-time packet-size prompt (total IPv4 datagram bytes on
 * the wire for the UDP leg). The minimum is 20-byte IP header + 8-byte UDP
 * header + 32-byte minimum payload; the maximum is the engine's datagram
 * buffer. The TCP leg is always a 40-byte header-only SYN and is not
 * affected by this setting. */
#define NERO_SYN_PACKET_SIZE_MIN 60
#define NERO_SYN_PACKET_SIZE_MAX 4096

/* Legacy engine boundary; offline tests may substitute a mock implementation.
 * The dispatcher validates IPv4 input before transferring control here. */
int SYN_Flood(const char *target_ip);
/* Engine-owned interrupt handler; installed as part of its signal lifecycle. */
void sigint_handler(int sig);

#endif
