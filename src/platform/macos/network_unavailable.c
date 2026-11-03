/* No socket, DNS, ping or scanner calls belong in this adapter. */
#include <stdio.h>
#include "nero/syn_flood_powered.h"

int SYN_Flood(const char *target_ip)
{
    (void)target_ip;
    fputs("\n-Syn: unsupported in this build; no network traffic is generated.\n", stderr);
    return 1;
}
