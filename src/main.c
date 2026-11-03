/* Application entry point. The terminal session owns input, dispatch and
 * cleanup; propagating its status preserves signal and I/O failures. */
#include "nero/terminal.h"
#include <stdio.h>

int main(void)
{

    return terminal_code();
}
