/* Run-time settings prompt for the legacy -Syn engine.
 *
 * This translation unit is intentionally free of packet-engine state: it
 * validates operator input and returns the selected worker count and packet
 * size through its output parameters. The terminal session keeps stdin in
 * raw mode, so the prompt owns echo, backspace and Escape handling. */
#include <stdio.h>
#include <unistd.h>
#include <poll.h>

#include "nero/hardware.h"
#include "nero/log.h"
#include "nero/syn_flood_powered.h"

#include "syn_internal.h"

/* Input capacity: 8 digits covers the largest selectable values (65535 worker
 * threads, 4096 bytes) yet stays bounded, so a stuck keyboard repeat cannot
 * grow the line. */
#define SYN_PROMPT_INPUT_MAX 8

/* Drain a bracketed control sequence up to its final byte so a lone Escape
 * (cancel) stays distinguishable from arrow keys (ignored); same bounded
 * convention as the reader in src/cli/workload.c. */
static int syn_prompt_consume_escape_sequence(void)
{
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

/* Raw-mode numeric prompt with local echo: the terminal session keeps
 * ICANON and ECHO off, so this function owns the echo and backspace.
 * Enter on an empty line accepts `default_value`; an entered value must be
 * a plain digit string within [min, max] (rejected input re-prompts, never
 * clamps). Returns 0 and stores the value in *out, or -1 when the operator
 * cancelled with Esc/q/Ctrl+C or stdin closed. */
static int syn_prompt_count(const char *prompt, long min, long max,
                            long default_value, long *out)
{
    char buffer[SYN_PROMPT_INPUT_MAX];

    for (;;) {
        printf("%s (%ld-%ld, Enter = %ld): ", prompt, min, max, default_value);
        fflush(stdout);

        size_t length = 0;
        buffer[0] = '\0';
        int cancelled = 0;

        for (;;) {
            char byte;
            ssize_t status = read(STDIN_FILENO, &byte, 1);
            if (status <= 0) {  /* stdin closed: treat as cancel */
                cancelled = 1;
                break;
            }
            if (byte == '\n' || byte == '\r') {
                putchar('\n');
                fflush(stdout);
                break;
            }
            if (byte == 27) {
                struct pollfd input = {STDIN_FILENO, POLLIN, 0};
                int ready = poll(&input, 1, 35);
                if (ready > 0 && (input.revents & POLLIN) != 0) {
                    if (syn_prompt_consume_escape_sequence() != 0) {
                        cancelled = 1;
                        break;
                    }
                    continue;  /* arrow key or similar: ignore, keep editing */
                }
                cancelled = 1;
                break;
            }
            if (byte == 'q' || byte == 'Q' || byte == 3) {
                cancelled = 1;
                break;
            }
            if (byte == 127 || byte == '\b') {
                if (length > 0) {
                    --length;
                    fputs("\b \b", stdout);
                    fflush(stdout);
                }
                continue;
            }
            if (byte < '0' || byte > '9')
                continue;  /* ignore every other byte, never echo it */
            if (length + 1 >= sizeof(buffer))
                continue;  /* bounded: extra digits make the parse fail */

            buffer[length++] = byte;
            putchar(byte);
            fflush(stdout);
        }

        if (cancelled)
            return -1;

        /* Empty entry: keep the default. */
        if (length == 0) {
            *out = default_value;
            return 0;
        }

        buffer[length] = '\0';
        long value = 0;
        if (nero_count_parse(buffer, min, max, &value) != 0) {
            printf("Invalid value; enter a whole number between %ld and %ld.\n",
                   min, max);
            continue;
        }
        *out = value;
        return 0;
    }
}

/* Prints the measured local hardware plus the suggested worker ceiling at the
 * top of the settings prompt, so the operator sees "how high to go" before
 * choosing a count. */
static void syn_print_hardware(const nero_hardware_info *info, long suggested)
{
    fputs("Hardware: ", stdout);
    if (info->cpu_logical > 0)
        printf("%ld logical CPU%s", info->cpu_logical,
               info->cpu_logical == 1 ? "" : "s");
    else
        fputs("CPU count unavailable", stdout);

    if (info->ram_total_bytes != NERO_HARDWARE_UNAVAILABLE) {
        const unsigned long long gib = 1024ULL * 1024 * 1024;
        printf(", %llu.%llu GiB RAM",
               info->ram_total_bytes / gib,
               (info->ram_total_bytes % gib) * 10 / gib);
    } else {
        fputs(", RAM unavailable", stdout);
    }
    putchar('\n');
    printf("Suggested max worker threads: %ld\n", suggested);
}

/* Asks the operator to pick the worker count and the packet size before any
 * thread or socket for the run is created. The worker default is the local
 * hardware suggestion, but the accepted range extends to the absolute safety
 * bound so the old fixed 512 ceiling no longer applies. Returns 0 on success
 * (outputs written) or -1 when cancelled, in which case -Syn must not start. */
int nero_syn_prompt_settings(long *thread_count, long *packet_size)
{
    nero_hardware_info info;
    char reason[256];
    nero_hardware_probe(&info);

    long suggested =
        nero_workload_suggested_workers(&info, reason, sizeof(reason));
    if (suggested <= 0) {
        suggested = NERO_SYN_THREAD_COUNT;
        snprintf(reason, sizeof(reason),
                 "hardware recommendation unavailable; using fallback %ld",
                 suggested);
    }
    if (suggested > NERO_SYN_THREAD_HARD_MAX)
        suggested = NERO_SYN_THREAD_HARD_MAX;

    /* Each run starts from the documented defaults instead of silently
     * carrying a previous manual entry forward. */
    *thread_count = suggested;
    *packet_size = NERO_SYN_PACKET_SIZE_MAX;

    fputs("\nNERO - -Syn engine settings (Enter accepts the default, "
          "Esc cancels)\n", stdout);
    syn_print_hardware(&info, suggested);
    printf("Recommendation: %s\n", reason);
    fputs("TCP leg: header-only SYN (40 B). Packet size sets the UDP "
          "datagram size\n"
          "(payload = packet size - 28 B of headers).\n\n", stdout);
    fflush(stdout);

    if (syn_prompt_count("Worker thread count", 1, NERO_SYN_THREAD_HARD_MAX,
                         suggested, thread_count) != 0)
        return -1;
    if (syn_prompt_count("Packet size (bytes)", NERO_SYN_PACKET_SIZE_MIN,
                         NERO_SYN_PACKET_SIZE_MAX,
                         NERO_SYN_PACKET_SIZE_MAX, packet_size) != 0)
        return -1;

    nero_log_write(NERO_LOG_INFO, "syn",
                   "Engine settings selected: threads=%ld packet_size=%ld",
                   *thread_count, *packet_size);
    return 0;
}
