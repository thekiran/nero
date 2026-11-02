#include <stdio.h>
#include "nero/syn_flood_powered.h"

int main(void)
{
    /* NULL and shell metacharacters must be ignored, never resolved/executed. */
    if (SYN_Flood(NULL) == 0 || SYN_Flood("not-a-target; must-not-execute") == 0) {
        fputs("Unavailable adapter reported success\n", stderr);
        return 1;
    }
    return 0;
}
