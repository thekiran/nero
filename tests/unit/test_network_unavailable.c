#include <stdio.h>
#include "nero/syn_flood_powered.h"

int main(void)
{
    /* NULL and shell metacharacters must be ignored, never resolved/executed. */
    if (SYN_Flood(NULL) == 0 || SYN_Flood("not-a-target; must-not-execute") == 0) {
        fputs("Unavailable adapter reported success\n", stderr);
        return 1;
    }
    /* Valid addresses must be rejected too, without attempting a connection. */
    const char *targets[] = {"", "192.0.2.1", "127.0.0.1", "::1", "example.invalid"};
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        if (SYN_Flood(targets[i]) != 1) {
            fputs("Unavailable adapter did not reject target\n", stderr);
            return 1;
        }
    }
    return 0;
}
