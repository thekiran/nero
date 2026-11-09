#ifndef NERO_DOCTOR_INTERNAL_H
#define NERO_DOCTOR_INTERNAL_H
#include <stdio.h>
typedef enum { DOCTOR_UNKNOWN, DOCTOR_MACOS, DOCTOR_LINUX, DOCTOR_WINDOWS } doctor_os;
typedef struct {
    doctor_os os;
    int architecture; /* 0 unknown, 1 ARM64, 2 x86_64 */
    int executable; /* 0 unavailable, 1 standard system path, 2 private path */
    int path; /* -1 unavailable/over-limit, 0 missing, 1 same executable, 2 other */
    int terminal; /* 0 redirected, 1 plain, 2 color-capable */
    int config; /* -1 unavailable, 0 absent/not directory, 1 directory exists */
    int privilege; /* -1 unverified, 0 unprivileged, 1 elevated */
    int scanner, raw_engine;
} doctor_snapshot;
void doctor_collect(doctor_snapshot *snapshot);
int doctor_render(FILE *out, const doctor_snapshot *snapshot);
#endif
