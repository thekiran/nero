/* Read-only -Syn preflight screen (`preflight`). It is display-only: it never
 * creates sockets, resolves names, spawns the legacy engine or forwards
 * anything to it. The live -Syn path and the packet builders are not reachable
 * from here. Hardware probing and the workload recommendation live in
 * src/core/hardware.c so the -Syn settings prompt can share them. */
#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include "cli_internal.h"
#include "nero/hardware.h"
#include "nero/log.h"

/* The `preflight` screen describes what the live -Syn engine would attempt in
 * THIS build. The engine is compiled in only when none of the offline/safe
 * guards are set (the same guards the dispatcher uses in commands.c). When it
 * is present we pull in its public header purely for the read-only default/
 * ceiling constants NERO_SYN_THREAD_* and NERO_SYN_PACKET_SIZE_*; no engine
 * symbol is referenced, so offline binaries still link no network code. */
#if !defined(NERO_SAFE_TEST) && !defined(NERO_NETWORK_UNAVAILABLE) && \
    !defined(NERO_SYN_UNAVAILABLE)
#include "nero/syn_flood_powered.h"
#define PREFLIGHT_ENGINE_COMPILED 1
#else
#define PREFLIGHT_ENGINE_COMPILED 0
#endif

#include <signal.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Menu input capacity: bounded far below CLI_LINE_CAPACITY so a stuck
 * keyboard repeat cannot grow the line without limit. */
#define MENU_INPUT_MAX 16

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

static int menu_consume_escape_sequence(void)
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

static int menu_read_line(char *buffer, size_t capacity)
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
                if (menu_consume_escape_sequence() != 0)
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

/* ============================================================
 * READ-ONLY -Syn PREFLIGHT SCREEN (`preflight`)
 * ============================================================ */

/* Describes what the live -Syn engine in THIS build would attempt, as an
 * upper bound. It opens no sockets, sends no packets, resolves no names and
 * never starts -Syn; the only syscall it makes about privilege is a read-only
 * geteuid(). All figures are qualified as attempt ceilings, never throughput
 * and never a safe transmission rate (issue #60). */
static void preflight_print_engine(const nero_hardware_info *info)
{
    (void)info; /* Used only when the live engine is compiled in. */
#if PREFLIGHT_ENGINE_COMPILED
    char reason[256];
    const long suggested =
        nero_workload_suggested_workers(info, reason, sizeof(reason));
    const long workers = suggested > 0 ? suggested : NERO_SYN_THREAD_COUNT;
    const long descriptors = workers * 2 + 64;

    fputs("Live -Syn engine:     Compiled into this build (experimental)\n",
          stdout);
    fputs("Raw-socket mode:      Requires root or CAP_NET_RAW; "
          "unavailable without it\n", stdout);
#ifndef _WIN32
    /* Read-only privilege check: no escalation is attempted or offered. */
    printf("Running as root now:  %s        "
           "[read-only check; -Syn would fail without it]\n",
           geteuid() == 0 ? "yes" : "no");
#endif
    printf("Worker count:         run-time selectable 1-%ld (absolute safety "
           "bound);\n"
           "                      suggested max on this machine: %ld "
           "(default when available)\n",
           (long)NERO_SYN_THREAD_HARD_MAX, workers);
    printf("Packet size:          run-time selectable %ld-%ld B, default %ld B; "
           "sets the\n"
           "                      UDP datagram size (TCP SYN is 40 B "
           "header-only)\n",
           (long)NERO_SYN_PACKET_SIZE_MIN, (long)NERO_SYN_PACKET_SIZE_MAX,
           (long)NERO_SYN_PACKET_SIZE_MAX);
    fputs("Actual workers:       Not guaranteed - thread and raw-socket "
          "creation can\n"
          "                      fail; the engine reports the real count "
          "when it runs.\n", stdout);
    putchar('\n');
    fputs("If you later run -Syn, this build may attempt "
          "(upper bounds, not throughput):\n", stdout);
    printf("  - Threads:          up to the value you choose (1-%ld); suggested "
           "max %ld,\n"
           "                      subject to OS limits\n",
           (long)NERO_SYN_THREAD_HARD_MAX, workers);
    fputs("  - Raw sockets:      up to 2 per worker, each requiring "
          "privilege\n", stdout);
    printf("  - File descriptors: about %ld at the suggested worker count "
           "(%ld x 2 + 64),\n"
           "                      raised via setrlimit only when permitted\n",
           descriptors, workers);
#else
    fputs("Live -Syn engine:     Not built into this target "
          "(offline / safe build)\n", stdout);
    fputs("Raw-socket mode:      Unsupported in this build\n", stdout);
    fputs("                      No workers, sockets or packets can be "
          "created here.\n", stdout);
#endif
}

static void preflight_screen(void)
{
    nero_hardware_info info;
    nero_hardware_probe(&info);

    fputs("\nNERO - -Syn Preflight (READ-ONLY)\n", stdout);
    fputs("This screen is informational only. It opens no sockets, sends no "
          "packets,\n"
          "resolves no names, and never starts the -Syn operation. Running "
          "-Syn itself\n"
          "is unchanged by viewing this screen.\n\n", stdout);
    printf("Platform:             %s / %s\n", info.os, info.architecture);
    preflight_print_engine(&info);
    putchar('\n');
    fputs("These are upper bounds the build may attempt. They are not "
          "measured\n"
          "throughput and not a safe transmission rate.\n", stdout);
    fputs("Laboratory safety:    docs/laboratory-safety.md\n\n", stdout);

    for (;;) {
        fputs("[1] Return to shell\n[0] Cancel\n\nYour choice: ", stdout);
        fflush(stdout);

        char buffer[MENU_INPUT_MAX];
        int status = menu_read_line(buffer, sizeof(buffer));
        if (status == INPUT_EOF || status == INPUT_CANCELLED) {
            fputs("Returning to shell.\n", stdout);
            return;
        }
        if (status == INPUT_OVERFLOW) {
            fputs("Input too long; enter 1 or 0.\n", stdout);
            continue;
        }
        if (strcmp(buffer, "0") == 0 || strcmp(buffer, "1") == 0) {
            fputs("Returning to shell.\n", stdout);
            return;
        }
        fputs("Invalid choice; enter 1 or 0.\n", stdout);
    }
}

cli_cmd_result cmd_preflight(char **tokens, size_t count)
{
    if (tokens == NULL || count != 1)
        return CLI_CMD_INVALID;

    nero_log_write(NERO_LOG_INFO, "preflight",
                   "Read-only -Syn preflight screen viewed (no network I/O)");
    preflight_screen();

    /* Ctrl+C during the screen is a Return, not a session abort; mirror the
     * scanner's handling of the shared interrupt flag. */
    if (cli_termination_signal == SIGINT)
        cli_termination_signal = 0;

    return CLI_CMD_CONTINUE;
}
