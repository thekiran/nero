/* Application entry point. The terminal session owns input, dispatch and
 * cleanup; propagating its status preserves signal and I/O failures. */
#include "nero/terminal.h"
#include <stdio.h>

#ifdef _WIN32
#include "platform/windows/nero_win_compat.h"
#endif

int main(void)
{
#ifdef _WIN32
    nero_windows_init(); /* winsock startup, UTF-8 + VT console output */
#endif
    return terminal_code();
}
