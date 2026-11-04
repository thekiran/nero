#ifndef NERO_OPERATION_UI_H
#define NERO_OPERATION_UI_H

/* Zero-initialize before begin. One caller owns the surface until end;
 * overlapping or nested operation surfaces are not supported. */
typedef struct {
    int interactive;
    int active;
    int rows, columns;
    int content_top, content_bottom;
    int scroll_offset;
    int color, compact;
    int unicode; /* 0 on non-UTF-8 locales or NERO_ASCII: ASCII glyphs only */
    const char *title, *detail, *controls;
    /* Optional banner lookup for the selected port in nero_operation_ports.
     * banner_ports[i] pairs with banner_texts[i]; the selected port's text is
     * shown on a detail line. Looked up by port value, so it is independent of
     * any sort applied to the displayed `ports` array. NULL/0 disables it. */
    const int *banner_ports;
    const char *const *banner_texts;
    int banner_count;
} nero_operation_ui;

/* Scroll units for nero_operation_discovery's scroll_delta. Plain values move
 * by lines; multiples of NERO_UI_SCROLL_PAGE move by whole viewport pages and
 * NERO_UI_SCROLL_EDGE jumps to the top (negative) or bottom (positive). */
#define NERO_UI_SCROLL_PAGE 10000
#define NERO_UI_SCROLL_EDGE 100000000

/* NONE means no input yet; EOF means input is unavailable. CANCEL unifies
 * standalone Escape, Q and a literal Ctrl+C byte for operation callers. */
typedef enum {
    NERO_UI_KEY_NONE = 0,
    NERO_UI_KEY_UP,
    NERO_UI_KEY_DOWN,
    NERO_UI_KEY_ENTER,
    NERO_UI_KEY_CANCEL,
    NERO_UI_KEY_EOF,
    NERO_UI_KEY_WHEEL_UP,
    NERO_UI_KEY_WHEEL_DOWN,
    NERO_UI_KEY_PAGE_UP,
    NERO_UI_KEY_PAGE_DOWN,
    NERO_UI_KEY_HOME,
    NERO_UI_KEY_END,
    NERO_UI_KEY_LEFT,
    NERO_UI_KEY_RIGHT,
    NERO_UI_KEY_OTHER
} nero_ui_key;

/* Opens an isolated operation surface when attached to a terminal. Restoring
 * it brings the user's main shell view back exactly as it was. */
void nero_operation_begin(
    nero_operation_ui *ui,
    const char *title,
    const char *detail,
    const char *controls
);

/* Render a section heading at the current cursor position. Strings are
 * borrowed for the duration of the call and are never retained. */
void nero_operation_section(const char *label);
/* Frames 0..5 highlight a dot; values >= 6 render the static loader. */
void nero_operation_loader(const char *label, const char *detail, unsigned frame);
/* color must be a trusted ANSI SGR parameter such as "32" for success. */
void nero_operation_status(const char *color, const char *label, const char *detail);
/* Safe to call again after ending. Restores the screen, not input termios;
 * the terminal session remains responsible for its input-mode lifecycle. */
void nero_operation_end(nero_operation_ui *ui);

/* Recalculate layout on resize; draw only within the operation body. */
void nero_operation_ports(nero_operation_ui *ui, const int *ports, int count, int selected);
/* Discovery shows the live results without enabling selection until workers
 * join. permille (0..1000 = 0.0..100.0%) drives a full-width progress bar on
 * the terminal's last row. scroll_delta (wheel/arrow) moves the viewport when
 * the discovered list is taller than the body; 0 keeps following the newest. */
void nero_operation_discovery(const int *ports, int count, int total,
                              unsigned frame, int latest_port, int permille,
                              int scroll_delta);
void nero_operation_frame(nero_operation_ui *ui, const char *frame, unsigned attempt);

/* Generic single-column selection menu drawn inside the operation body, using
 * the same highlight, scrollbar and glyph conventions as the port list.
 * `heading` is an optional bold title above the list (NULL to omit).
 * `labels[0..count-1]` are the rows; `values`, when non-NULL, prints
 * values[i] right-aligned after each label (used for adjustable settings).
 * `selected` is the highlighted row. Rendering only; the caller owns the key
 * loop and state. */
void nero_operation_menu(nero_operation_ui *ui, const char *heading,
                         const char *const *labels, const char *const *values,
                         int count, int selected);

/* Well-known TCP service name for a port, e.g. 22 -> "ssh". Static lookup,
 * no network. Returns "" for unknown ports. */
const char *nero_service_name(int port);

/* Decodes a standalone Escape separately from CSI/SS3 arrow sequences.
 * timeout_ms < 0 waits indefinitely; zero performs a non-blocking poll. */
nero_ui_key nero_operation_read_key(int timeout_ms);

#endif
