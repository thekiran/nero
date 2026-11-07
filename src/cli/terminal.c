/* Terminal session lifecycle: raw mode, signals, cleanup and the main loop. */
#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include "nero/terminal.h"
#include "cli_internal.h"
#include "nero/ascii.h"
#include "nero/log.h"
#if !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
#include "nero/portscanner.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <termios.h>
#endif
#include <unistd.h>

static int terminal_changed = 0;
static int terminal_error = 0;
static struct termios saved_terminal;
volatile sig_atomic_t cli_termination_signal = 0;
#ifdef _WIN32
static const int shell_signals[] = {SIGINT, SIGTERM, SIGBREAK};
#else
static const int shell_signals[] = {SIGINT, SIGTERM, SIGHUP, SIGPIPE};
#endif
static struct sigaction previous_signals[4];
static int signals_saved = 0;

/* Signal context only publishes flags. Rendering, joining workers and freeing
 * resources must run later in normal execution, never inside this handler. */
static void request_termination(int sig)
{
    cli_termination_signal = sig;
#if !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
    portscanner_interrupt_requested = 1;
#endif
}

int cli_signal_mode(int enabled)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_termination;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(shell_signals) / sizeof(shell_signals[0]); ++i) {
        if (sigaction(shell_signals[i], enabled ? &action : &previous_signals[i], NULL) != 0)
            return -1;
    }
    return 0;
}

static int save_signals(void)
{
    for (size_t i = 0; i < sizeof(shell_signals) / sizeof(shell_signals[0]); ++i)
        if (sigaction(shell_signals[i], NULL, &previous_signals[i]) != 0)
            return -1;
    signals_saved = 1;
    return cli_signal_mode(1);
}

static int set_terminal(const struct termios *settings)
{
    int result;
    do {
        result = tcsetattr(STDIN_FILENO, TCSANOW, settings);
    } while (result < 0 && errno == EINTR);
    return result;
}

/* Restore the session-owned snapshot once. Menu-local raw modes restore their
 * own snapshots before the outer terminal session reaches this cleanup. */
static void terminal_cleanup(void)
{
    if (terminal_changed) {
        if (set_terminal(&saved_terminal) != 0) {
            nero_log_write(NERO_LOG_ERROR, "terminal", "Terminal restoration failed: %s", strerror(errno));
            perror("tcsetattr restore");
            terminal_error = 1;
        } else {
            terminal_changed = 0;
        }
    }
}

// Enable immediate terminal input
void disableBuffering(struct termios *old)
{
    struct termios new;
    if (old == NULL) {
        terminal_error = 1;
        return;
    }
    terminal_changed = 0;
    if (!isatty(STDIN_FILENO))
        return;
    if (tcgetattr(STDIN_FILENO, old) != 0) {
        nero_log_write(NERO_LOG_ERROR, "terminal", "Terminal state read failed: %s", strerror(errno));
        perror("tcgetattr");
        terminal_error = 1;
        return;
    }
    new = *old;
    new.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode and echo
    new.c_cc[VMIN] = 1;
    new.c_cc[VTIME] = 0;
    saved_terminal = *old;
    if (set_terminal(&new) == 0) {
        terminal_changed = 1;
    } else {
        nero_log_write(NERO_LOG_ERROR, "terminal", "Terminal setup failed: %s", strerror(errno));
        perror("tcsetattr");
        terminal_error = 1;
    }
}
// Restore terminal settings
void restoreBuffering(struct termios *old)
{
    (void)old; /* Restore the snapshot owned by this terminal session. */
    terminal_cleanup();
}

int terminal_code(void)
{
    nero_log_write(NERO_LOG_INFO, "session", "NERO terminal session started");
    struct termios old = {0};
    char command[CLI_LINE_CAPACITY] = {0};

    static int cleanup_registered = 0;
    terminal_error = 0;
    cli_termination_signal = 0;
    if (!cleanup_registered) {
        if (atexit(terminal_cleanup) != 0)
            return 1;
        cleanup_registered = 1;
    }
    if (save_signals() != 0) {
        nero_log_write(NERO_LOG_ERROR, "terminal", "Signal setup failed: %s", strerror(errno));
        perror("sigaction");
        terminal_error = 1;
        goto input_end;
    }
    disableBuffering(&old);
    if (terminal_error)
        goto input_end;
    ascii_function();
    fflush(stdout);

    while (1) {
        cli_render_prompt();

        int result = cli_read_line(command);
        if (result <= 0) {
            if (result < 0) {
                nero_log_write(NERO_LOG_ERROR, "terminal", "Input read failed: %s", strerror(errno));
                perror("terminal read");
                terminal_error = 1;
            }
            goto input_end;
        }
        if (result == 2) {
            nero_log_write(NERO_LOG_WARN, "cli", "Input rejected: command exceeds 255 bytes");
            fputs("\nCommand too long (maximum 255 bytes).\n", stderr);
            continue;
        }
        row_location = row_location > INT_MAX - 2 ? 16 : row_location + 2;
        cli_history_add(command);

        int dispatched = cli_dispatch_command(command);
        if (dispatched < 0) {
            terminal_error = 1;
            goto input_end;
        }
        if (dispatched == 1)
            break;
    }

input_end:
    restoreBuffering(&old);
    if (signals_saved && cli_signal_mode(0) != 0)
        terminal_error = 1;
    signals_saved = 0;
    nero_log_write(terminal_error ? NERO_LOG_ERROR :
                   (cli_termination_signal ? NERO_LOG_WARN : NERO_LOG_INFO), "session",
                   "NERO terminal session ended: signal=%d status=%d",
                   (int)cli_termination_signal, terminal_error);
    if (cli_termination_signal)
        return 128 + cli_termination_signal;
    return terminal_error || ferror(stdout) ? 1 : 0;
}
