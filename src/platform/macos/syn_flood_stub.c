#include <stdio.h>
#include "nero/syn_flood_powered.h"

int SYN_Flood(const char *target_ip)
{
    (void)target_ip;

    fprintf(stderr,
            "SYN operation is not supported on this platform.\n");

    return 1;
}
