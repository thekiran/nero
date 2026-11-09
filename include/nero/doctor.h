#ifndef NERO_DOCTOR_H
#define NERO_DOCTOR_H
#include <stdio.h>
/* Read-only local diagnostics. Does not initialize logging, history or sockets. */
int nero_doctor(FILE *out);
#endif
