#ifndef NERO_NETWORK_DIAGNOSTICS_H
#define NERO_NETWORK_DIAGNOSTICS_H

/* Reserved diagnostics adapter entry point. Declaration alone does not imply
 * an implementation is linked; current CLI diagnostics live in commands.c. */
int nero_diagnostic_run(const char *target_ip);

#endif
