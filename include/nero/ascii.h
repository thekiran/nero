#ifndef ASCII_H
#define ASCII_H

/* Single logo definition shared by the shell and isolated operation screens. */
#define NERO_ASCII_LOGO \
    "                                                         \n" \
    "                                                         \n" \
    "░▒▓███████▓▒░ ░▒▓████████▓▒░░▒▓███████▓▒░  ░▒▓██████▓▒░  \n" \
    "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n" \
    "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n" \
    "░▒▓█▓▒░░▒▓█▓▒░░▒▓██████▓▒░  ░▒▓███████▓▒░ ░▒▓█▓▒░░▒▓█▓▒░ \n" \
    "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n" \
    "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n" \
    "░▒▓█▓▒░░▒▓█▓▒░░▒▓████████▓▒░░▒▓█▓▒░░▒▓█▓▒░ ░▒▓██████▓▒░  \n" \
    "                                                         \n"

/* Shared, read-only frame strings; consumers own animation timing/rendering. */
extern const char *windows[];

/* Redraw the banner using terminal capabilities and the shared display lock. */
int ascii_function(void);
/* Legacy declaration retained for source compatibility. */
void flood_animation_a(void);

#endif

