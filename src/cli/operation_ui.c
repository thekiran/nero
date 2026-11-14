/* Reusable terminal operation surface and keyboard decoder.
 * The caller owns operation state and rendering order; this module owns the
 * alternate-screen transition. Operations are sequential, not nested. */
#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include "nero/operation_ui.h"
#include "nero/ascii.h"

#include <errno.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif

#define ESCAPE_CONTINUATION_MS 35
#define OPERATION_DOT_COUNT 6

static int alternate_screen_active = 0;
static nero_operation_ui *current_ui;

/* Flicker-free frame cache for nero_operation_render_lines: the previously
 * drawn body rows plus the geometry they were drawn at. Reset on every
 * nero_operation_begin so a new surface always repaints in full. */
#define RENDER_MAX_ROWS 256
#define RENDER_MAX_COLS 1024
static char render_prev[RENDER_MAX_ROWS][RENDER_MAX_COLS];
static int render_prev_count = 0;
static int render_prev_rows = -1;
static int render_prev_cols = -1;

/* Live-discovery viewport state: follow the newest results until the user
 * scrolls up, then resume following once scrolled back to the bottom. Reset
 * per operation in nero_operation_begin. */
static int discovery_scroll = 0;
static int discovery_follow = 1;

/* The scrollbar sits directly beside the port table (whose widest line,
 * "  PORT     PROTOCOL     STATUS", ends at column 30) instead of at the far
 * screen edge, so the eye does not have to travel across an empty row. */
#define SCROLLBAR_COLUMN 33

/* Move the cursor to a 1-based (row, column) on the terminal. */
static void position(int row, int column)
{
    printf("\033[%d;%dH", row, column);
}

/* Case-insensitive "contains UTF-8/UTF8" without non-C11 strcasestr. */
static int names_utf8(const char *value)
{
    for (const char *p = value; *p; ++p) {
        if ((p[0] == 'u' || p[0] == 'U') && (p[1] == 't' || p[1] == 'T') &&
            (p[2] == 'f' || p[2] == 'F')) {
            const char *q = p + 3;
            if (*q == '-' || *q == '_') ++q;
            if (q[0] == '8') return 1;
        }
    }
    return 0;
}

/* Block and box glyphs are multi-byte UTF-8. On a non-UTF-8 locale (LC_ALL=C,
 * ISO-8859-x, some serial/legacy consoles) they print as several garbage
 * cells and break every column calculation, so fall back to plain ASCII.
 * NERO_ASCII forces the fallback, e.g. for CJK "ambiguous = wide" terminals
 * where these glyphs occupy two cells. Unset locale assumes modern UTF-8. */
static int glyphs_unicode(void)
{
    if (getenv("NERO_ASCII") != NULL) return 0;
    const char *names[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const char *value = getenv(names[i]);
        if (value && value[0]) return names_utf8(value);
    }
    return 1;
}

/* Pick the UTF-8 glyph on capable terminals, else the ASCII fallback. */
static const char *glyph(const nero_operation_ui *ui, const char *utf8, const char *ascii)
{
    return ui && ui->unicode ? utf8 : ascii;
}

/* Draw the fixed frame once: brand logo, title, detail and footer controls.
 * The operation body is painted separately and never overwrites these. */
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
    if (ui->unicode) {
        fputs(NERO_ASCII_LOGO, stdout);
    } else {
        position(6, 3);
        fputs("N E R O", stdout);
    }
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

/* Re-read the terminal size and, only when it changed, recompute the body
 * bounds and repaint the fixed frame. A no-op when the size is unchanged. */
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
    /* Also turn off mouse reporting here so any exit path (including the atexit
     * fallback after an error) never leaves the terminal emitting mouse codes
     * into the user's shell. Disabling an already-off mode is a harmless no-op. */
    fputs("\033[0m\033[?1006l\033[?1000l\033[?1049l", stdout);
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
#ifdef _WIN32
    /* Windows consoles carry no TERM convention; VT output is enabled by
     * nero_windows_init() so a live console is ANSI-capable by default. */
    ui->interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
                      (!term || strcmp(term, "dumb") != 0);
#else
    ui->interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
                      term && strcmp(term, "dumb") != 0;
#endif
    ui->color = ui->interactive && getenv("NO_COLOR") == NULL;
    ui->title = title;
    ui->detail = detail;
    ui->controls = controls;
    ui->unicode = glyphs_unicode();
    ui->rows = ui->columns = ui->scroll_offset = 0;
    discovery_scroll = 0;
    discovery_follow = 1;
    current_ui = ui;
    ui->active = 1;
    render_prev_count = 0;
    render_prev_rows = -1;
    render_prev_cols = -1;
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
    fputs(ui->unicode ? NERO_ASCII_LOGO : "  N E R O\n", stdout);
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

/* Print a section heading at the top of the operation body. */
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
        fputs(index == frame ? glyph(current_ui, "●", "o") : glyph(current_ui, "·", "."), stdout);
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

/* Flicker-free block renderer: draw `lines` into the body, rewriting only the
 * rows that changed since the previous frame. See the header for contract. */
void nero_operation_render_lines(nero_operation_ui *ui, const char *const *lines, int count,
                                 int scroll_offset)
{
    if (ui == NULL)
        return;
    if (!ui->interactive) {
        for (int i = 0; i < count; ++i)
            printf("%s\n", lines[i] != NULL ? lines[i] : "");
        fflush(stdout);
        return;
    }

    refresh_layout(ui);
    if (ui->compact) {
        fflush(stdout);
        render_prev_count = 0;
        render_prev_rows = -1;
        return;
    }

    int body_rows = ui->content_bottom - ui->content_top + 1;
    if (body_rows < 1)
        body_rows = 1;
    if (body_rows > RENDER_MAX_ROWS)
        body_rows = RENDER_MAX_ROWS;

    int max_offset = count - body_rows;
    if (max_offset < 0)
        max_offset = 0;
    if (scroll_offset < 0)
        scroll_offset = 0;
    if (scroll_offset > max_offset)
        scroll_offset = max_offset;

    /* A geometry change (first frame after begin, or a resize that repainted
     * the fixed frame) invalidates the cache and forces a full repaint. */
    int full = (ui->rows != render_prev_rows || ui->columns != render_prev_cols);
    if (full) {
        render_prev_count = 0;
        render_prev_rows = ui->rows;
        render_prev_cols = ui->columns;
    }

    for (int r = 0; r < body_rows; ++r) {
        int src = scroll_offset + r;
        const char *line = (src < count && lines[src] != NULL) ? lines[src] : "";
        char current[RENDER_MAX_COLS];
        snprintf(current, sizeof(current), "%s", line);
        if (full || r >= render_prev_count || strcmp(current, render_prev[r]) != 0) {
            position(ui->content_top + r, 1);
            fputs("\033[2K", stdout);
            position(ui->content_top + r, 3);
            fputs(current, stdout);
            snprintf(render_prev[r], sizeof(render_prev[r]), "%s", current);
        }
    }
    render_prev_count = body_rows;
    /* Park the cursor on the footer row so a stray terminal echo cannot land in
     * the middle of the table. */
    position(ui->rows, 1);
    fflush(stdout);
}

/* Close the operation surface: restore the alternate screen (interactive) or
 * emit a trailing newline (redirected). Idempotent via ui->active. */
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
/* Well-known TCP service names for the SERVICE column. Static lookup only,
 * no network traffic. Unknown ports return "" (blank). */
const char *nero_service_name(int port)
{
    switch (port) {
    case 20: case 21: return "ftp";
    case 22: return "ssh";
    case 23: return "telnet";
    case 25: return "smtp";
    case 53: return "dns";
    case 67: case 68: return "dhcp";
    case 69: return "tftp";
    case 80: return "http";
    case 110: return "pop3";
    case 111: return "rpcbind";
    case 123: return "ntp";
    case 135: return "msrpc";
    case 137: case 138: case 139: return "netbios";
    case 143: return "imap";
    case 161: case 162: return "snmp";
    case 389: return "ldap";
    case 443: return "https";
    case 445: return "smb";
    case 465: return "smtps";
    case 514: return "syslog";
    case 515: return "printer";
    case 548: return "afp";
    case 554: return "rtsp";
    case 587: return "submission";
    case 631: return "ipp";
    case 636: return "ldaps";
    case 993: return "imaps";
    case 995: return "pop3s";
    case 1080: return "socks";
    case 1433: return "mssql";
    case 1521: return "oracle";
    case 1723: return "pptp";
    case 1883: return "mqtt";
    case 2049: return "nfs";
    case 3128: return "proxy";
    case 3306: return "mysql";
    case 3389: return "rdp";
    case 5000: return "upnp";
    case 5060: return "sip";
    case 5353: return "mdns";
    case 5432: return "postgres";
    case 5900: case 5901: return "vnc";
    case 6379: return "redis";
    case 7000: return "airplay";
    case 8000: case 8080: case 8888: return "http-alt";
    case 8443: return "https-alt";
    case 9000: return "http-alt";
    case 9200: return "elastic";
    case 11211: return "memcache";
    case 27017: return "mongodb";
    case 62078: return "lockdownd"; /* iOS usbmux */
    default: return "";
    }
}

/* Column header shared by the live discovery view and the selection menu. */
static void port_table_header(nero_operation_ui *ui, int row)
{
    if (ui->interactive) position(row, 1);
    else putchar('\n');
    if (ui->color) fputs("\033[2m", stdout);
    printf("  %-8s%-14s%s", "PORT", "SERVICE", "STATUS");
    if (ui->color) fputs("\033[0m", stdout);
}

/* One port row: number, well-known service name (or "-"), and OPEN status. */
static void port_table_row(nero_operation_ui *ui, int row, int port, int selected)
{
    const char *svc = nero_service_name(port);
    char svc_field[14];
    /* Unknown ports show a dim dash so the column never looks broken. */
    snprintf(svc_field, sizeof svc_field, "%-13s", svc[0] ? svc : "-");
    if (ui->interactive) position(row, 1);
    else putchar('\n');
    if (ui->color) fputs(selected ? "\033[1;35m" : "\033[1m", stdout);
    printf("%s %-8d", selected ? ">" : " ", port);
    if (ui->color) fputs("\033[0;36m", stdout);
    printf("%s", svc_field);
    if (ui->color) fputs("\033[0;35m", stdout);
    fputs("OPEN", stdout);
    if (ui->color) fputs("\033[0m", stdout);
}

/* Vertical scrollbar directly beside the port table: a dim track with an
 * accent thumb, shown only when the list is taller than its viewport.
 * first_row is the top list row; visible is how many rows fit; offset is the
 * current top index. */
static void draw_scrollbar(nero_operation_ui *ui, int first_row, int visible,
                           int count, int offset)
{
    if (!ui->interactive || ui->compact || visible <= 0 || count <= visible)
        return;
    int bar_col = SCROLLBAR_COLUMN;
    if (bar_col > ui->columns - 1) bar_col = ui->columns - 1;
    int max_off = count - visible;
    /* Explicit denominator invariant for Clang's path-sensitive analyzer. */
    if (max_off <= 0) return;
    if (offset < 0) offset = 0;
    if (offset > max_off) offset = max_off;

    /* Thumb length is proportional to the visible share (rounded, >= 1). */
    int thumb = (int)(((long)visible * visible + count / 2) / count);
    if (thumb < 1) thumb = 1;
    if (thumb > visible) thumb = visible;
    int track = visible - thumb;
    int thumb_pos = track > 0 ? (int)(((long)offset * track + max_off / 2) / max_off) : 0;
    /* The thumb touches an end only when the list really is at that end, so
     * "there is more above/below" is never hidden by rounding. */
    if (track > 1) {
        if (offset > 0 && thumb_pos == 0) thumb_pos = 1;
        if (offset < max_off && thumb_pos == track) thumb_pos = track - 1;
    }

    for (int r = 0; r < visible; ++r) {
        int on = (r >= thumb_pos && r < thumb_pos + thumb);
        position(first_row + r, bar_col);
        if (ui->color) fputs(on ? "\033[35m" : "\033[2m", stdout);
        fputs(on ? glyph(ui, "█", "#") : glyph(ui, "│", "|"), stdout);
        if (ui->color) fputs("\033[0m", stdout);
    }
}

/* Render the interactive port-selection list: heading, columns, scrollable
 * rows, scrollbar, and — when ui->banner_* is set — a banner detail line for
 * the selected port. See the header for the public contract. */
void nero_operation_ports(nero_operation_ui *ui, const int *ports, int count, int selected)
{
    ui->controls = ui->unicode
        ? "↑↓ PgUp PgDn · Move   Enter · Select   Esc · Back"
        : "Up/Down PgUp/PgDn: Move   Enter: Select   Esc: Back";
    if (!body(ui)) return;
    if (ui->interactive) {
        /* Discovery parked a progress bar on the last row and the controls one
         * row above it; clear both before the selection menu redraws. */
        position(ui->rows - 1, 1); fputs("\033[2K", stdout);
        position(ui->rows, 1); fputs("\033[2K", stdout);
        /* Clearing the footer moved the cursor to the last row; without this
         * the "Choose a port" heading was printed there and then overwritten
         * by the controls, leaving a stray "Ch" in the bottom-left corner. */
        position(ui->content_top, 3);
    }
    if (count < 0) count = 0;
    if (selected >= count) selected = count - 1;
    if (selected < 0) selected = 0;
    int visible = ui->interactive ? ui->content_bottom - ui->content_top - 2 : count;
    if (visible < 1) visible = 1;
    if (selected < ui->scroll_offset) ui->scroll_offset = selected;
    if (selected >= ui->scroll_offset + visible) ui->scroll_offset = selected - visible + 1;
    /* After a resize to a taller window, pull the window back down so rows are
     * never left empty below the last item while items above stay hidden. */
    int max_off = count > visible ? count - visible : 0;
    if (ui->scroll_offset > max_off) ui->scroll_offset = max_off;
    if (ui->scroll_offset < 0) ui->scroll_offset = 0;
    if (ui->color) fputs("\033[1m", stdout);
    printf("Choose a port");
    if (ui->color) fputs("\033[0;2m", stdout);
    printf("     %d / %d", count ? selected + 1 : 0, count);
    if (ui->color) fputs("\033[0m", stdout);
    port_table_header(ui, ui->content_top + 2);
    for (int index = 0; index < visible && ui->scroll_offset + index < count; ++index) {
        int item = ui->scroll_offset + index;
        port_table_row(ui, ui->content_top + index + 3, ports[item], item == selected);
    }
    draw_scrollbar(ui, ui->content_top + 3, visible, count, ui->scroll_offset);
    /* Banner/version detail for the selected port on the second-to-last row,
     * looked up by port value (independent of the list's sort order). */
    if (ui->interactive && ui->banner_count > 0 && ui->banner_ports &&
        ui->banner_texts && count > 0) {
        const char *banner = NULL;
        for (int i = 0; i < ui->banner_count; ++i) {
            if (ui->banner_ports[i] == ports[selected]) {
                banner = ui->banner_texts[i];
                break;
            }
        }
        position(ui->rows - 1, 1); fputs("\033[2K", stdout);
        if (banner && banner[0]) {
            position(ui->rows - 1, 3);
            if (ui->color) fputs("\033[2m", stdout);
            printf("%.*s", ui->columns - 4, banner);
            if (ui->color) fputs("\033[0m", stdout);
        }
    }
    if (ui->interactive) {
        position(ui->rows, 3);
        printf("%.*s", ui->columns - 4, ui->controls);
    }
    fflush(stdout);
}

void nero_operation_menu(nero_operation_ui *ui, const char *heading,
                         const char *const *labels, const char *const *values,
                         int count, int selected)
{
    if (!body(ui)) return;
    if (count < 0) count = 0;
    if (selected >= count) selected = count - 1;
    if (selected < 0) selected = 0;

    int row = ui->content_top;
    if (heading && heading[0]) {
        if (ui->interactive) position(row, 3);
        if (ui->color) fputs("\033[1m", stdout);
        printf("%.*s", ui->columns - 4, heading);
        if (ui->color) fputs("\033[0m", stdout);
        if (!ui->interactive) putchar('\n');
        row += 2;
    }
    int first_row = row;
    int visible = ui->interactive ? ui->content_bottom - first_row + 1 : count;
    if (visible < 1) visible = 1;

    if (selected < ui->scroll_offset) ui->scroll_offset = selected;
    if (selected >= ui->scroll_offset + visible) ui->scroll_offset = selected - visible + 1;
    int max_off = count > visible ? count - visible : 0;
    if (ui->scroll_offset > max_off) ui->scroll_offset = max_off;
    if (ui->scroll_offset < 0) ui->scroll_offset = 0;

    for (int i = 0; i < visible && ui->scroll_offset + i < count; ++i) {
        int item = ui->scroll_offset + i;
        int sel = item == selected;
        if (ui->interactive) position(first_row + i, 1);
        else putchar('\n');
        if (ui->color) fputs(sel ? "\033[1;35m" : "\033[0m", stdout);
        printf("%s %s", sel ? ">" : " ", labels[item] ? labels[item] : "");
        if (values && values[item]) {
            if (ui->color) fputs(sel ? "\033[0;35m" : "\033[0;2m", stdout);
            printf("   %s", values[item]);
        }
        if (ui->color) fputs("\033[0m", stdout);
    }
    draw_scrollbar(ui, first_row, visible, count, ui->scroll_offset);
    if (ui->interactive && ui->controls && ui->controls[0]) {
        position(ui->rows, 3);
        if (ui->color) fputs("\033[2m", stdout);
        printf("%.*s", ui->columns - 4, ui->controls);
        if (ui->color) fputs("\033[0m", stdout);
    }
    fflush(stdout);
}

/* Legacy three-row connection-test frames; falls back to a plain text counter
 * when color, Unicode or animation is unavailable. */
void nero_operation_frame(nero_operation_ui *ui, const char *frame, unsigned attempt)
{
    if (!body(ui)) return;
    if (ui->color && ui->unicode && getenv("NERO_REDUCE_MOTION") == NULL)
        fputs(frame, stdout); /* Existing frames occupy three bounded body rows. */
    else
        printf("Connection test %u/4", attempt);
    fflush(stdout);
}

/* Full-width progress bar on the terminal's last row. Filled cells use the
 * UI accent; the percentage is printed at the right. The last column is left
 * untouched so writing the bar never triggers an auto-wrap scroll. */
static void draw_progress_bar(nero_operation_ui *ui, int permille)
{
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;

    /* One decimal place (permille/10) keeps the readout honest and precise
     * instead of snapping between whole percents. */
    char suffix[12];
    int suffix_len = snprintf(suffix, sizeof(suffix), " %3d.%d%%",
                              permille / 10, permille % 10);
    int usable = ui->columns - 1;            /* keep the final column empty */
    int bar_cells = usable - suffix_len;
    if (bar_cells < 1) return;               /* too narrow for a meaningful bar */
    int filled = (int)((long)bar_cells * permille / 1000);
    const char *full = glyph(ui, "█", "#");
    const char *empty = glyph(ui, "░", "-");

    position(ui->rows, 1);
    fputs("\033[2K", stdout);
    if (ui->color) fputs("\033[35m", stdout);
    for (int cell = 0; cell < filled; ++cell) fputs(full, stdout);
    if (ui->color) fputs("\033[0;2m", stdout);
    for (int cell = filled; cell < bar_cells; ++cell) fputs(empty, stdout);
    if (ui->color) fputs("\033[0m", stdout);
    fputs(suffix, stdout);
}

/* Convert line/page/edge units (see NERO_UI_SCROLL_*) into list lines. */
static long scroll_lines(int delta, int visible)
{
    if (delta >= NERO_UI_SCROLL_EDGE / 2 || delta <= -NERO_UI_SCROLL_EDGE / 2)
        return delta > 0 ? 0x7fffffffL : -0x7fffffffL;
    long pages = delta / NERO_UI_SCROLL_PAGE;
    long lines = delta % NERO_UI_SCROLL_PAGE;
    return pages * (visible > 1 ? visible - 1 : 1) + lines;
}

void nero_operation_discovery(const int *ports, int count, int total,
                              unsigned frame, int latest_port, int permille,
                              int scroll_delta)
{
    nero_operation_loader("Scanning", NULL, frame);
    nero_operation_ui *ui = current_ui;
    if (!ui || !ui->interactive || ui->compact) return;
    if (count < 0) count = 0;

    int visible = ui->content_bottom - ui->content_top - 3;
    if (visible < 1) visible = 1;

    /* Follow the newest results until the user scrolls up with the wheel,
     * arrows or PgUp; resume following once they scroll back to the bottom
     * (or press End). Clamp in long so huge page/edge deltas cannot overflow. */
    int max_off = count > visible ? count - visible : 0;
    if (scroll_delta != 0 && max_off > 0) {
        long next = (long)discovery_scroll + scroll_lines(scroll_delta, visible);
        if (discovery_follow) next = (long)max_off + scroll_lines(scroll_delta, visible);
        if (next < 0) next = 0;
        discovery_follow = next >= max_off;
        discovery_scroll = next >= max_off ? max_off : (int)next;
    }
    if (discovery_follow || discovery_scroll > max_off)
        discovery_scroll = max_off;

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
    /* Where the viewport is, and whether it stopped following new results. */
    if (max_off > 0) {
        int last = discovery_scroll + visible;
        if (last > count) last = count;
        printf("   %d-%d / %d", discovery_scroll + 1, last, count);
        if (!discovery_follow) fputs("   End: follow", stdout);
    }
    if (ui->color) fputs("\033[0m", stdout);
    port_table_header(ui, ui->content_top + 3);

    for (int index = 0; index < visible && discovery_scroll + index < count; ++index) {
        int item = discovery_scroll + index;
        port_table_row(ui, ui->content_top + 4 + index, ports[item], 0);
    }
    draw_scrollbar(ui, ui->content_top + 4, visible, count, discovery_scroll);

    /* Controls hint on the second-to-last row, with the full-width progress
     * bar pinned to the very bottom row, directly beneath it. */
    const char *hint = ui->unicode ? "↑↓ PgUp PgDn · Scroll   Esc · Back"
                                   : "Up/Down PgUp/PgDn: Scroll   Esc: Back";
    position(ui->rows - 1, 1);
    fputs("\033[2K", stdout);
    position(ui->rows - 1, 3);
    if (ui->color) fputs("\033[2m", stdout);
    printf("%.*s", ui->columns - 4, hint);
    if (ui->color) fputs("\033[0m", stdout);
    draw_progress_bar(ui, permille);
    fflush(stdout);
}

/* Mouse wheel buttons carry modifier bits (Shift 4, Meta 8, Ctrl 16) and
 * some terminals set the motion bit (32); strip them before matching. */
static nero_ui_key wheel_key(int button)
{
    if (button < 64 || button > 127) return NERO_UI_KEY_NONE;
    switch (button & ~(4 | 8 | 16 | 32)) {
    case 64: return NERO_UI_KEY_WHEEL_UP;
    case 65: return NERO_UI_KEY_WHEEL_DOWN;
    default: return NERO_UI_KEY_NONE; /* 66/67: horizontal wheel */
    }
}

/* Read and decode one key or mouse-wheel event. See the header for the timeout
 * contract; the body documents the Escape/CSI/SS3/mouse sequence handling. */
nero_ui_key nero_operation_read_key(int timeout_ms)
{
    return nero_operation_read_key_ex(timeout_ms, NULL);
}

nero_ui_key nero_operation_read_key_ex(int timeout_ms, int *first_byte)
{
    char byte;
    if (first_byte != NULL)
        *first_byte = 0;
    int status = read_byte(&byte, timeout_ms);
    if (status < 0)
        return NERO_UI_KEY_EOF;
    if (status == 0)
        return NERO_UI_KEY_NONE;
    if (first_byte != NULL)
        *first_byte = (unsigned char)byte;
    if (byte == 'q' || byte == 'Q' || byte == 3)
        return NERO_UI_KEY_CANCEL;
    if (byte == '\n' || byte == '\r')
        return NERO_UI_KEY_ENTER;
    /* vi-style keys for terminals/multiplexers that swallow arrow keys. */
    if (byte == 'k') return NERO_UI_KEY_UP;
    if (byte == 'j') return NERO_UI_KEY_DOWN;
    if ((unsigned char)byte != 27)
        return NERO_UI_KEY_OTHER;

    /* Escape also prefixes arrow keys. A short continuation window separates
     * a standalone Back action from a complete CSI or SS3 control sequence. */
    status = read_byte(&byte, ESCAPE_CONTINUATION_MS);
    if (status == 0)
        return NERO_UI_KEY_CANCEL;
    if (status < 0 || (byte != '[' && byte != 'O'))
        return NERO_UI_KEY_CANCEL;
    int ss3 = byte == 'O';

    /* Bound malformed sequences so terminal input cannot monopolize the UI.
     * Capture parameter bytes up to the final control byte, then map arrows,
     * paging keys and mouse-wheel reports. */
    char params[24];
    int params_len = 0;
    char final = 0;
    for (int index = 0; index < 32; ++index) {
        status = read_byte(&byte, ESCAPE_CONTINUATION_MS);
        if (status <= 0)
            return NERO_UI_KEY_OTHER;
        if ((unsigned char)byte >= 0x40 && (unsigned char)byte <= 0x7e) {
            final = byte;
            break;
        }
        if (params_len < (int)sizeof(params) - 1)
            params[params_len++] = byte;
    }
    params[params_len] = '\0';
    if (final == 0)
        return NERO_UI_KEY_OTHER;

    /* X10/normal mouse report (terminals without SGR 1006 support, e.g. older
     * Terminal.app/PuTTY): ESC [ M followed by three raw bytes (button+32,
     * col+32, row+32). Always consume all three so they never leak back as
     * stray keystrokes. */
    if (!ss3 && final == 'M' && params_len == 0) {
        char mb;
        if (read_byte(&mb, ESCAPE_CONTINUATION_MS) <= 0)
            return NERO_UI_KEY_NONE;
        int button = (int)(unsigned char)mb - 32;
        if (read_byte(&mb, ESCAPE_CONTINUATION_MS) <= 0 ||
            read_byte(&mb, ESCAPE_CONTINUATION_MS) <= 0)
            return NERO_UI_KEY_NONE;
        return wheel_key(button);
    }

    /* SGR mouse report: ESC [ < button ; col ; row M|m. Only wheel presses
     * scroll; clicks, releases and motion are ignored. */
    if (params_len > 0 && params[0] == '<')
        return final == 'M' ? wheel_key(atoi(params + 1)) : NERO_UI_KEY_NONE;

    /* urxvt 1015 mouse report: ESC [ button+32 ; col ; row M. */
    if (!ss3 && final == 'M' && params_len > 0)
        return wheel_key(atoi(params) - 32);

    if (ss3 && final == 'M')
        return NERO_UI_KEY_ENTER; /* keypad Enter in application keypad mode */

    switch (final) {
    case 'A': return NERO_UI_KEY_UP;   /* CSI A, CSI 1;5A, SS3 A */
    case 'B': return NERO_UI_KEY_DOWN;
    case 'C': return NERO_UI_KEY_RIGHT;
    case 'D': return NERO_UI_KEY_LEFT;
    case 'H': return NERO_UI_KEY_HOME; /* CSI H, SS3 H */
    case 'F': return NERO_UI_KEY_END;  /* CSI F, SS3 F */
    case '~':
        /* VT220 editing keys; 1/7 Home and 4/8 End vary by terminal family. */
        switch (atoi(params)) {
        case 1: case 7: return NERO_UI_KEY_HOME;
        case 4: case 8: return NERO_UI_KEY_END;
        case 5: return NERO_UI_KEY_PAGE_UP;
        case 6: return NERO_UI_KEY_PAGE_DOWN;
        default: return NERO_UI_KEY_OTHER;
        }
    default:
        return NERO_UI_KEY_OTHER;
    }
}
