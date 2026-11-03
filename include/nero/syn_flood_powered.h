#ifndef SYN_FLOOD_H
#define SYN_FLOOD_H

/* Sentinel return value from SYN_Flood: the engine is a stub for this build
 * and did not attempt any traffic. Callers should NOT treat this as a hard
 * failure — the stub has already printed its own message. The real Linux
 * engine returns only 0 (success) or 1 (operational failure). */
#define SYN_UNAVAILABLE 2

int SYN_Flood(const char *target_ip);
void sigint_handler(int sig);

#endif

