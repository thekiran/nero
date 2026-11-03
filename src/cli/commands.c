/* Command tokenization, argument validation and dispatch. */
#include "cli_internal.h"
#include "nero/ascii.h"
#include "nero/printing.h"
#include "nero/syn_flood_powered.h"
#include "nero/terminal.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/* Return 1 for exit, 0 to continue, -1 for a session failure. */
int cli_dispatch_command(const char *command)
{
    char copy[CLI_LINE_CAPACITY];
    size_t length = strlen(command);
    if (length >= sizeof(copy))
        return -1;
    memcpy(copy, command, length + 1);
    char *tokens[3];
    size_t count = 0;
    char *save = NULL;
    for (char *token = strtok_r(copy, " \t\r\n", &save); token;
         token = strtok_r(NULL, " \t\r\n", &save)) {
        if (count == 3)
            goto invalid;
        tokens[count++] = token;
    }
    if (count == 0)
        return 0;
    if (count >= 2 && strcmp(tokens[1], "-Syn") == 0) {
        if (count != 2)
            goto invalid;
        struct in_addr address;
        if (inet_pton(AF_INET, tokens[0], &address) != 1)
            goto invalid;
        /* Do not hold the display mutex or shell handlers across engine calls. */
        if (cli_signal_mode(0) != 0)
            return -1;
        int status = SYN_Flood(tokens[0]);
        if (cli_signal_mode(1) != 0)
            return -1;
        if (status != 0)
            fprintf(stderr, "\n-Syn operation failed (status %d).\n", status);
        return 0;
    }
    if ((count == 1 && strcmp(tokens[0], "-mode") == 0) ||
        (count == 2 && (strcmp(tokens[1], "-Sc") == 0 || strcmp(tokens[1], "-mode") == 0))) {
        /* Inactive legacy commands: keep parsing stable, but say so. */
        if (count == 2 && strcmp(tokens[1], "-Sc") == 0)
            printf("\n-Sc: not available in this build; no network traffic is generated.\n");
        else
            printf("\n-mode: not available in this build.\n");
        return 0;
    }
    if (strcmp(tokens[0], "help") == 0) {
        if (count > 2 || (count == 2 && strcmp(tokens[1], "-All") != 0))
            goto invalid;
        cli_show_help();
    } else if (strcmp(tokens[0], "history") == 0) {
        if (count == 2 && strcmp(tokens[1], "-clear") == 0) {
            memset(history, 0, sizeof(history));
            history_count = 0;
            printf("\nHistory cleared.\n");
        } else if (count == 1) {
            for (int i = 0; i < history_count; ++i)
                printf("\n%s", history[i]);
        } else
            goto invalid;
    } else if (strcmp(tokens[0], "exit") == 0) {
        if (count > 2 || (count == 2 && strcmp(tokens[1], "-All") != 0))
            goto invalid;
        if (terminal_color_enabled())
            fputs(EXIT_MESSAGE, stdout);
        else
            puts("\nExiting...");
        if (count == 2)
            clear_screen();
        return 1;
    } else if (strcmp(tokens[0], "clear") == 0) {
        if (count != 1)
            goto invalid;
        row_location = 16;
        ascii_function();
    } else if (strcmp(tokens[0], "version") == 0) {
        if (count != 1)
            goto invalid;
        fputs(VERSION_MESSAGE, stdout);
    } else {
        if (terminal_color_enabled())
            fputs(UNKNOWN_COMMAND_WARNING, stdout);
        else
            fputs("\n[WARNING] Unknown command! Available commands: 'help'", stdout);
    }
    return 0;
invalid:
    fputs("\nInvalid command parameters. Use 'help'.\n", stderr);
    return 0;
}
