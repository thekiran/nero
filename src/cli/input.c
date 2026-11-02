/* Line editing, escape-sequence decoding and command history. */
#include "cli_internal.h"
#include "nero/terminal.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Legacy symbols retained; this module owns history writes. */
char history[CLI_HISTORY_CAPACITY][CLI_LINE_CAPACITY];
int history_index = 0;
int history_count = 0;

static void copy_line(cli_line *line, const char *text)
{
    size_t length = strlen(text);
    if (length >= sizeof(line->text))
        length = sizeof(line->text) - 1;
    memcpy(line->text, text, length);
    line->text[length] = '\0';
    line->length = line->cursor = length;
}

/* Timeout is used only for escape continuation, never for a whole command. */
static int read_byte(char *ch, int timeout_ms)
{
    if (timeout_ms < 0) {
        ssize_t count;
        do {
            if (cli_termination_signal)
                return 0;
            count = read(STDIN_FILENO, ch, 1);
        } while (count < 0 && errno == EINTR && !cli_termination_signal);
        return count == 1 ? 1 : (count == 0 ? 0 : -1);
    }

    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    int ready;
    do {
        if (cli_termination_signal)
            return 0;
        ready = poll(&input, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (cli_termination_signal)
        return 0;
    if (ready == 0)
        return -2;
    if (ready < 0)
        return -1;
    if ((input.revents & POLLNVAL) != 0)
        return -1;
    if ((input.revents & (POLLERR | POLLHUP)) != 0)
        return 0;
    ssize_t count;
    do {
        count = read(STDIN_FILENO, ch, 1);
    } while (count < 0 && errno == EINTR && !cli_termination_signal);
    return count == 1 ? 1 : (count == 0 ? 0 : -1);
}

enum input_key { KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_DELETE, KEY_REJECT };

static enum input_key escape_key(void)
{
    char ch;
    if (read_byte(&ch, 100) != 1 || (ch != '[' && ch != 'O'))
        return KEY_NONE;
    char sequence[16];
    size_t length = 0;
    while (length < sizeof(sequence) - 1) {
        if (read_byte(&ch, 100) != 1)
            return KEY_NONE;
        sequence[length++] = ch;
        if ((unsigned char)ch >= 0x40 && (unsigned char)ch <= 0x7e) {
            sequence[length] = '\0';
            if (strcmp(sequence, "A") == 0)
                return KEY_UP;
            if (strcmp(sequence, "B") == 0)
                return KEY_DOWN;
            if (strcmp(sequence, "C") == 0)
                return KEY_RIGHT;
            if (strcmp(sequence, "D") == 0)
                return KEY_LEFT;
            if (strcmp(sequence, "3~") == 0)
                return KEY_DELETE;
            return KEY_NONE;
        }
    }
    return KEY_REJECT;
}

void cli_history_add(const char *command)
{
    if (command[strspn(command, " \t\r\n")] == '\0')
        return;
    if (history_count == CLI_HISTORY_CAPACITY) {
        memmove(history[0], history[1], sizeof(history[0]) * (CLI_HISTORY_CAPACITY - 1));
        --history_count;
    }
    size_t length = strlen(command);
    if (length >= CLI_LINE_CAPACITY)
        return;
    memcpy(history[history_count++], command, length + 1);
}

/* 1=line, 2=overlong line rejected, 0=EOF, -1=read failure. */
int cli_read_line(char command[CLI_LINE_CAPACITY])
{
    cli_line line = {{0}, 0, 0};
    char draft[CLI_LINE_CAPACITY] = {0};
    int position = history_count;
    int overflow = 0;
    for (;;) {
        char ch;
        int result = read_byte(&ch, -1);
        if (result != 1)
            return result;
        if (ch == '\n' || ch == '\r') {
            if (overflow)
                return 2;
            memcpy(command, line.text, line.length + 1);
            return 1;
        }
        if (ch == 3) {
            cli_termination_signal = SIGINT;
            return 0;
        }
        if (ch == 4) { /* Ctrl+D: EOF only on an empty line; otherwise forward delete. */
            if (line.length == 0)
                return 0;
            if (line.cursor < line.length) {
                memmove(line.text + line.cursor, line.text + line.cursor + 1,
                        line.length - line.cursor);
                --line.length;
            }
        } else if (ch == 27) {
            enum input_key key = escape_key();
            if (key == KEY_REJECT) {
                overflow = 1;
                continue;
            }
            if (key == KEY_UP && position > 0) {
                if (position == history_count)
                    memcpy(draft, line.text, line.length + 1);
                copy_line(&line, history[--position]);
            } else if (key == KEY_DOWN && position < history_count) {
                ++position;
                copy_line(&line, position == history_count ? draft : history[position]);
            } else if (key == KEY_LEFT && line.cursor > 0) {
                --line.cursor;
            } else if (key == KEY_RIGHT && line.cursor < line.length) {
                ++line.cursor;
            } else if (key == KEY_DELETE && line.cursor < line.length) {
                memmove(line.text + line.cursor, line.text + line.cursor + 1,
                        line.length - line.cursor);
                --line.length;
            }
        } else if (ch == 127 || ch == 8) {
            if (line.cursor > 0) {
                memmove(line.text + line.cursor - 1, line.text + line.cursor,
                        line.length - line.cursor + 1);
                --line.cursor;
                --line.length;
            }
        } else if (((unsigned char)ch >= 32 && ch != 127) || ch == '\t') {
            if (ch == '\t')
                ch = ' ';
            if (line.length == sizeof(line.text) - 1) {
                overflow = 1;
                continue;
            }
            memmove(line.text + line.cursor + 1, line.text + line.cursor,
                    line.length - line.cursor + 1);
            line.text[line.cursor++] = ch;
            ++line.length;
            if (!isatty(STDOUT_FILENO))
                putchar((unsigned char)ch);
        }
        cli_render_line(&line);
    }
}
