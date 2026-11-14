#ifndef NERO_TABLE_H
#define NERO_TABLE_H

#include <stdio.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- *
 *  Shared, capability-gated terminal table engine.
 *
 *  One responsive grid renderer used by every NERO surface (Wi-Fi survey,
 *  live monitor, future scanners). Column widths are measured in display
 *  cells, not bytes, so accented Latin, CJK and emoji line up. Box-drawing
 *  and colour are gated: on NO_COLOR, non-ANSI or non-UTF-8 terminals the
 *  output falls back to a plain ASCII grid so it stays copy-pasteable and
 *  scriptable. Colours encode meaning only; they never invent data.
 *
 *  The engine renders into any FILE*, so the same column specs feed both the
 *  inline scrollback path (out = stdout) and the flicker-free live TUI, which
 *  captures a frame into a memory stream and diffs it line by line.
 * ------------------------------------------------------------------------- */

/* ANSI SGR colour constants shared across the engine and its callers. */
#define NERO_C_RESET   "\033[0m"
#define NERO_C_DIM     "\033[2m"
#define NERO_C_HEAD    "\033[1;36m"
#define NERO_C_GREEN   "\033[32m"
#define NERO_C_YELLOW  "\033[33m"
#define NERO_C_RED     "\033[31m"
#define NERO_C_CYAN    "\033[36m"
#define NERO_C_MAGENTA "\033[35m"

/* Tables stop widening past this so an ultrawide terminal does not produce a
 * single 200-cell text column; beyond it the grid stays centred-left. */
#define NERO_TABLE_DEFAULT_MAX 120

/* Maximum columns a single table may declare. */
#define NERO_TABLE_MAX_COLS 16

typedef enum { NERO_TABLE_TEXT, NERO_TABLE_SIGNAL } nero_table_kind;

/* A column declares a min/max content width, whether it can flex to absorb
 * slack, right alignment, a drop priority (0 = never drop) and a flex grow
 * cap. SIGNAL columns render a fixed four-segment meter from the cell RSSI. */
typedef struct {
    const char *title;
    int min;
    int max;
    int flex;
    int right;
    int prio;
    int grow;
    nero_table_kind kind;
} nero_table_col;

/* One rendered cell. `color` is an ANSI SGR string (e.g. NERO_C_GREEN) or NULL;
 * it is ignored when colour is disabled. SIGNAL cells use has_rssi/rssi. */
typedef struct {
    char text[128];
    const char *color;
    int has_rssi;
    int rssi;
} nero_table_cell;

/* Fills out[0..ncols-1] for the given row index. Rendering is streamed one row
 * at a time, so arbitrarily large data sets never need a big staging buffer. */
typedef void (*nero_table_row_fn)(int index, void *ctx, nero_table_cell *out);

/* Render options. Zero-initialise then call nero_table_opts_default() to pull
 * live capability gates, or set the fields directly to force a mode (the live
 * TUI pins them so a captured frame matches the surface it draws into). */
typedef struct {
    int color;     /* emit ANSI colour escapes */
    int unicode;   /* use box-drawing + meter glyphs; else pure ASCII */
    int max_width; /* cap table width; <= 0 uses NERO_TABLE_DEFAULT_MAX */
    int width;     /* available terminal width; <= 0 queries the live width */
} nero_table_opts;

/* Single source of truth for terminal width: COLUMNS -> TIOCGWINSZ -> 80. */
int nero_terminal_width(void);

/* Populate opts from the live terminal capability gates and default caps. */
void nero_table_opts_default(nero_table_opts *opts);

/* Display width of a UTF-8 string in terminal cells. */
int nero_display_width(const char *s);

/* Copy up to maxw display cells of s into out, appending an ellipsis (U+2026,
 * or '.' in ASCII mode) when clipped. Returns the display cells written. */
int nero_fit(const char *s, int maxw, int ascii, char *out, size_t outlen);

/* Map an RSSI to a 1..4 strength level and a traffic-light colour. Level 0
 * means no signal information was captured and is never shown as weak. */
int nero_signal_level(int has_rssi, int rssi, const char **color);

/* Draw a full table (top rule, header, divider, every row, bottom rule, and
 * the narrow-view disclosure line) into out. opts may be NULL for defaults. */
void nero_table_draw(FILE *out, const nero_table_opts *opts,
                     const nero_table_col *cols, int ncols, int nrows,
                     nero_table_row_fn fn, void *ctx);

#endif /* NERO_TABLE_H */
