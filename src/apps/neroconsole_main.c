/* NERO Console (`neroconsole`) — standalone interactive terminal UI.
 *
 * This is a distinct executable from `nero`, but it is NOT a reimplementation:
 * it links the exact same C11 operation core, command dispatcher and terminal
 * engine (terminal_code()). The separate entry point exists so the interactive
 * console can be launched, installed, documented and tested on its own, which is
 * what the three-frontend parity invariant (nero / neroconsole / nero-gui)
 * requires. It carries no Qt or graphical dependency and needs no display.
 *
 * Behaviour parity with `nero`:
 *   - `--doctor` runs the read-only diagnostics and exits (no session setup).
 *   - every other invocation launches the shared interactive terminal session,
 *     which owns input, command dispatch, cancellation and terminal restoration.
 */
#include "nero/terminal.h"
#include "nero/doctor.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "platform/windows/nero_win_compat.h"
#endif

int main(int argc, char **argv)
{
    /* Read-only diagnostics path: bypass session/log/history/Winsock setup,
     * identical to the `nero` entry point so both frontends report the same
     * capability information. */
    if (argc == 2 && strcmp(argv[1], "--doctor") == 0)
        return nero_doctor(stdout);

#ifdef _WIN32
    nero_windows_init(); /* winsock startup, UTF-8 + VT console output */
#endif

    /* The shared terminal engine returns the real session status so signal and
     * I/O failures propagate as a correct process exit code. */
    return terminal_code();
}
