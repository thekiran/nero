/* Command tokenization, argument validation and dispatch.
 *
 * The shell recognises two shapes of command:
 *   - keyword commands whose verb is the first token (help, history, exit,
 *     clear, version), dispatched through CLI_COMMANDS below; and
 *   - target+verb commands whose verb is the second token (<ipv4> -Syn,
 *     <x> -Sc, <x> -mode, or a bare -mode), matched explicitly because the
 *     target occupies the first token.
 * Each handler validates its own arity and returns a cli_cmd_result; the
 * dispatcher maps that onto the public 1/0/-1 contract and owns the single
 * "invalid parameters" diagnostic so every handler reports it identically.
 */
#include "cli_internal.h"
#include "nero/cli/parser.h"
#include "nero/ascii.h"
#include "nero/printing.h"
#include "nero/syn_flood_powered.h"
#include "nero/terminal.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#define CLI_MAX_TOKENS 64

/* Handler outcome. CONTINUE/EXIT/FAILURE map directly onto the values
 * cli_dispatch_command returns; INVALID asks the dispatcher to emit the
 * shared "invalid parameters" diagnostic and then continue the session. */
typedef enum {
    CLI_CMD_CONTINUE = 0,
    CLI_CMD_EXIT = 1,
    CLI_CMD_FAILURE = -1,
    CLI_CMD_INVALID = -2
} cli_cmd_result;

/* "<ipv4> -Syn": validate the target, then hand it to the engine with the
 * shell's own signal handlers suspended for the duration of the call. */
static cli_cmd_result cmd_syn(char **tokens, size_t count)
{
    if (count != 2)
        return CLI_CMD_INVALID;
    struct in_addr address;
    if (inet_pton(AF_INET, tokens[0], &address) != 1)
        return CLI_CMD_INVALID;
    /* Do not hold the display mutex or shell handlers across engine calls. */
    if (cli_signal_mode(0) != 0)
        return CLI_CMD_FAILURE;
    int status = SYN_Flood(tokens[0]);
    if (cli_signal_mode(1) != 0)
        return CLI_CMD_FAILURE;
    /* SYN_UNAVAILABLE means a stub engine was linked and has already
     * explained itself; adding "operation failed" on top would mislead. */
    if (status != 0 && status != SYN_UNAVAILABLE)
        fprintf(stderr, "\n-Syn operation failed (status %d).\n", status);
    return CLI_CMD_CONTINUE;
}

/* "-mode", "<x> -mode" and "<x> -Sc": inactive legacy verbs kept so the
 * parser stays stable, each announcing that it generates no traffic. */
static cli_cmd_result cmd_legacy(char **tokens, size_t count)
{
    if (count == 2 && strcmp(tokens[1], "-Sc") == 0)
        printf("\n-Sc: not available in this build; no network traffic is generated.\n");
    else
        printf("\n-mode: not available in this build.\n");
    return CLI_CMD_CONTINUE;
}

/* "help" or "help -All": both print the full help screen. */
static cli_cmd_result cmd_help(char **tokens, size_t count)
{
    if (count > 2 || (count == 2 && strcmp(tokens[1], "-All") != 0))
        return CLI_CMD_INVALID;
    cli_show_help();
    return CLI_CMD_CONTINUE;
}

/* "history" lists the buffer; "history -clear" empties it. */
static cli_cmd_result cmd_history(char **tokens, size_t count)
{
    if (count == 2 && strcmp(tokens[1], "-clear") == 0) {
        memset(history, 0, sizeof(history));
        history_count = 0;
        printf("\nHistory cleared.\n");
    } else if (count == 1) {
        for (int i = 0; i < history_count; ++i)
            printf("\n%s", history[i]);
    } else {
        return CLI_CMD_INVALID;
    }
    return CLI_CMD_CONTINUE;
}

/* "exit" or "exit -All": leave the shell; the -All form also clears. */
static cli_cmd_result cmd_exit(char **tokens, size_t count)
{
    if (count > 2 || (count == 2 && strcmp(tokens[1], "-All") != 0))
        return CLI_CMD_INVALID;
    if (terminal_color_enabled())
        fputs(EXIT_MESSAGE, stdout);
    else
        puts("\nExiting...");
    if (count == 2)
        clear_screen();
    return CLI_CMD_EXIT;
}

/* "clear": redraw the banner and reset the animation row cursor. */
static cli_cmd_result cmd_clear(char **tokens, size_t count)
{
    (void)tokens;
    if (count != 1)
        return CLI_CMD_INVALID;
    row_location = 16;
    ascii_function();
    return CLI_CMD_CONTINUE;
}

/* "version": print the program version banner. */
static cli_cmd_result cmd_version(char **tokens, size_t count)
{
    (void)tokens;
    if (count != 1)
        return CLI_CMD_INVALID;
    fputs(VERSION_MESSAGE, stdout);
    return CLI_CMD_CONTINUE;
}

/* First-token keyword commands. Second-token verbs are matched ahead of this
 * table in cli_dispatch_command because they carry a target first. */
static const struct {
    const char *name;
    cli_cmd_result (*run)(char **tokens, size_t count);
} CLI_COMMANDS[] = {
    {"help", cmd_help},
    {"history", cmd_history},
    {"exit", cmd_exit},
    {"clear", cmd_clear},
    {"version", cmd_version},
};

static void cli_report_unknown(void)
{
    if (terminal_color_enabled())
        fputs(UNKNOWN_COMMAND_WARNING, stdout);
    else
        fputs("\n[WARNING] Unknown command! Available commands: 'help'", stdout);
}

/* Return 1 for exit, 0 to continue, -1 for a session failure. */
int cli_dispatch_command(const char *command)
{
    char copy[CLI_LINE_CAPACITY];
    size_t length = strlen(command);
    if (length >= sizeof(copy))
        return -1;
    char *tokens[CLI_MAX_TOKENS];
    size_t count = 0;
    if (nero_parse(command, sizeof(copy), copy, sizeof(copy), tokens,
                   CLI_MAX_TOKENS, &count) != NERO_PARSE_OK)
        goto invalid;
    if (count == 0)
        return 0;

    cli_cmd_result result;
    if (count >= 2 && strcmp(tokens[1], "-Syn") == 0) {
        result = cmd_syn(tokens, count);
    } else if ((count == 1 && strcmp(tokens[0], "-mode") == 0) ||
               (count == 2 && (strcmp(tokens[1], "-Sc") == 0 ||
                               strcmp(tokens[1], "-mode") == 0))) {
        result = cmd_legacy(tokens, count);
    } else {
        const size_t command_count = sizeof(CLI_COMMANDS) / sizeof(CLI_COMMANDS[0]);
        size_t i = 0;
        for (; i < command_count; ++i) {
            if (strcmp(tokens[0], CLI_COMMANDS[i].name) == 0) {
                result = CLI_COMMANDS[i].run(tokens, count);
                break;
            }
        }
        if (i == command_count) {
            cli_report_unknown();
            return 0;
        }
    }

    if (result == CLI_CMD_INVALID)
        goto invalid;
    return (int)result;

invalid:
    fputs("\nInvalid command parameters. Use 'help'.\n", stderr);
    return 0;
}
