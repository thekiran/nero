/* Link-time replacement: never include or link the real scanner/load engine. */
#include <stdio.h>
#include "nero/syn_flood_powered.h"

int SYN_Flood(const char *target_ip)
{
    /* Match the engine boundary contract before formatting the argument. */
    if (target_ip == NULL || target_ip[0] == '\0')
        return 1;
    printf("\nMOCK_SYN:%s\n", target_ip);
    return 0;
}
