#include "nero/syn_flood_powered.h"
#include <stddef.h>

int main(void)
{
    if (SYN_Flood(NULL) == 0 || SYN_Flood("") == 0)
        return 1;
    return SYN_Flood("192.0.2.1") == 0 ? 0 : 1;
}
