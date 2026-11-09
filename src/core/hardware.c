/* Read-only local hardware probing and the deterministic offline workload
 * recommendation. No network I/O, no subprocesses, no privilege changes and
 * no telemetry; missing values are reported as unavailable, never guessed. */
#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include "nero/hardware.h"
#include "nero/platform.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NERO_HARDWARE_TEST_HOOKS
/* Fixture override installed by offline tests; cleared for production runs. */
static nero_hardware_info test_override;
static int test_override_active;
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>

static long sysctl_long(const char *name)
{
    long value = 0;
    size_t length = sizeof(value);
    if (sysctlbyname(name, &value, &length, NULL, 0) != 0 ||
        length != sizeof(value))
        return NERO_HARDWARE_UNAVAILABLE;
    return value;
}

static int probe_posix(nero_hardware_info *info)
{
    info->cpu_logical = sysctl_long("hw.logicalcpu");
    info->cpu_physical = sysctl_long("hw.physicalcpu");
    unsigned long long bytes = 0;
    size_t length = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &length, NULL, 0) == 0 &&
        length == sizeof(bytes))
        info->ram_total_bytes = bytes;
    /* macOS offers no unprivileged, read-only API for genuinely available
     * memory; report it as unavailable rather than deriving a rough guess
     * from Mach paging statistics. */
    info->ram_available_bytes = NERO_HARDWARE_UNAVAILABLE;
    return 1;
}

#elif defined(__linux__)

/* /proc/meminfo is read-only and always present on Linux; parsing it avoids
 * the non-POSIX sysconf(_SC_PHYS_PAGES) extension so the strict offline
 * build flags keep working. */
static int meminfo_field(const char *label, unsigned long long *out)
{
    FILE *file = fopen("/proc/meminfo", "r");
    if (file == NULL)
        return -1;
    char line[128];
    int found = -1;
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t label_length = strlen(label);
        if (strncmp(line, label, label_length) != 0)
            continue;
        unsigned long long kilobytes = 0;
        if (sscanf(line + label_length, ": %llu", &kilobytes) == 1) {
            *out = kilobytes * 1024ULL;
            found = 0;
        }
        break;
    }
    fclose(file);
    return found;
}

static int probe_posix(nero_hardware_info *info)
{
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0)
        info->cpu_logical = cores;
    unsigned long long bytes = 0;
    if (meminfo_field("MemTotal", &bytes) == 0)
        info->ram_total_bytes = bytes;
    bytes = 0;
    if (meminfo_field("MemAvailable", &bytes) == 0)
        info->ram_available_bytes = bytes;
    /* /proc/cpuinfo does not expose a reliable physical-core count across
     * architectures and container quotas; report it as unavailable. */
    info->cpu_physical = NERO_HARDWARE_UNAVAILABLE;
    return 1;
}

#endif

#ifdef _WIN32
#include <windows.h>

static int probe_windows(nero_hardware_info *info)
{
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    if (system.dwNumberOfProcessors > 0 &&
        system.dwNumberOfProcessors <= (DWORD)LONG_MAX)
        info->cpu_logical = (long)system.dwNumberOfProcessors;
    /* The logical processor count is what the scheduler exposes; deriving
     * physical cores needs per-CPU topology queries, so leave it out. */
    info->cpu_physical = NERO_HARDWARE_UNAVAILABLE;

    MEMORYSTATUSEX status;
    memset(&status, 0, sizeof(status));
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        info->ram_total_bytes = status.ullTotalPhys;
    return 1;
}
#endif

int nero_hardware_probe(nero_hardware_info *info)
{
    if (info == NULL)
        return 0;
    memset(info, 0, sizeof(*info));
    const nero_platform_info *platform = nero_get_platform_info();
    info->os = platform->os;
    info->architecture = platform->architecture;
    info->cpu_logical = NERO_HARDWARE_UNAVAILABLE;
    info->cpu_physical = NERO_HARDWARE_UNAVAILABLE;

#ifdef NERO_HARDWARE_TEST_HOOKS
    if (test_override_active) {
        const nero_hardware_info *fixture = &test_override;
        nero_hardware_info copy = *fixture;
        /* Platform identity stays real; fixtures override CPU/RAM only. */
        copy.os = info->os;
        copy.architecture = info->architecture;
        *info = copy;
        return copy.cpu_logical > 0 || copy.ram_total_bytes != 0;
    }
#endif

#if defined(_WIN32)
    probe_windows(info);
#elif defined(__APPLE__) || defined(__linux__)
    probe_posix(info);
#else
    /* Unsupported platform: keep every field unavailable. */
    return 0;
#endif

    return info->cpu_logical > 0 || info->ram_total_bytes != 0;
}

nero_workload_profile nero_workload_recommend(
    const nero_hardware_info *info, char *reason, size_t reason_size)
{
    nero_workload_profile profile = NERO_WORKLOAD_PROFILE_NONE;

    if (reason != NULL && reason_size > 0)
        reason[0] = '\0';

    if (info == NULL) {
        if (reason != NULL && reason_size > 0)
            snprintf(reason, reason_size,
                     "Hardware probing failed; no recommendation offered");
        return NERO_WORKLOAD_PROFILE_NONE;
    }

    /* Unavailable or nonsensical inputs (probing failure, negative/overflow
     * counts) yield no recommendation instead of a fabricated guess. */
    if (info->cpu_logical <= 0 || info->ram_total_bytes == 0) {
        if (reason != NULL && reason_size > 0)
            snprintf(reason, reason_size,
                     "CPU or memory information unavailable; "
                     "choose a profile manually");
        return NERO_WORKLOAD_PROFILE_NONE;
    }

    if (info->cpu_logical <= 2 || info->ram_total_bytes <= 4ULL * 1024 * 1024 * 1024) {
        profile = NERO_WORKLOAD_PROFILE_LOW;
        if (reason != NULL && reason_size > 0)
            snprintf(reason, reason_size,
                     "Detected %ld logical core(s) and limited memory: "
                     "conservative simulated workload",
                     info->cpu_logical);
    } else {
        profile = NERO_WORKLOAD_PROFILE_BALANCED;
        if (reason != NULL && reason_size > 0)
            snprintf(reason, reason_size,
                     "Estimated local CPU / memory headroom "
                     "(%ld logical cores, >= 4 GiB RAM)",
                     info->cpu_logical);
    }
    return profile;
}

int nero_sim_count_parse(const char *text, long min, long max, long *out)
{
    if (text == NULL || out == NULL || min > max)
        return -1;

    /* Reject blank input and any surrounding whitespace: the menu expects a
     * single bare number. */
    if (*text == '\0')
        return -1;
    for (const char *p = text; *p; ++p) {
        if (*p == ' ' || *p == '\t')
            return -1;
    }

    /* strtoull detects range overflow; strtoul's errno contract is weaker
     * across platforms, so parse unsigned and re-check against long. */
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0')
        return -1;

    /* Leading '+'/'-' are accepted by strtoull but are not bare numbers;
     * negative values wrap into huge unsigned ones, so reject both. */
    if (text[0] == '+' || text[0] == '-')
        return -1;
    if (value > (unsigned long long)LONG_MAX)
        return -1;
    long result = (long)value;
    if (result < min || result > max)
        return -1;

    *out = result;
    return 0;
}

#ifdef NERO_HARDWARE_TEST_HOOKS
void nero_hardware_test_set(const nero_hardware_info *info)
{
    if (info == NULL) {
        test_override_active = 0;
        return;
    }
    test_override = *info;
    test_override_active = 1;
}

void nero_hardware_test_clear(void)
{
    test_override_active = 0;
}
#endif
