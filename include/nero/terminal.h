#ifndef TERMINAL_H
#define TERMINAL_H

/* Run one terminal session; restore terminal settings before returning. */
int terminal_code(void);

/* Legacy display position used by the animation module. */
int row_location_function(void);

/* Rendering capability boundary for a future non-POSIX adapter. */
int terminal_ansi_enabled(void);
int terminal_color_enabled(void);
int terminal_unicode_enabled(void);
void clear_screen(void);

#endif
