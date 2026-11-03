/* Compile-time capability report for the offline development executables.
 * These flags describe supported adapters, not runtime network availability. */
#include "nero/platform.h"

#if defined(_WIN32)
#define NERO_OS "Windows"
#define NERO_POSIX_COMPONENTS 0
#elif defined(__APPLE__)
#define NERO_OS "macOS"
#define NERO_POSIX_COMPONENTS 1
#elif defined(__linux__)
#define NERO_OS "Linux"
#define NERO_POSIX_COMPONENTS 1
#else
#define NERO_OS "Unknown"
#define NERO_POSIX_COMPONENTS 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define NERO_ARCH "ARM64"
#elif defined(__x86_64__) || defined(_M_X64)
#define NERO_ARCH "x86_64"
#else
#define NERO_ARCH "Unknown"
#endif

const nero_platform_info *nero_get_platform_info(void)
{
    static const nero_platform_info info = {
        NERO_OS, NERO_ARCH, NERO_POSIX_COMPONENTS, NERO_POSIX_COMPONENTS
    };
    return &info;
}
