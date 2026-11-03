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

#define ESCAPE_CONTINUATION_MS 35
#define OPERATION_DOT_COUNT 6

static int alternate_screen_active = 0;

/* Idempotent normal-exit fallback. atexit does not run after SIGKILL or a
 * fatal crash, so callers must still end operations on recoverable paths. */
static void restore_operation_surface(void)
{
    if (!alternate_screen_active)
        return;
    fputs("\033[0m\033[?1049l", stdout);
    fflush(stdout);
    alternate_screen_active = 0;
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
    ui->interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    ui->active = 1;
    if (ui->interactive) {
        static int cleanup_registered = 0;
        if (!cleanup_registered && atexit(restore_operation_surface) == 0)
            cleanup_registered = 1;
        alternate_screen_active = 1;
        fputs("\033[?1049h\033[H\033[2J", stdout);
    } else {
        putchar('\n');
    }

    /* Draw the shared brand above all operation content. Loader redraws only
     * replace their own line, leaving the logo intact during discovery. */
    if (ui->interactive && getenv("NO_COLOR") == NULL)
        fputs("\033[35m", stdout);
    fputs(NERO_ASCII_LOGO, stdout);
    if (ui->interactive && getenv("NO_COLOR") == NULL)
        fputs("\033[0m", stdout);
    putchar('\n');

    /* Keep operation metadata light so the shared logo remains the heading. */
    if (title != NULL && title[0] != '\0')
        printf("  \033[1m%s\033[0m\n", title);
    if (detail != NULL && detail[0] != '\0')
        printf("  \033[2m%s\033[0m\n", detail);
    if (controls != NULL && controls[0] != '\0') {
        printf("  \033[2m%s\033[0m\n", controls);
    }
    putchar('\n');
    fflush(stdout);
}

void nero_operation_section(const char *label)
{
    printf("\033[1;36m%s\033[0m\n\n", label);
    fflush(stdout);
}

/* Replace one status line in place. Frames 0..5 highlight one dot; any larger
 * frame produces the static six-dot representation used for reduced motion. */
void nero_operation_loader(const char *label, const char *detail, unsigned frame)
{
    printf("\r\033[2K  %-12s ", label);
    for (unsigned index = 0; index < OPERATION_DOT_COUNT; ++index) {
        if (index == frame)
            fputs("\033[96m\342\227\217\033[0m", stdout);
        else
            fputs("\033[2m\302\267\033[0m", stdout);
    }
    if (detail != NULL && detail[0] != '\0')
        printf("  \033[2m%s\033[0m", detail);
    fflush(stdout);
}

/* color is an internal ANSI SGR parameter, not arbitrary external text.
 * Finalizing the loader with a newline leaves later sections on a fresh row. */
void nero_operation_status(const char *color, const char *label, const char *detail)
{
    fputs("\r\033[2K", stdout);
    printf("\033[%sm%s\033[0m", color == NULL ? "0" : color, label);
    if (detail != NULL && detail[0] != '\0')
        printf("  \033[2m%s\033[0m", detail);
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
