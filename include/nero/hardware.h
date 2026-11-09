#ifndef NERO_HARDWARE_H
#define NERO_HARDWARE_H

/* Read-only local resource probing and the deterministic workload
 * recommendation used by the live `-Syn` run-time settings prompt.
 *
 * Scope contract (issue #62): this module reads basic CPU/RAM facts with
 * supported native APIs, never guesses missing values, and never touches the
 * legacy packet engine, portscanner() or scan_session.c. The recommendation
 * is informational only; the operator always makes the final choice and
 * nothing is auto-started. */

#include <stddef.h>

/* Unavailable sentinel values: a probe that cannot measure a field keeps the
 * sentinel instead of substituting an estimate. */
#define NERO_HARDWARE_UNAVAILABLE 0

typedef struct nero_hardware_info {
    /* Static strings owned by nero_get_platform_info(); never NULL. */
    const char *os;
    const char *architecture;
    /* Logical and physical processor counts; <= 0 means unavailable. Logical
     * cores differ from physical cores (SMT) and container/virtualization
     * quotas can make host totals misleading; callers must present these
     * limitations rather than silently trusting one number. */
    long cpu_logical;
    long cpu_physical;
    /* Byte counts; 0 means unavailable. Available memory may differ from
     * total and is optional per platform. */
    unsigned long long ram_total_bytes;
    unsigned long long ram_available_bytes;
} nero_hardware_info;

/* Populates *info from read-only OS APIs (POSIX sysconf/sysctl where
 * available; Windows GetSystemInfo / GlobalMemoryStatusEx). No shelling out,
 * sockets, privilege changes or telemetry. os/architecture are set even when
 * CPU/RAM probing fails.
 *
 * Returns 1 when at least one CPU or RAM value was measured, 0 when the probe
 * produced no usable hardware data (info is still fully populated). Never
 * fabricates values. */
int nero_hardware_probe(nero_hardware_info *info);

typedef enum {
    NERO_WORKLOAD_PROFILE_NONE = 0, /* probing failed: no suggestion offered */
    NERO_WORKLOAD_PROFILE_LOW,
    NERO_WORKLOAD_PROFILE_BALANCED
} nero_workload_profile;

/* Documented, deterministic recommendation rule:
 *   - CPU or total RAM unavailable            -> NONE (never a guess)
 *   - cpu_logical <= 2 or RAM <= 4 GiB        -> LOW
 *   - otherwise                               -> BALANCED
 * reason (when reason_size > 0) is always filled with a plain-language
 * explanation, including the explicit "unavailable" case. The result is a
 * conservative worker-count suggestion, not a network transmission rate. */
nero_workload_profile nero_workload_recommend(
    const nero_hardware_info *info, char *reason, size_t reason_size);

/* Conservative worker ceilings for the two documented profiles. They are
 * starting points shown by the `-Syn` settings prompt, not hard limits: the
 * operator may still choose any value the engine accepts. */
#define NERO_WORKLOAD_LOW_WORKERS 64
#define NERO_WORKLOAD_BALANCED_WORKERS 256

/* Maps nero_workload_recommend() to the profile's suggested worker ceiling.
 * Returns 0 when no recommendation can be made (probe unavailable), in which
 * case the caller supplies its own fallback. `reason`, when non-NULL, receives
 * the same plain-language explanation as nero_workload_recommend(). */
long nero_workload_suggested_workers(
    const nero_hardware_info *info, char *reason, size_t reason_size);

/* Strict parser for bounded run-time counts (worker count, packet size).
 *
 * Accepts only a whole decimal number in [min, max] with no surrounding
 * characters. Rejects empty/blank input, signs ("+5", "-1"), non-numeric
 * text ("5x", "abc"), digit junk after the number ("5 "), and values that
 * overflow long ("999999999999999999999") or fall outside [min, max].
 *
 * Returns 0 and stores the parsed value in *out on success; returns -1
 * without touching *out otherwise. Input may not be NULL. */
int nero_count_parse(const char *text, long min, long max, long *out);

#ifdef NERO_HARDWARE_TEST_HOOKS
/* Deterministic offline fixtures (tests/workload_profile.c) override the
 * probe result instead of depending on the build machine's hardware.
 * Release builds never define the hooks. */
void nero_hardware_test_set(const nero_hardware_info *info);
void nero_hardware_test_clear(void);
#endif

#endif
