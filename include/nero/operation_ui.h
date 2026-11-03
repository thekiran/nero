#ifndef NERO_OPERATION_UI_H
#define NERO_OPERATION_UI_H

/* Zero-initialize before begin. One caller owns the surface until end;
 * overlapping or nested operation surfaces are not supported. */
typedef struct {
    int interactive;
    int active;
} nero_operation_ui;

/* NONE means no input yet; EOF means input is unavailable. CANCEL unifies
 * standalone Escape, Q and a literal Ctrl+C byte for operation callers. */
typedef enum {
    NERO_UI_KEY_NONE = 0,
    NERO_UI_KEY_UP,
    NERO_UI_KEY_DOWN,
    NERO_UI_KEY_ENTER,
    NERO_UI_KEY_CANCEL,
    NERO_UI_KEY_EOF,
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

/* Decodes a standalone Escape separately from CSI/SS3 arrow sequences.
 * timeout_ms < 0 waits indefinitely; zero performs a non-blocking poll. */
nero_ui_key nero_operation_read_key(int timeout_ms);

#endif
