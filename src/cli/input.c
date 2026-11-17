/* Line editing, escape-sequence decoding and command history. */
#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include "cli_internal.h"
#include "inline_help.h"
#include "suggestion_engine.h"
#include "nero/terminal.h"

#include <errno.h>
#ifndef _WIN32
#include <poll.h>
#include <sys/select.h>
#endif
#include <stdio.h>
#include <stdlib.h>
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

/* ------------------------------------------------------------------ */
/* IntelliSense configuration                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int enabled; /* master switch (NERO_INTELLISENSE != off) */
    int ghost;   /* inline gray suggestions (needs a color TTY) */
    int menu;    /* interactive Tab completion menu (needs a TTY) */
    int help;    /* live contextual help line (full mode, needs a TTY) */
    int full;    /* full vs compact presentation */
} intellisense_cfg;

/*
 * Resolve the assistant configuration from the environment and terminal
 * capabilities. Honors NERO_INTELLISENSE (off/0 disables; full enables the
 * live help line) and degrades gracefully for NO_COLOR, TERM=dumb, and
 * non-TTY / redirected output. Completion of the buffer still works when
 * rendering is disabled; only the visual aids are suppressed.
 */
static void intellisense_load(intellisense_cfg *cfg)
{
    const char *mode = getenv("NERO_INTELLISENSE");
    int master = !(mode != NULL && (strcmp(mode, "off") == 0 || strcmp(mode, "0") == 0));
    int full = mode != NULL && strcmp(mode, "full") == 0;
    int tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && terminal_ansi_enabled();
    int color = terminal_color_enabled();
    cfg->enabled = master;
    cfg->full = full;
    cfg->ghost = master && tty && color;
    cfg->menu = master && tty;
    cfg->help = master && tty && full;
}

/* Snapshot of the in-process history as a pointer array for the engine. */
static size_t history_pointers(const char *out[CLI_HISTORY_CAPACITY])
{
    size_t n = history_count < 0 ? 0 : (size_t)history_count;
    if (n > CLI_HISTORY_CAPACITY)
        n = CLI_HISTORY_CAPACITY;
    for (size_t i = 0; i < n; ++i)
        out[i] = history[i];
    return n;
}

/* Compute the current ghost suffix for `line` (cursor must be at the end). */
static void current_ghost(const cli_line *line, char *ghost, size_t ghost_size)
{
    const char *hist[CLI_HISTORY_CAPACITY];
    size_t n = history_pointers(hist);
    nero_suggest_ghost(line->text, hist, n, ghost, ghost_size);
}

/* Redraw the input line (with ghost, if enabled) plus the live help line. */
static void refresh_line(const cli_line *line, const intellisense_cfg *cfg)
{
    char ghost[CLI_LINE_CAPACITY] = {0};
    if (cfg->ghost && line->cursor == line->length)
        current_ghost(line, ghost, sizeof(ghost));
    cli_render_line_ex(line, cfg->ghost ? ghost : NULL);

    if (!cfg->help)
        return;
    if (line->length == 0) {
        cli_render_below_clear();
        return;
    }
    nero_help_entry entry;
    if (!nero_inline_help_lookup(line->text, &entry)) {
        cli_render_below_clear();
        return;
    }
    const char *status = "";
    switch (entry.status) {
        case NERO_VALIDATE_INCOMPLETE: status = "incomplete"; break;
        case NERO_VALIDATE_VALID:      status = "ready"; break;
        case NERO_VALIDATE_UNSUPPORTED:status = "unsupported here"; break;
        case NERO_VALIDATE_INVALID:    status = "unknown"; break;
        default: break;
    }
    char row[256];
    snprintf(row, sizeof(row), "\033[2m%s  %s  [%s]\033[0m", entry.syntax, entry.description,
             status);
    const char *rows[1] = {row};
    cli_render_below(rows, 1);
}

/* Bytes consumed while probing an escape sequence that turn out not to
 * belong to one are replayed here, so a plain Esc can never swallow the
 * keystrokes that followed it into the continuation window. */
static char escape_pushback[16];
static size_t escape_pushback_length;

/* Timeout is used only for escape continuation, never for a whole command. */
static int read_byte(char *ch, int timeout_ms)
{
    if (escape_pushback_length > 0) {
        *ch = escape_pushback[0];
        memmove(escape_pushback, escape_pushback + 1, --escape_pushback_length);
        return 1;
    }
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
    if (read_byte(&ch, continuation_timeout) != 1)
        return KEY_NONE;
    if (ch != '[' && ch != 'O') {
        /* The byte after Esc is real input that landed inside the
         * continuation window; replay it instead of dropping it. */
        escape_pushback[escape_pushback_length++] = ch;
        return KEY_NONE;
    }
    char sequence[16];
    size_t length = 0;
    while (length < sizeof(sequence) - 1) {
        if (read_byte(&ch, continuation_timeout) != 1) {
            /* Incomplete sequence: replay the consumed bytes as literal text. */
            for (size_t index = 0; index < length; ++index)
                escape_pushback[escape_pushback_length++] = sequence[index];
            return KEY_NONE;
        }
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

/* ------------------------------------------------------------------ */
/* Tab completion menu                                                */
/* ------------------------------------------------------------------ */

#define MENU_MAX_ROWS 10

enum menu_key { MK_CANCEL, MK_PREV, MK_NEXT, MK_ACCEPT, MK_IGNORE };

/* Decode an escape sequence while the menu is open: arrows, Shift+Tab (CSI Z)
 * and Right-to-accept. A bare Esc (or an unrecognized sequence) cancels. */
static enum menu_key menu_escape(void)
{
    char ch;
    int timeout = isatty(STDIN_FILENO) ? 100 : -1;
    if (read_byte(&ch, timeout) != 1)
        return MK_CANCEL;
    if (ch != '[' && ch != 'O')
        return MK_CANCEL;
    if (read_byte(&ch, timeout) != 1)
        return MK_CANCEL;
    switch (ch) {
        case 'A': return MK_PREV;   /* Up */
        case 'B': return MK_NEXT;   /* Down */
        case 'C': return MK_ACCEPT; /* Right */
        case 'D': return MK_IGNORE; /* Left */
        case 'Z': return MK_PREV;   /* Shift+Tab */
        default:  return MK_IGNORE;
    }
}

static void menu_draw(const nero_suggestion_list *list, size_t shown, size_t selected,
                      const intellisense_cfg *cfg)
{
    int color = terminal_color_enabled();
    int unicode = terminal_unicode_enabled();
    const char *marker = unicode ? "\xe2\x96\xb8 " : "> "; /* ▸ */
    char buffers[MENU_MAX_ROWS + 1][192];
    const char *rows[MENU_MAX_ROWS + 1];
    size_t rowc = 0;
    for (size_t i = 0; i < shown; ++i) {
        const char *label = list->items[i].display != NULL ? list->items[i].display
                                                           : list->items[i].text;
        const char *desc = list->items[i].description != NULL ? list->items[i].description : "";
        /* Bound the label/description fields with a precision so the formatted
         * row provably fits the fixed buffer. Realistic completion labels and
         * descriptions are far shorter; this only caps pathological inputs and
         * keeps GCC's -Wformat-truncation analysis satisfied. */
        if (i == selected && color)
            snprintf(buffers[i], sizeof(buffers[i]),
                     "%s\033[1;36m%-22.40s\033[0m \033[2m%.100s\033[0m", marker, label, desc);
        else
            snprintf(buffers[i], sizeof(buffers[i]), "%s%-22.40s %s%.100s%s",
                     i == selected ? marker : "  ", label, color ? "\033[2m" : "", desc,
                     color ? "\033[0m" : "");
        rows[rowc++] = buffers[i];
    }
    if (list->count > shown) {
        snprintf(buffers[shown], sizeof(buffers[shown]), "  %s... %zu more",
                 color ? "\033[2m" : "", list->count - shown);
        rows[rowc++] = buffers[shown];
    }
    (void)cfg;
    cli_render_below(rows, rowc);
}

/* Interactive menu. Returns: 1 = accepted (line updated), 0 = closed, and
 * writes the propagated read result into *propagate (used only when 0 is
 * returned because of EOF/error, see caller). */
enum menu_result { MENU_CLOSED, MENU_ACCEPTED, MENU_EOF, MENU_ERROR };

static enum menu_result run_menu(cli_line *line, const nero_suggestion_list *list,
                                 const intellisense_cfg *cfg)
{
    size_t shown = list->count < MENU_MAX_ROWS ? list->count : MENU_MAX_ROWS;
    if (shown == 0)
        return MENU_CLOSED;
    size_t selected = 0;
    for (;;) {
        menu_draw(list, shown, selected, cfg);
        char ch;
        int result = read_byte(&ch, -1);
        if (result == 0) {
            cli_render_below_clear();
            return MENU_EOF;
        }
        if (result < 0) {
            cli_render_below_clear();
            return MENU_ERROR;
        }
        if (ch == '\t') {
            selected = (selected + 1) % shown;
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            copy_line(line, list->items[selected].text);
            cli_render_below_clear();
            return MENU_ACCEPTED;
        }
        if (ch == 27) {
            enum menu_key key = menu_escape();
            if (key == MK_NEXT)
                selected = (selected + 1) % shown;
            else if (key == MK_PREV)
                selected = (selected + shown - 1) % shown;
            else if (key == MK_ACCEPT) {
                copy_line(line, list->items[selected].text);
                cli_render_below_clear();
                return MENU_ACCEPTED;
            } else if (key == MK_CANCEL) {
                cli_render_below_clear();
                return MENU_CLOSED;
            }
            /* MK_IGNORE: keep the menu open. */
            continue;
        }
        /* Any other key closes the menu and is replayed to the editor so that
         * typing, Ctrl+C and Ctrl+D keep their normal semantics. */
        cli_render_below_clear();
        if (escape_pushback_length < sizeof(escape_pushback))
            escape_pushback[escape_pushback_length++] = ch;
        return MENU_CLOSED;
    }
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
    cli_history_persist(command);
}

/* Accept the current ghost suggestion by appending it to the line. Returns 1
 * if anything was appended. Only valid when the cursor is at the end. */
static int accept_ghost(cli_line *line)
{
    if (line->cursor != line->length)
        return 0;
    char ghost[CLI_LINE_CAPACITY] = {0};
    current_ghost(line, ghost, sizeof(ghost));
    if (ghost[0] == '\0')
        return 0;
    size_t add = strlen(ghost);
    if (line->length + add >= sizeof(line->text))
        add = sizeof(line->text) - 1 - line->length;
    if (add == 0)
        return 0;
    memcpy(line->text + line->length, ghost, add);
    line->length += add;
    line->text[line->length] = '\0';
    line->cursor = line->length;
    return 1;
}

/* Handle a Tab keypress. Returns the completion outcome; on an ambiguous
 * result a menu is shown when enabled. May set *overflow / return an EOF or
 * error propagation request via *propagate (non-zero = return that value). */
static void handle_tab(cli_line *line, const intellisense_cfg *cfg, int *propagate)
{
    *propagate = 0;
    if (line->cursor != line->length)
        return; /* only complete at end of buffer */

    char buffer[CLI_LINE_CAPACITY];
    memcpy(buffer, line->text, line->length + 1);
    nero_suggestion_list list;
    nero_tab_result outcome = nero_suggest_complete(buffer, sizeof(buffer), &list);

    if (outcome == NERO_TAB_COMPLETED) {
        copy_line(line, buffer);
        return;
    }
    if (outcome == NERO_TAB_AMBIGUOUS) {
        /* Ambiguous: show a menu when possible, but never fall back to
         * inserting a literal space. */
        if (cfg->menu) {
            enum menu_result mr = run_menu(line, &list, cfg);
            if (mr == MENU_EOF)
                *propagate = 0x100; /* sentinel: return 0 (EOF) */
            else if (mr == MENU_ERROR)
                *propagate = 0x200; /* sentinel: return -1 (error) */
        }
        return;
    }
    /* NERO_TAB_NONE: nothing to complete. Preserve the legacy editor's
     * behavior of treating Tab as a token separator so command lines typed
     * with a Tab between words still parse, but never produce a double space. */
    if (line->length > 0 && line->length < sizeof(line->text) - 1 &&
        line->text[line->length - 1] != ' ') {
        line->text[line->length++] = ' ';
        line->text[line->length] = '\0';
        line->cursor = line->length;
        if (!isatty(STDOUT_FILENO))
            putchar(' ');
    }
}

/* 1=line, 2=overlong line rejected, 0=EOF, -1=read failure. */
int cli_read_line(char command[CLI_LINE_CAPACITY])
{
    intellisense_cfg cfg;
    intellisense_load(&cfg);
    cli_line line = {{0}, 0, 0};
    char draft[CLI_LINE_CAPACITY] = {0};
    int position = history_count;
    int overflow = 0;
    for (;;) {
        char ch;
        int result = read_byte(&ch, -1);
        if (result != 1) {
            if (cfg.help)
                cli_render_below_clear();
            return result;
        }
        if (ch == '\n' || ch == '\r') {
            if (cfg.help)
                cli_render_below_clear();
            if (overflow)
                return 2;
            memcpy(command, line.text, line.length + 1);
            return 1;
        }
        if (ch == 3) {
            if (cfg.help)
                cli_render_below_clear();
            cli_termination_signal = SIGINT;
            return 0;
        }
        if (ch == 4) { /* Ctrl+D: EOF only on an empty line; otherwise forward delete. */
            if (line.length == 0) {
                if (cfg.help)
                    cli_render_below_clear();
                return 0;
            }
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
            } else if (key == KEY_RIGHT) {
                /* Right moves the cursor; at the end of the buffer it accepts
                 * the inline ghost suggestion instead. */
                if (line.cursor < line.length)
                    ++line.cursor;
                else if (cfg.ghost)
                    accept_ghost(&line);
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
        } else if (ch == '\t') {
            int propagate = 0;
            handle_tab(&line, &cfg, &propagate);
            if (propagate == 0x100)
                return 0;
            if (propagate == 0x200)
                return -1;
        } else if ((unsigned char)ch >= 32 && ch != 127) {
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
        refresh_line(&line, &cfg);
    }
}
