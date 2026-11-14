/* Shared, capability-gated terminal table engine. See nero/table.h. */

#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif

#include "nero/table.h"
#include "nero/terminal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(_WIN32)
#include <sys/ioctl.h>
#endif

/* ---- device adaptivity: terminal width --------------------------------- *
 * A table never assumes 80 columns. The usable width comes from COLUMNS (so
 * pipes, tmux and tests can pin it), then the live TTY size, then a safe 80.
 * Everything downstream fits inside this number, so the grid stays aligned on
 * a phone SSH client, a CI log pane or an ultrawide monitor alike. */
int nero_terminal_width(void)
{
    const char *env = getenv("COLUMNS");
    if (env != NULL && env[0] != '\0') {
        int value = atoi(env);
        if (value >= 20 && value <= 1000)
            return value;
    }
#if !defined(_WIN32)
    struct winsize ws;
    if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20)
        return (int)ws.ws_col;
#endif
    return 80;
}

void nero_table_opts_default(nero_table_opts *opts)
{
    if (opts == NULL)
        return;
    opts->color = terminal_color_enabled();
    opts->unicode = terminal_unicode_enabled();
    opts->max_width = NERO_TABLE_DEFAULT_MAX;
    opts->width = 0;
}

/* ---- Unicode-aware measurement ----------------------------------------- *
 * Column widths are measured in display cells, not bytes, so accented Latin,
 * CJK and emoji line up instead of shearing the grid. */
static int table_utf8_next(const char *s, unsigned int *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char c = p[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c >> 5) == 0x6 && (p[1] & 0xC0) == 0x80) {
        *cp = (unsigned int)((c & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((c >> 4) == 0xE && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = (unsigned int)((c & 0x0F) << 12) | (unsigned int)((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((c >> 3) == 0x1E && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *cp = (unsigned int)((c & 0x07) << 18) | (unsigned int)((p[1] & 0x3F) << 12) |
              (unsigned int)((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD; /* invalid lead byte: consume one, render as width 1 */
    return 1;
}

static int table_wcwidth(unsigned int cp)
{
    if (cp == 0)
        return 0;
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 || cp == 0xFEFF)
        return 0;
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) ||
        (cp >= 0x0591 && cp <= 0x05BD) || (cp >= 0x0610 && cp <= 0x061A) ||
        (cp >= 0x064B && cp <= 0x065F) || (cp >= 0x06D6 && cp <= 0x06DC) ||
        (cp >= 0x0E31 && cp <= 0x0E3A) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F))
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F000 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

int nero_display_width(const char *s)
{
    int width = 0;
    while (*s) {
        unsigned int cp;
        int n = table_utf8_next(s, &cp);
        if (n <= 0)
            break;
        width += table_wcwidth(cp);
        s += n;
    }
    return width;
}

int nero_fit(const char *s, int maxw, int ascii, char *out, size_t outlen)
{
    size_t o = 0;
    int used = 0;
    if (maxw < 1 || outlen == 0) {
        if (outlen)
            out[0] = '\0';
        return 0;
    }
    int total = nero_display_width(s);
    int clip = total > maxw;
    int limit = clip ? maxw - 1 : maxw; /* reserve one cell for the ellipsis */
    while (*s) {
        unsigned int cp;
        int n = table_utf8_next(s, &cp);
        if (n <= 0)
            break;
        int w = table_wcwidth(cp);
        if (used + w > limit)
            break;
        if (o + (size_t)n + 4 >= outlen)
            break;
        memcpy(out + o, s, (size_t)n);
        o += (size_t)n;
        s += n;
        used += w;
    }
    if (clip) {
        const char *ell = ascii ? "." : "\xE2\x80\xA6";
        size_t el = strlen(ell);
        if (o + el < outlen) {
            memcpy(out + o, ell, el);
            o += el;
            used += 1;
        }
    }
    out[o] = '\0';
    return used;
}

/* Maps an RSSI to a 1..4 strength level and a traffic-light colour. Level 0
 * means "no signal information was captured" and is never shown as weak. */
int nero_signal_level(int has_rssi, int rssi, const char **color)
{
    if (!has_rssi) {
        *color = NERO_C_DIM;
        return 0;
    }
    if (rssi >= -55) {
        *color = NERO_C_GREEN;
        return 4;
    }
    if (rssi >= -67) {
        *color = NERO_C_GREEN;
        return 3;
    }
    if (rssi >= -75) {
        *color = NERO_C_YELLOW;
        return 2;
    }
    *color = NERO_C_RED;
    return 1;
}

/* ---- render context ---------------------------------------------------- */

typedef struct {
    const char *h, *v, *tl, *tr, *bl, *br, *lt, *rt, *tt, *bt, *x;
} table_box;

typedef struct {
    FILE *out;
    int color;
    int unicode;
    int width; /* resolved, capped available width */
    table_box box;
} table_ctx;

/* Returns the colour escape when colour is enabled, else an empty string. */
static const char *tc(const table_ctx *c, const char *code) { return c->color ? code : ""; }
static const char *tc_off(const table_ctx *c) { return c->color ? NERO_C_RESET : ""; }

static table_box table_box_glyphs(int unicode)
{
    if (unicode) {
        table_box u = {"\xE2\x94\x80", "\xE2\x94\x82", "\xE2\x94\x8C", "\xE2\x94\x90",
                       "\xE2\x94\x94", "\xE2\x94\x98", "\xE2\x94\x9C", "\xE2\x94\xA4",
                       "\xE2\x94\xAC", "\xE2\x94\xB4", "\xE2\x94\xBC"};
        return u;
    }
    table_box a = {"-", "|", "+", "+", "+", "+", "+", "+", "+", "+", "+"};
    return a;
}

/* Horizontal border: left/mid/right choose the junction glyphs so one helper
 * draws the top (tl/tt/tr), divider (lt/x/rt) and bottom (bl/bt/br) rules. */
static void table_rule(const table_ctx *c, const char *left, const char *mid, const char *right,
                       const int *widths, int columns)
{
    fputs(tc(c, NERO_C_DIM), c->out);
    fputs(left, c->out);
    for (int i = 0; i < columns; ++i) {
        for (int k = 0; k < widths[i] + 2; ++k)
            fputs(c->box.h, c->out);
        fputs(i + 1 < columns ? mid : right, c->out);
    }
    fputs(tc_off(c), c->out);
    fputc('\n', c->out);
}

static void table_vbar(const table_ctx *c)
{
    fputs(tc(c, NERO_C_DIM), c->out);
    fputs(c->box.v, c->out);
    fputs(tc_off(c), c->out);
}

/* One padded cell. Text is clipped/padded to the visible width in display
 * cells, then colour wraps the already-sized text so alignment never drifts. */
static void table_cell(const table_ctx *c, const char *text, int width, int right,
                       const char *color)
{
    char buf[256];
    int used = nero_fit(text, width, !c->unicode, buf, sizeof(buf));
    int pad = width - used;
    if (pad < 0)
        pad = 0;
    table_vbar(c);
    fputc(' ', c->out);
    if (color)
        fputs(tc(c, color), c->out);
    if (right)
        for (int k = 0; k < pad; ++k)
            fputc(' ', c->out);
    fputs(buf, c->out);
    if (!right)
        for (int k = 0; k < pad; ++k)
            fputc(' ', c->out);
    if (color)
        fputs(tc_off(c), c->out);
    fputc(' ', c->out);
}

static void table_row_end(const table_ctx *c)
{
    table_vbar(c);
    fputc('\n', c->out);
}

/* Signal column: a four-segment meter plus the raw dBm, total visible width 9
 * so it aligns like any other fixed cell. */
static void table_signal_cell(const table_ctx *c, int has_rssi, int rssi)
{
    static const char *bars_u[4] = {"\xE2\x96\x82", "\xE2\x96\x84", "\xE2\x96\x86", "\xE2\x96\x88"};
    const char *color = NERO_C_DIM;
    int level = nero_signal_level(has_rssi, rssi, &color);
    char dbm[8];
    if (has_rssi)
        snprintf(dbm, sizeof(dbm), "%d", rssi);
    else
        snprintf(dbm, sizeof(dbm), "--");

    table_vbar(c);
    fputc(' ', c->out);
    for (int i = 0; i < 4; ++i) {
        int on = i < level;
        fputs(tc(c, on ? color : NERO_C_DIM), c->out);
        if (c->unicode)
            fputs(on ? bars_u[i] : "\xE2\x96\x81", c->out);
        else
            fputc(on ? '#' : '.', c->out);
        fputs(tc_off(c), c->out);
    }
    fprintf(c->out, " %s%4s%s ", tc(c, color), dbm, tc_off(c));
}

/* ---- responsive column layout ------------------------------------------ */

static int table_total(const int *w, const int *vis, int n)
{
    int cols = 0, content = 0;
    for (int i = 0; i < n; ++i) {
        if (!vis[i])
            continue;
        ++cols;
        content += w[i];
    }
    if (cols == 0)
        return 0;
    return content + (cols + 1) + cols * 2; /* verticals + one pad cell each side */
}

/* Fills w[] and vis[] for the given terminal width. */
static void table_layout(const nero_table_col *c, int n, int termw, int *w, int *vis)
{
    for (int i = 0; i < n; ++i) {
        vis[i] = 1;
        w[i] = c[i].max;
    }
    for (;;) {
        int total = table_total(w, vis, n);
        if (total <= termw) {
            int slack = termw - total;
            for (int i = 0; i < n && slack > 0; ++i) {
                if (!vis[i] || !c[i].flex)
                    continue;
                int room = c[i].grow < slack ? c[i].grow : slack;
                w[i] += room;
                slack -= room;
            }
            return;
        }
        /* shrink flex columns toward their minimum first */
        int progressed = 0;
        for (int i = 0; i < n && total > termw; ++i) {
            if (vis[i] && c[i].flex && w[i] > c[i].min) {
                int need = total - termw;
                int can = w[i] - c[i].min;
                int d = need < can ? need : can;
                w[i] -= d;
                total -= d;
                progressed = 1;
            }
        }
        if (total <= termw)
            continue;
        /* drop the lowest-priority droppable column */
        int drop = -1, best = (1 << 30);
        for (int i = 0; i < n; ++i) {
            if (vis[i] && c[i].prio > 0 && c[i].prio < best) {
                best = c[i].prio;
                drop = i;
            }
        }
        if (drop < 0) {
            if (!progressed) {
                /* nothing left to shrink or drop: clamp flex columns hard */
                for (int i = 0; i < n; ++i)
                    if (vis[i] && c[i].flex && w[i] > 3)
                        w[i] = 3;
            }
            return;
        }
        vis[drop] = 0;
    }
}

static int table_visible_widths(const int *w, const int *vis, int n, int *out)
{
    int m = 0;
    for (int i = 0; i < n; ++i)
        if (vis[i])
            out[m++] = w[i];
    return m;
}

static void table_header(const table_ctx *c, const nero_table_col *cols, const int *w,
                         const int *vis, int n)
{
    for (int i = 0; i < n; ++i)
        if (vis[i])
            table_cell(c, cols[i].title, w[i], cols[i].right, NERO_C_HEAD);
    table_row_end(c);
}

static void table_row(const table_ctx *c, const nero_table_col *cols, const int *w, const int *vis,
                      int n, const nero_table_cell *v)
{
    for (int i = 0; i < n; ++i) {
        if (!vis[i])
            continue;
        if (cols[i].kind == NERO_TABLE_SIGNAL)
            table_signal_cell(c, v[i].has_rssi, v[i].rssi);
        else
            table_cell(c, v[i].text, w[i], cols[i].right, v[i].color);
    }
    table_row_end(c);
}

/* Prints a dim, word-wrapped note so even the disclosure line fits the live
 * terminal width instead of wrapping mid-column. */
static void table_note(const table_ctx *c, const char *text)
{
    int width = c->width;
    int col = 2;
    const char *indent = "  ";

    fprintf(c->out, "%s%s", tc(c, NERO_C_DIM), indent);
    while (*text != '\0') {
        while (*text == ' ')
            ++text;
        if (*text == '\0')
            break;
        const char *start = text;
        while (*text != '\0' && *text != ' ')
            ++text;
        size_t len = (size_t)(text - start);
        if (len > 96)
            len = 96;
        char word[100];
        memcpy(word, start, len);
        word[len] = '\0';
        int word_w = nero_display_width(word);
        int sep = col > 2 ? 1 : 0;
        if (col + sep + word_w > width && col > 2) {
            fprintf(c->out, "\n%s", indent);
            col = 2;
            sep = 0;
        }
        if (sep) {
            fputc(' ', c->out);
            ++col;
        }
        fputs(word, c->out);
        col += word_w;
    }
    fprintf(c->out, "%s\n", tc_off(c));
}

/* Honest disclosure when the terminal was too narrow to show every column. */
static void table_dropped_note(const table_ctx *c, const nero_table_col *cols, const int *vis, int n)
{
    char list[192] = "";
    char note[256];
    int any = 0;
    for (int i = 0; i < n; ++i) {
        if (vis[i])
            continue;
        if (any)
            strncat(list, ", ", sizeof(list) - strlen(list) - 1);
        strncat(list, cols[i].title, sizeof(list) - strlen(list) - 1);
        any = 1;
    }
    if (any) {
        snprintf(note, sizeof(note),
                 "(narrow view: %s hidden %s widen the terminal to show every column)", list,
                 c->unicode ? "\xE2\x80\x94" : "-");
        table_note(c, note);
    }
}

void nero_table_draw(FILE *out, const nero_table_opts *opts, const nero_table_col *cols, int ncols,
                     int nrows, nero_table_row_fn fn, void *ctx)
{
    nero_table_opts local;
    if (opts == NULL) {
        nero_table_opts_default(&local);
        opts = &local;
    }
    if (ncols > NERO_TABLE_MAX_COLS)
        ncols = NERO_TABLE_MAX_COLS;

    int max_width = opts->max_width > 0 ? opts->max_width : NERO_TABLE_DEFAULT_MAX;
    int width = opts->width > 0 ? opts->width : nero_terminal_width();
    if (width > max_width)
        width = max_width;

    table_ctx c;
    c.out = out;
    c.color = opts->color;
    c.unicode = opts->unicode;
    c.width = width;
    c.box = table_box_glyphs(opts->unicode);

    int w[NERO_TABLE_MAX_COLS], vis[NERO_TABLE_MAX_COLS], vw[NERO_TABLE_MAX_COLS];
    table_layout(cols, ncols, width, w, vis);
    int m = table_visible_widths(w, vis, ncols, vw);
    table_rule(&c, c.box.tl, c.box.tt, c.box.tr, vw, m);
    table_header(&c, cols, w, vis, ncols);
    table_rule(&c, c.box.lt, c.box.x, c.box.rt, vw, m);
    for (int r = 0; r < nrows; ++r) {
        nero_table_cell v[NERO_TABLE_MAX_COLS];
        for (int i = 0; i < ncols; ++i) {
            v[i].text[0] = '\0';
            v[i].color = NULL;
            v[i].has_rssi = 0;
            v[i].rssi = 0;
        }
        fn(r, ctx, v);
        table_row(&c, cols, w, vis, ncols, v);
    }
    table_rule(&c, c.box.bl, c.box.bt, c.box.br, vw, m);
    table_dropped_note(&c, cols, vis, ncols);
}
