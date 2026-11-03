/* Terminal capabilities, prompts, line redraws and help presentation. */
#include "cli_internal.h"
#include "command_catalog.h"
#include "nero/printing.h"
#include "nero/terminal.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern pthread_mutex_t ekran_mutex;
int row_location = 14;

/* Capability checks belong at the rendering boundary: redirected output and
 * dumb terminals must not receive interactive cursor-control sequences here. */
int terminal_ansi_enabled(void)
{
    const char *term = getenv("TERM");
    return isatty(STDOUT_FILENO) && term && strcmp(term, "dumb") != 0;
}

int terminal_color_enabled(void)
{
    return terminal_ansi_enabled() && getenv("NO_COLOR") == NULL;
}

int terminal_unicode_enabled(void)
{
#ifdef UNICODE_SUPPORT
    const char *term = getenv("TERM");
    return getenv("NERO_ASCII") == NULL && (!term || strcmp(term, "dumb") != 0);
#else
    return 0;
#endif
}

void clear_screen(void)
{
    if (terminal_ansi_enabled())
        fputs("\033[H\033[J", stdout);
}

static const char *prompt_cursor(void)
{
    return terminal_unicode_enabled() ? TERMINAL_CURSOR : ">";
}

void cli_render_prompt(void)
{
    pthread_mutex_lock(&ekran_mutex);
    if (terminal_color_enabled())
        fputs("\033[0;35m", stdout);
    printf("\n%s ", prompt_cursor());
    if (terminal_color_enabled())
        fputs("\033[0m", stdout);
    fflush(stdout);
    pthread_mutex_unlock(&ekran_mutex);
}

int row_location_function(void)
{
    return row_location;
}

void cli_render_line(const cli_line *line)
{
    if (!isatty(STDOUT_FILENO))
        return;
    struct winsize size;
    size_t columns = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 4)
        columns = size.ws_col;
    size_t visible = columns - 3; /* prompt plus spare column: never autowrap */
    if (visible > CLI_LINE_CAPACITY - 1)
        visible = CLI_LINE_CAPACITY - 1;
    size_t start = line->cursor >= visible ? line->cursor - visible + 1 : 0;
    size_t end = line->length < start + visible ? line->length : start + visible;
    pthread_mutex_lock(&ekran_mutex);
    fputc('\r', stdout);
    if (terminal_ansi_enabled())
        fputs("\033[K", stdout);
    if (terminal_color_enabled())
        fputs("\033[0;35m", stdout);
    printf("%s ", prompt_cursor());
    if (terminal_color_enabled())
        fputs("\033[0m", stdout);
    for (size_t i = start; i < end; ++i) {
        unsigned char ch = (unsigned char)line->text[i];
        /* Commands are ASCII; non-ASCII bytes get one safe display cell each. */
        fputc(ch >= 32 && ch < 127 ? ch : '?', stdout);
    }
    size_t back = end - line->cursor;
    if (!terminal_ansi_enabled()) {
        size_t spaces = visible - (end - start);
        for (size_t i = 0; i < spaces; ++i)
            fputc(' ', stdout);
        back += spaces;
        for (size_t i = 0; i < back; ++i)
            fputc('\b', stdout);
    } else if (back > 0) {
        printf("\033[%zuD", back);
    }
    fflush(stdout);
    pthread_mutex_unlock(&ekran_mutex);
}

void cli_show_help(void)
{
    pthread_mutex_lock(&ekran_mutex);
    row_location = row_location > INT_MAX - 11 ? 16 : row_location + 11;
    static const char *const commands[][2] = {
#define CLI_HELP(usage, description) {usage, description},
        CLI_HELP_ENTRIES(CLI_HELP)
#undef CLI_HELP
    };

    int num_commands = sizeof(commands) / sizeof(commands[0]);

    printf("\nEXPERIMENTAL SOFTWARE — LABORATORY USE ONLY\n"
           "EXPERIMENTAL SOFTWARE — RESTRICTED TO LABORATORY USE\n"
           "Isolated or explicitly authorized testing only. Not for public or production use.\n"
           "Use isolated or authorized test environments; public and production use is "
           "not permitted.\n");
#if defined(__APPLE__) || defined(NERO_SAFE_TEST)
    printf("-Syn: unsupported in this build; no network traffic is generated.\n");
#endif
    if (terminal_color_enabled())
        printf("\n\033[1;32mAvailable Commands:\033[0m\n");
    else
        puts("\nAvailable Commands:");
    if (terminal_color_enabled() && terminal_unicode_enabled())
        printf(PRINTING_LINE_1);
    else
        puts("-------------------------------------------------------------");

    for (int counter_help = 0; counter_help < num_commands; counter_help++) {
        if (terminal_color_enabled())
            printf("%d.\033[1;33m%-20s\033[0m %s\n", counter_help + 1, commands[counter_help][0],
                   commands[counter_help][1]);
        else
            printf("%d.%-20s %s\n", counter_help + 1, commands[counter_help][0],
                   commands[counter_help][1]);
        fflush(stdout);
    }

    if (terminal_color_enabled() && terminal_unicode_enabled())
        printf(PRINTING_LINE_1);
    else
        puts("-------------------------------------------------------------");

    fflush(stdout);
    pthread_mutex_unlock(&ekran_mutex);
}
