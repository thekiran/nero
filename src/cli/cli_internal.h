#ifndef NERO_CLI_INTERNAL_H
#define NERO_CLI_INTERNAL_H

/* Private CLI contract. Applications include nero/terminal.h instead. */
#include <signal.h>
#include <stddef.h>

#define CLI_HISTORY_CAPACITY 256
#define CLI_LINE_CAPACITY 256

/* Input owns byte offsets; rendering must not mutate the line. */
typedef struct {
    char text[CLI_LINE_CAPACITY];
    size_t length;
    size_t cursor;
} cli_line;

/* Compatibility state: input.c owns history; render.c owns row_location. */
extern char history[CLI_HISTORY_CAPACITY][CLI_LINE_CAPACITY];
extern int history_count;
extern int row_location;
extern volatile sig_atomic_t cli_termination_signal;

/* terminal.c: switch between shell and saved signal handlers. */
int cli_signal_mode(int enabled);

/* input.c: 1 = line, 2 = rejected overflow, 0 = EOF, -1 = read error. */
int cli_read_line(char command[CLI_LINE_CAPACITY]);
void cli_history_add(const char *command);
void cli_history_load(void);
void cli_history_persist(const char *command);
int cli_history_clear(void);

/* render.c: presentation only. */
void cli_render_prompt(void);
void cli_render_line(const cli_line *line);
/* Like cli_render_line, but also draws `ghost` (inline gray suggestion) after
 * the cursor, clipped to the viewport. `ghost` NULL/empty renders no ghost.
 * Ghost is drawn only when the terminal supports color (so it is visually
 * distinct from typed text); otherwise it is silently ignored. */
void cli_render_line_ex(const cli_line *line, const char *ghost);
/* Draw `count` lines beneath the current input line (completion menu or live
 * help), then return the cursor to the input position. No-op unless the output
 * is an ANSI-capable TTY. */
void cli_render_below(const char *const *lines, size_t count);
/* Erase any below-line region previously drawn by cli_render_below. */
void cli_render_below_clear(void);
void cli_show_help(void);
void cli_show_help_details(void);
void cli_show_syn_status(void);

/* commands.c: 1 = exit, 0 = continue, -1 = session failure. */
int cli_dispatch_command(const char *command);

/* Shared keyword-handler contract. Handlers live in commands.c and workload.c;
 * the dispatcher maps CLI_CMD_INVALID onto the "invalid parameters" path and
 * forwards the rest unchanged. */
typedef enum {
    CLI_CMD_CONTINUE = 0,
    CLI_CMD_EXIT = 1,
    CLI_CMD_FAILURE = -1,
    CLI_CMD_INVALID = -2
} cli_cmd_result;

/* workload.c: read-only -Syn preflight screen (`preflight`). Describes the
 * engine's run-time limits and platform support without opening a socket,
 * sending a packet or starting -Syn. */
cli_cmd_result cmd_preflight(char **tokens, size_t count);

/* wifi_command.c: passive wireless monitoring, active CoreWLAN survey and
 * offline 802.11 capture analysis. Live capture is unavailable in safe builds. */
cli_cmd_result cmd_wifi(char **tokens, size_t count);

#endif
