#include <stdio.h>
#include "nero/syn_flood_powered.h"

int SYN_Flood(const char *target_ip)
{
    (void)target_ip;
    /* Leading newline so the message does not glue to the echoed command;
     * SYN_UNAVAILABLE tells the shell to skip its "operation failed" line. */
    fputs("\n-Syn: unsupported in this build; no network traffic is generated.\n",
          stderr);
    return SYN_UNAVAILABLE;
}
