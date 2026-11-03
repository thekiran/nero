#ifndef SYN_FLOOD_H
#define SYN_FLOOD_H

/* Legacy engine boundary; offline tests may substitute a mock implementation.
 * The dispatcher validates IPv4 input before transferring control here. */
int SYN_Flood(const char *target_ip);
/* Engine-owned interrupt handler; installed as part of its signal lifecycle. */
void sigint_handler(int sig);

#endif

