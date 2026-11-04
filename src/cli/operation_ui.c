/* Reusable terminal operation surface and keyboard decoder.
 * The caller owns operation state and rendering order; this module owns the
 * alternate-screen transition. Operations are sequential, not nested. */
#include "nero/operation_ui.h"
#include "nero/ascii.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define ESCAPE_CONTINUATION_MS 35
#define OPERATION_DOT_COUNT 6

static int alternate_screen_active = 0;
static nero_operation_ui *current_ui;

static void position(int row, int column)
{
    printf("\033[%d;%dH", row, column);
}

static void paint_layout(nero_operation_ui *ui)
{
    if (!ui->interactive)
        return;
    fputs("\033[0m\033[H\033[2J", stdout);
    if (ui->compact) {
        /* Never wrap or shrink the brand into a different logo. */
        position(1, 1);
        printf("%.*s", ui->columns - 1, "Enlarge terminal to at least 64 x 24");
        return;
    }
    if (ui->color) fputs("\033[35m", stdout);
    fputs(NERO_ASCII_LOGO, stdout);
    if (ui->color) fputs("\033[0m", stdout);
    position(14, 3);
    if (ui->color) fputs("\033[1m", stdout);
    printf("%.*s", ui->columns - 4, ui->title ? ui->title : "");
    if (ui->color) fputs("\033[0m", stdout);
    position(15, 3);
    if (ui->color) fputs("\033[2m", stdout);
    printf("%.*s", ui->columns - 4, ui->detail ? ui->detail : "");
    if (ui->color) fputs("\033[0m", stdout);
    position(ui->rows, 3);
    if (ui->color) fputs("\033[2m", stdout);
    printf("%.*s", ui->columns - 4, ui->controls ? ui->controls : "");
    if (ui->color) fputs("\033[0m", stdout);
}

static void refresh_layout(nero_operation_ui *ui)
{
    struct winsize size = {0};
    int rows = 24, columns = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        if (size.ws_row) rows = size.ws_row;
        if (size.ws_col) columns = size.ws_col;
    }
    if (rows == ui->rows && columns == ui->columns) return;
    ui->rows = rows;
    ui->columns = columns;
    ui->compact = columns < 64 || rows < 24;
    ui->content_top = 17;
    ui->content_bottom = rows - 2;
    paint_layout(ui);
}

/* Clear bounded rows rather than the rest of the screen: the logo and footer
 * are owned by the layout and cannot be erased by an operation update. */
static int body(nero_operation_ui *ui)
{
    if (!ui || !ui->interactive) return 1;
    refresh_layout(ui);
    if (ui->compact) { fflush(stdout); return 0; }
    for (int row = ui->content_top; row <= ui->content_bottom; ++row) {
        position(row, 1);
        fputs("\033[2K", stdout);
    }
    position(ui->content_top, 3);
    return 1;
}

/* Idempotent normal-exit fallback. atexit does not run after SIGKILL or a
 * fatal crash, so callers must still end operations on recoverable paths. */
static void restore_operation_surface(void)
{
    if (!alternate_screen_active)
        return;
    fputs("\033[0m\033[?1049l", stdout);
    fflush(stdout);
    alternate_screen_active = 0;
    fputs("\033[?25h", stdout);
    fflush(stdout);
}

/* Return 1 for a byte, 0 for timeout and a negative value for input failure.
 * A zero timeout lets animation loops inspect input without blocking a frame. */
static int read_byte(char *byte, int timeout_ms)
{
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    int ready;
    do {
        ready = poll(&input, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0)
        return ready;
    if ((input.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        return -1;
    if ((input.revents & POLLIN) == 0)
        return 0;
    return read(STDIN_FILENO, byte, 1) == 1 ? 1 : -1;
}

void nero_operation_begin(
    nero_operation_ui *ui,
    const char *title,
    const char *detail,
    const char *controls
)
{
    if (ui == NULL)
        return;
    /* Both ends must be terminals before switching screen buffers. Redirected
     * output keeps its linear transcript instead of entering another screen. */
    const char *term = getenv("TERM");
    ui->interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
                      term && strcmp(term, "dumb") != 0;
    ui->color = ui->interactive && getenv("NO_COLOR") == NULL;
    ui->title = title;
    ui->detail = detail;
    ui->controls = controls;
    ui->rows = ui->columns = ui->scroll_offset = 0;
    current_ui = ui;
    ui->active = 1;
    if (ui->interactive) {
        static int cleanup_registered = 0;
        if (!cleanup_registered && atexit(restore_operation_surface) == 0)
            cleanup_registered = 1;
        alternate_screen_active = 1;
        fputs("\033[?1049h\033[?25l", stdout);
        refresh_layout(ui);
        fflush(stdout);
        return;
    } else {
        putchar('\n');
    }

    /* Redirected output is a plain transcript, with no cursor or color codes. */
    fputs(NERO_ASCII_LOGO, stdout);
    putchar('\n');

    /* Keep operation metadata light so the shared logo remains the heading. */
    if (title != NULL && title[0] != '\0')
        printf("  %s\n", title);
    if (detail != NULL && detail[0] != '\0')
        printf("  %s\n", detail);
    if (controls != NULL && controls[0] != '\0') {
        printf("  %s\n", controls);
    }
    putchar('\n');
    fflush(stdout);
}

void nero_operation_section(const char *label)
{
    if (!body(current_ui)) return;
    printf("%s\n", label);
    fflush(stdout);
}

/* Replace one status line in place. Frames 0..5 highlight one dot; any larger
 * frame produces the static six-dot representation used for reduced motion. */
void nero_operation_loader(const char *label, const char *detail, unsigned frame)
{
    (void)detail; /* The target already appears once in the fixed header. */
    if (!body(current_ui)) return;
    printf("%-12s ", label);
    for (unsigned index = 0; index < OPERATION_DOT_COUNT; ++index) {
        int color = current_ui && current_ui->color;
        if (color) fputs(index == frame ? "\033[35m" : "\033[2m", stdout);
        fputs(index == frame ? "●" : "·", stdout);
        if (color) fputs("\033[0m", stdout);
    }
    fflush(stdout);
}

/* color is an internal ANSI SGR parameter, not arbitrary external text.
 * Finalizing the loader with a newline leaves later sections on a fresh row. */
void nero_operation_status(const char *color, const char *label, const char *detail)
{
    if (!body(current_ui)) return;
    int styled = current_ui && current_ui->color;
    if (styled) printf("\033[%sm", color ? color : "0");
    printf("%s", label);
    if (styled) fputs("\033[0m", stdout);
    if (detail != NULL && detail[0] != '\0')
        printf("  %s", detail);
    putchar('\n');
    fflush(stdout);
}

void nero_operation_end(nero_operation_ui *ui)
{
    if (ui == NULL || !ui->active)
        return;
    if (ui->interactive) {
        restore_operation_surface();
    } else {
        putchar('\n');
    }
    fflush(stdout);
    ui->active = 0;
    current_ui = NULL;
}

/* Discovery and selection share column widths, so the same result keeps its
 * visual position when the operation changes from scanning to choosing. */
static void port_table_header(nero_operation_ui *ui, int row)
{
    if (ui->interactive) position(row, 3);
    else putchar('\n');
    if (ui->color) fputs("\033[2m", stdout);
    printf("  %-9s%-13s%s", "PORT", "PROTOCOL", "STATUS");
    if (ui->color) fputs("\033[0m", stdout);
}

static void port_table_row(nero_operation_ui *ui, int row, int port, int selected)
{
    if (ui->interactive) position(row, 3);
    else putchar('\n');
    if (ui->color) fputs(selected ? "\033[1;35m" : "\033[1m", stdout);
    printf("%s %-9d", selected ? ">" : " ", port);
    if (ui->color) fputs("\033[0;2m", stdout);
    printf("%-13s", "TCP");
    if (ui->color) fputs("\033[0;35m", stdout);
    fputs("OPEN", stdout);
    if (ui->color) fputs("\033[0m", stdout);
}

void nero_operation_ports(nero_operation_ui *ui, const int *ports, int count, int selected)
{
    ui->controls = "↑↓ · Move   Enter · Select   Esc · Back";
    if (!body(ui)) return;
    if (ui->interactive) {
        /* Discovery parked a progress bar on the last row and the controls one
         * row above it; clear both before the selection menu redraws. */
        position(ui->rows - 1, 1); fputs("\033[2K", stdout);
        position(ui->rows, 1); fputs("\033[2K", stdout);
    }
    int visible = ui->interactive ? ui->content_bottom - ui->content_top - 2 : count;
    if (selected < ui->scroll_offset) ui->scroll_offset = selected;
    if (selected >= ui->scroll_offset + visible) ui->scroll_offset = selected - visible + 1;
    if (ui->color) fputs("\033[1m", stdout);
    printf("Choose a port");
    if (ui->color) fputs("\033[0;2m", stdout);
    printf("     %d / %d", selected + 1, count);
    if (ui->color) fputs("\033[0m", stdout);
    port_table_header(ui, ui->content_top + 2);
    for (int index = 0; index < visible && ui->scroll_offset + index < count; ++index) {
        int item = ui->scroll_offset + index;
        port_table_row(ui, ui->content_top + index + 3, ports[item], item == selected);
    }
    if (ui->interactive) {
        position(ui->rows, 3);
        printf("%s", ui->controls);
    }
    fflush(stdout);
}

void nero_operation_frame(nero_operation_ui *ui, const char *frame, unsigned attempt)
{
    if (!body(ui)) return;
    if (ui->color && getenv("NERO_REDUCE_MOTION") == NULL)
        fputs(frame, stdout); /* Existing frames occupy three bounded body rows. */
    else
        printf("Connection test %u/4", attempt);
    fflush(stdout);
}

/* Full-width progress bar on the terminal's last row. Filled cells use the
 * UI accent; the percentage is printed at the right. The last column is left
 * untouched so writing the bar never triggers an auto-wrap scroll. */
static void draw_progress_bar(nero_operation_ui *ui, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    char suffix[8];
    int suffix_len = snprintf(suffix, sizeof(suffix), " %3d%%", percent);
    int usable = ui->columns - 1;            /* keep the final column empty */
    int bar_cells = usable - suffix_len;
    if (bar_cells < 1) return;               /* too narrow for a meaningful bar */
    int filled = (int)((long)bar_cells * percent / 100);

    position(ui->rows, 1);
    fputs("\033[2K", stdout);
    if (ui->color) fputs("\033[35m", stdout);
    for (int cell = 0; cell < filled; ++cell) fputs("█", stdout);
    if (ui->color) fputs("\033[2m", stdout);
    for (int cell = filled; cell < bar_cells; ++cell) fputs("░", stdout);
    if (ui->color) fputs("\033[0m", stdout);
    fputs(suffix, stdout);
}

void nero_operation_discovery(const int *recent, int recent_count, int total,
                              unsigned frame, int latest_port, int percent)
{
    nero_operation_loader("Scanning", NULL, frame);
    nero_operation_ui *ui = current_ui;
    if (!ui || !ui->interactive || ui->compact) return;
    position(ui->content_top + 1, 3);
    if (ui->color) fputs("\033[2m", stdout);
    if (latest_port > 0)
        printf("Latest probe  TCP / %d", latest_port);
    else
        fputs("Preparing TCP probes", stdout);
    if (ui->color) fputs("\033[0m", stdout);
    position(ui->content_top + 2, 3);
    if (ui->color) fputs("\033[1m", stdout);
    printf("Discovered");
    if (ui->color) fputs("\033[0;35m", stdout);
    printf("  %d", total);
    if (ui->color) fputs("\033[0;2m", stdout);
    printf(" open port%s", total == 1 ? "" : "s");
    if (ui->color) fputs("\033[0m", stdout);
    port_table_header(ui, ui->content_top + 3);
    int visible = ui->content_bottom - ui->content_top - 3;
    if (visible < 1) visible = 1;
    int start = recent_count > visible ? recent_count - visible : 0;
    for (int index = start; index < recent_count; ++index) {
        port_table_row(ui, ui->content_top + 4 + index - start, recent[index], 0);
    }
    /* Controls hint on the second-to-last row, with the full-width progress
     * bar pinned to the very bottom row, directly beneath it. */
    position(ui->rows - 1, 1);
    fputs("\033[2K", stdout);
    position(ui->rows - 1, 3);
    if (ui->color) fputs("\033[2m", stdout);
    printf("%.*s", ui->columns - 4, ui->controls ? ui->controls : "Esc · Back");
    if (ui->color) fputs("\033[0m", stdout);
    draw_progress_bar(ui, percent);
    fflush(stdout);
}

nero_ui_key nero_operation_read_key(int timeout_ms)
{
    char byte;
    int status = read_byte(&byte, timeout_ms);
    if (status < 0)
        return NERO_UI_KEY_EOF;
    if (status == 0)
        return NERO_UI_KEY_NONE;
    if (byte == 'q' || byte == 'Q' || byte == 3)
        return NERO_UI_KEY_CANCEL;
    if (byte == '\n' || byte == '\r')
        return NERO_UI_KEY_ENTER;
    if ((unsigned char)byte != 27)
        return NERO_UI_KEY_OTHER;

    /* Escape also prefixes arrow keys. A short continuation window separates
     * a standalone Back action from a complete CSI or SS3 control sequence. */
    status = read_byte(&byte, ESCAPE_CONTINUATION_MS);
    if (status == 0)
        return NERO_UI_KEY_CANCEL;
    if (status < 0 || (byte != '[' && byte != 'O'))
        return NERO_UI_KEY_CANCEL;

    /* Bound malformed sequences so terminal input cannot monopolize the UI.
     * Consume parameter bytes through the final control byte before mapping. */
    char final = 0;
    for (int index = 0; index < 12; ++index) {
        status = read_byte(&byte, ESCAPE_CONTINUATION_MS);
        if (status <= 0)
            return NERO_UI_KEY_OTHER;
        if ((unsigned char)byte >= 0x40 && (unsigned char)byte <= 0x7e) {
            final = byte;
            break;
        }
    }
    if (final == 'A')
        return NERO_UI_KEY_UP;
    if (final == 'B')
        return NERO_UI_KEY_DOWN;
    return NERO_UI_KEY_OTHER;
}
