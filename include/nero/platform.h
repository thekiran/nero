#ifndef NERO_PLATFORM_H
#define NERO_PLATFORM_H

/* Describes the offline development targets, never the legacy Linux engine. */
typedef struct nero_platform_info {
    const char *os;
    const char *architecture;
    int development_cli;
    int interface_listing;
} nero_platform_info;

const nero_platform_info *nero_get_platform_info(void);
#endif
