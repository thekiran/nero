/* Application entry point. The terminal session owns input, dispatch and
 * cleanup; propagating its status preserves signal and I/O failures. */
#include "nero/terminal.h"
#include <stdio.h>
#include <string.h>
#include "nero/doctor.h"

#ifdef _WIN32
#include "platform/windows/nero_win_compat.h"
#endif

int main(int argc, char **argv)
{
    /* Bypass session/log/history/Winsock initialization for read-only support. */
    if (argc == 2 && strcmp(argv[1], "--doctor") == 0) return nero_doctor(stdout);
#ifdef _WIN32
    nero_windows_init(); /* winsock startup, UTF-8 + VT console output */
#endif
    return terminal_code();
}
