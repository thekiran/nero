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
void cli_show_help(void);
void cli_show_syn_status(void);

/* commands.c: 1 = exit, 0 = continue, -1 = session failure. */
int cli_dispatch_command(const char *command);

#endif
