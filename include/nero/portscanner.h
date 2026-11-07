#ifndef PORTSCANNER_H
#define PORTSCANNER_H

#include <signal.h>

#define PORTSCANNER_FAILED (-1)
#define PORTSCANNER_CANCELLED (-2)

/* Set by the production CLI's signal handler. Kept separate from the
 * thread-owned cancel flag so signal delivery remains async-signal-safe. */
extern volatile sig_atomic_t portscanner_interrupt_requested;

/* Returns the selected port (>= 1), 0 when no open port was found,
 * PORTSCANNER_CANCELLED when the user cancels, or PORTSCANNER_FAILED on an
 * input/internal failure or when no open port is available and local/network
 * errors leave checks inconclusive. Timeouts are filtered, never closed.
 * Every completed full scan accounts for all 65535 TCP ports; cancelled scans
 * report completed outcomes and unscanned ports separately. Host discovery is
 * advisory (unknown is never treated as offline). Optional ICMP discovery can
 * be enabled with NERO_SCAN_ICMP=1; failure never prevents TCP scanning. */
int portscanner(const char *target_ip);

#endif
