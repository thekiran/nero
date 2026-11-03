/* Standalone development report; inspecting capabilities performs no network
 * operations and does not start the interactive shell. */
#include <stdio.h>
#include "nero/platform.h"

int main(void)
{
    const nero_platform_info *info = nero_get_platform_info();
    puts("N E R O - Development platform report");
    puts("EXPERIMENTAL SOFTWARE - LABORATORY USE ONLY");
    puts("Not intended for public use or production deployment.");
    puts("EXPERIMENTAL SOFTWARE - RESTRICTED TO LABORATORY USE");
    printf("Platform: %s\nArchitecture: %s\n", info->os, info->architecture);
    printf("Development CLI: %s\n", info->development_cli ? "Available (POSIX)" : "Unsupported");
    printf("Interface listing: %s\n", info->interface_listing ? "Available (local only)" : "Unsupported");
    puts("Traffic generation: Unsupported in CMake development targets");
    puts("Port scanning: Unsupported in CMake development targets");
    puts("Build availability does not certify runtime compatibility.");
    return 0;
}
