/* Link-time replacement: never include or link the real scanner/load engine. */
#include <stdio.h>
#include "nero/syn_flood_powered.h"

int SYN_Flood(const char *target_ip)
{
    printf("\nMOCK_SYN:%s\n", target_ip);
    return 0;
}
