/* Offline workload-profile screen (`sim`) and the read-only -Syn preflight
 * (`preflight`). Both are display-only: they never create sockets, resolve
 * names, spawn the legacy engine or forward anything to it. The live -Syn
 * path, THREAD_COUNT and the packet builders are not reachable from here. */
#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include "cli_internal.h"
#include "nero/hardware.h"
#include "nero/log.h"

#include <signal.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Menu input capacity: bounded far below CLI_LINE_CAPACITY so a stuck
 * keyboard repeat cannot grow the line without limit. */
#define SIM_INPUT_MAX 16

/* One simulated-run animation step, POSIX only; the offline screen renders
 * instantly where no portable bounded sleep exists. */
#ifdef _WIN32
#define SIM_ANIMATE 0
#else
#define SIM_ANIMATE 1
#define SIM_STEP_MS 100
#define SIM_STEP_COUNT 10
#endif

/* ============================================================
 * BOUNDED CONSOLE INPUT
 * ============================================================ */

/* Raw-mode line reader with local echo. The terminal session keeps ICANON
 * and ECHO off, so this module owns the echo, backspace and the short
 * Escape-continuation window that separates a standalone Cancel from an
 * arrow-key sequence (same convention as nero_operation_read_key).
 *
 * Returns the non-negative length on success, INPUT_CANCELLED when the user
 * pressed Esc/q, and INPUT_EOF when stdin closed. INPUT_OVERFLOW marks a
 * line that exceeded the bounded capacity (fully drained, never echoed). */
#define INPUT_CANCELLED (-1)
#define INPUT_EOF (-2)
#define INPUT_OVERFLOW (-3)

static int sim_consume_escape_sequence(void)
{
    /* Drain a bracketed control sequence up to its final byte so Cancel and
     * arrow keys stay distinguishable; bounded like the operation decoder. */
    char byte;
    for (int index = 0; index < 32; ++index) {
        ssize_t status = read(STDIN_FILENO, &byte, 1);
        if (status <= 0)
            return -1;
        if ((unsigned char)byte >= 0x40 && (unsigned char)byte <= 0x7e)
            return 0;
    }
    return -1;
}

static int sim_read_line(char *buffer, size_t capacity)
{
    size_t length = 0;
    int overflow = 0;

    /* Terminate up front so every early return hands back an empty C string
     * instead of leftover bytes from a previous call's stack buffer. */
    if (capacity > 0)
        buffer[0] = '\0';

    for (;;) {
        char byte;
        ssize_t status = read(STDIN_FILENO, &byte, 1);
        if (status == 0)
            return INPUT_EOF;
        if (status < 0)
            return INPUT_CANCELLED;

        if (byte == '\n' || byte == '\r') {
            putchar('\n');
            fflush(stdout);
            /* The overflow guard keeps length <= capacity - 2, so the
             * terminator always fits; callers read buffer as a C string. */
            buffer[length] = '\0';
            return overflow ? INPUT_OVERFLOW : (int)length;
        }
        if (byte == 27) {
            /* A lone Escape cancels; a prefixed sequence is consumed. */
            struct pollfd input = {STDIN_FILENO, POLLIN, 0};
            int ready = poll(&input, 1, 35);
            if (ready > 0 && (input.revents & POLLIN) != 0) {
                if (sim_consume_escape_sequence() != 0)
                    return INPUT_CANCELLED;
                continue;
            }
            return INPUT_CANCELLED;
        }
        if (byte == 'q' || byte == 'Q' || byte == 3)
            return INPUT_CANCELLED;
        if (byte == 127 || byte == '\b') {
            if (length > 0) {
                --length;
                fputs("\b \b", stdout);
                fflush(stdout);
            }
            continue;
        }
        if (byte < 32 || byte > 126)
            continue; /* Ignore other control bytes; never echo them. */

        if (length + 1 >= capacity) {
            overflow = 1;
            continue;
        }
        buffer[length++] = byte;
        putchar(byte);
        fflush(stdout);
    }
}

/* Cross-platform bounded delay. POSIX poll sleeps; the Windows shim returns
 * immediately for an empty set, so the animation simply renders instantly. */
static void sim_delay_ms(int milliseconds)
{
    poll(NULL, 0, milliseconds);
}

/* ============================================================
 * OFFLINE SIMULATION SCREEN (`sim`)
 * ============================================================ */

static void sim_format_gib(char *buffer, size_t capacity,
                           unsigned long long bytes)
{
    if (bytes == NERO_HARDWARE_UNAVAILABLE) {
        snprintf(buffer, capacity, "Unavailable");
        return;
    }
    const unsigned long long gib = 1024ULL * 1024 * 1024;
    snprintf(buffer, capacity, "%llu.%llu GiB",
             bytes / gib, (bytes % gib) * 10 / gib);
}

static void sim_print_hardware(const nero_hardware_info *info)
{
    char total[32];
    char available[32];
    sim_format_gib(total, sizeof(total), info->ram_total_bytes);
    sim_format_gib(available, sizeof(available), info->ram_available_bytes);

    printf("CPU:      ");
    if (info->cpu_logical > 0)
        printf("%ld logical processor%s", info->cpu_logical,
               info->cpu_logical == 1 ? "" : "s");
    else
        fputs("Unavailable", stdout);
    if (info->cpu_physical > 0)
        printf(" (%ld physical)", info->cpu_physical);
    putchar('\n');

    printf("RAM:      %s total", total);
    if (info->ram_available_bytes != NERO_HARDWARE_UNAVAILABLE)
        printf(" (%s available)", available);
    putchar('\n');
    printf("Platform: %s / %s\n", info->os, info->architecture);
    fputs("Note:     logical cores differ from physical cores; container or\n"
          "          virtualization quotas can make machine totals misleading.\n",
          stdout);
}

static const char *sim_profile_name(nero_workload_profile profile)
{
    switch (profile) {
    case NERO_WORKLOAD_PROFILE_LOW:
        return "Low";
    case NERO_WORKLOAD_PROFILE_BALANCED:
        return "Balanced";
    default:
        return NULL; /* NONE: probing failed, no recommendation exists. */
    }
}

/* Documented simulation-only presets. These numbers are display data for the
 * offline screen; nothing forwards them to the live engine. */
#define SIM_PROFILE_LOW_WORKERS 64
#define SIM_PROFILE_BALANCED_WORKERS 256
#define SIM_PROFILE_SECONDS 10

static void sim_print_recommendation(const nero_hardware_info *info)
{
    char reason[256];
    nero_workload_profile profile =
        nero_workload_recommend(info, reason, sizeof(reason));
    const char *name = sim_profile_name(profile);

    if (name == NULL) {
        fputs("Recommended Profile: Unavailable\n", stdout);
    } else {
        printf("Recommended Profile: %s\n", name);
    }
    printf("Reason:   %s\n", reason);
}

/* One row of the menu; recommended marks the suggested option only. */
static void sim_print_menu(nero_workload_profile recommendation)
{
    fputs("[1] Low", stdout);
    if (recommendation == NERO_WORKLOAD_PROFILE_LOW)
        fputs(" (Recommended)", stdout);
    putchar('\n');
    fputs("[2] Balanced", stdout);
    if (recommendation == NERO_WORKLOAD_PROFILE_BALANCED)
        fputs(" (Recommended)", stdout);
    putchar('\n');
    fputs("[3] Custom\n", stdout);
    fputs("[0] Cancel\n", stdout);
    fputs("\nYour choice: ", stdout);
    fflush(stdout);
}

static void sim_print_summary(long workers, long seconds)
{
    printf("Selected profile: SIMULATION ONLY\n");
    printf("Simulated workers: %ld\n", workers);
    printf("Simulated duration: %ld s\n", seconds);
    nero_log_write(NERO_LOG_INFO, "sim",
                   "Offline simulation profile selected: workers=%ld seconds=%ld",
                   workers, seconds);

    /* Display-only simulated run: a bounded animation of the recorded
     * preset. No computation is performed, nothing is transmitted. */
#if SIM_ANIMATE
    fputs("Simulating (offline, display only):\n  [", stdout);
    fflush(stdout);
    for (int step = 0; step < SIM_STEP_COUNT; ++step) {
        putchar('#');
        fflush(stdout);
        sim_delay_ms(SIM_STEP_MS);
    }
    fputs("] done\n", stdout);
#endif
    fputs("Packets generated: 0\n", stdout);
    fputs("Sockets opened: 0\n", stdout);
    fputs("This affects the offline display only; the live -Syn engine is "
          "unchanged.\n", stdout);
}

static int sim_prompt_count(const char *prompt, long min, long max, long *out)
{
    char buffer[SIM_INPUT_MAX];

    for (;;) {
        printf("%s (%ld-%ld): ", prompt, min, max);
        fflush(stdout);
        int status = sim_read_line(buffer, sizeof(buffer));
        if (status == INPUT_EOF || status == INPUT_CANCELLED)
            return -1;
        if (status == INPUT_OVERFLOW) {
            fputs("Input too long.\n", stdout);
            continue;
        }
        long value = 0;
        if (nero_sim_count_parse(buffer, min, max, &value) != 0) {
            fputs("Invalid value; enter a whole number without signs, "
                  "spaces or extra characters.\n", stdout);
            continue;
        }
        *out = value;
        return 0;
    }
}

static void sim_screen(void)
{
    nero_hardware_info info;
    int probed = nero_hardware_probe(&info);
    nero_workload_profile recommendation =
        nero_workload_recommend(&info, NULL, 0);

    /* Hardware facts are read once per screen entry; probing failure is
     * displayed explicitly and still offers a manual selection (#62). */
    (void)probed;
    fputs("\nNERO - Hardware Analysis (OFFLINE SIMULATION)\n", stdout);
    fputs("No network activity is generated by this screen.\n\n", stdout);
    sim_print_hardware(&info);
    putchar('\n');
    sim_print_recommendation(&info);
    putchar('\n');

    for (;;) {
        sim_print_menu(recommendation);

        char buffer[SIM_INPUT_MAX];
        int status = sim_read_line(buffer, sizeof(buffer));
        if (status == INPUT_EOF || status == INPUT_CANCELLED) {
            fputs("Cancelled. No profile selected.\n", stdout);
            return;
        }
        if (status == INPUT_OVERFLOW) {
            fputs("Input too long; enter 1, 2, 3 or 0.\n", stdout);
            continue;
        }

        if (strcmp(buffer, "0") == 0) {
            fputs("Cancelled. No profile selected.\n", stdout);
            return;
        }
        if (strcmp(buffer, "1") == 0) {
            sim_print_summary(SIM_PROFILE_LOW_WORKERS, SIM_PROFILE_SECONDS);
            continue;
        }
        if (strcmp(buffer, "2") == 0) {
            sim_print_summary(SIM_PROFILE_BALANCED_WORKERS,
                              SIM_PROFILE_SECONDS);
            continue;
        }
        if (strcmp(buffer, "3") == 0) {
            long workers = 0;
            long seconds = 0;
            if (sim_prompt_count("Simulated worker count",
                                 NERO_SIM_WORKERS_MIN,
                                 NERO_SIM_WORKERS_MAX, &workers) == 0 &&
                sim_prompt_count("Simulated duration seconds",
                                 NERO_SIM_SECONDS_MIN,
                                 NERO_SIM_SECONDS_MAX, &seconds) == 0) {
                sim_print_summary(workers, seconds);
            } else {
                fputs("Custom entry cancelled.\n", stdout);
            }
            continue;
        }
        fputs("Invalid choice; enter 1, 2, 3 or 0.\n", stdout);
    }
}

cli_cmd_result cmd_sim(char **tokens, size_t count)
{
    if (tokens == NULL || count != 1)
        return CLI_CMD_INVALID;

    sim_screen();

    /* Ctrl+C during the menu is a Cancel, not a session abort: the scanner
     * commands reset the same session flag after consuming the interrupt. */
    if (cli_termination_signal == SIGINT)
        cli_termination_signal = 0;

    return CLI_CMD_CONTINUE;
}
