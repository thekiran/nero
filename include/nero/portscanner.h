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
 * input/internal failure. */
int portscanner(const char *target_ip);

#endif
