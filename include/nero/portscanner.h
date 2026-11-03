#ifndef PORTSCANNER_H
#define PORTSCANNER_H

/* Returns the selected port (>= 1), 0 when no open port was found, or -1 on
 * any failure (user abort, EOF, read error, allocation failure, or partial
 * scan). The single negative sentinel keeps port 1 unambiguous as a valid
 * selection. */
int portscanner(const char *target_ip);

#endif
