/* Line editing, escape-sequence decoding and command history. */
#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include "cli_internal.h"
#include "command_catalog.h"
#include "nero/terminal.h"

#include <errno.h>
#ifndef _WIN32
#include <poll.h>
#include <sys/select.h>
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Legacy symbols retained; this module owns history writes. */
char history[CLI_HISTORY_CAPACITY][CLI_LINE_CAPACITY] = {{0}};
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

static int complete_line(cli_line *line)
{
    static const char *const candidates[] = {
#define CLI_CANDIDATE(usage, description) usage,
        CLI_HELP_ENTRIES(CLI_CANDIDATE)
#undef CLI_CANDIDATE
    };
    const char *match = NULL;
    if (line->cursor != line->length || line->length == 0)
        return 0;
    for (size_t index = 0; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        if (candidates[index][0] == '<' ||
            strncmp(candidates[index], line->text, line->length) != 0)
            continue;
        if (match != NULL)
            return 0;
        match = candidates[index];
    }
    if (match == NULL || strlen(match) >= sizeof(line->text))
        return 0;
    copy_line(line, match);
    return 1;
}

/* Timeout is used only for escape continuation, never for a whole command. */
static int read_byte(char *ch, int timeout_ms)
{
#ifndef _WIN32
    /* Blocking the session signals makes the flag check and pselect's atomic
     * unblock/wait indivisible. A signal just before read() used to be lost as
     * a wakeup, leaving the terminal waiting for a key after SIGHUP. */
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGHUP);
    sigaddset(&blocked, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) != 0)
        return -1;
    int result = 0;
    while (!cli_termination_signal) {
        fd_set input;
        FD_ZERO(&input);
        FD_SET(STDIN_FILENO, &input);
        struct timespec timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
        int ready = pselect(STDIN_FILENO + 1, &input, NULL, NULL,
                            timeout_ms < 0 ? NULL : &timeout, &previous);
        if (cli_termination_signal)
            break;
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            result = ready == 0 ? -2 : -1;
            break;
        }
        /* This session is stdin's sole reader. Keep signals blocked through
         * the ready read; pending termination runs when the mask is restored. */
        ssize_t count = read(STDIN_FILENO, ch, 1);
        result = count == 1 ? 1 : (count == 0 ? 0 : -1);
        break;
    }
    int saved_errno = errno;
    if (sigprocmask(SIG_SETMASK, &previous, NULL) != 0)
        return -1;
    errno = saved_errno;
    return cli_termination_signal ? 0 : result;
#else
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
#endif
}

enum input_key { KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_DELETE, KEY_REJECT };

/* Consume the whole bounded escape sequence before returning a key so
 * terminal control bytes cannot leak into the editable command text. */
static enum input_key escape_key(void)
{
    char ch;
    int continuation_timeout = isatty(STDIN_FILENO) ? 100 : -1;
    if (read_byte(&ch, continuation_timeout) != 1 || (ch != '[' && ch != 'O'))
        return KEY_NONE;
    char sequence[16];
    size_t length = 0;
    while (length < sizeof(sequence) - 1) {
        if (read_byte(&ch, continuation_timeout) != 1)
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
            if (ch == '\t' && complete_line(&line)) {
                cli_render_line(&line);
                continue;
            }
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
