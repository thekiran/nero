#ifndef PORTSCANNER_H
#define PORTSCANNER_H

/* Returns the selected port (>= 1), 0 when no open port was found, 1 on an
 * internal failure (partial scan or allocation error), or -1 when the user
 * aborted the selection menu with EOF/input error. */
int portscanner(const char *target_ip);

#endif
