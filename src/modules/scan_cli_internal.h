#ifndef NERO_SCAN_CLI_INTERNAL_H
#define NERO_SCAN_CLI_INTERNAL_H
#include "scan_internal.h"

#include "nero/portscanner.h"
typedef enum { HOST_UNKNOWN, HOST_ONLINE } host_state;
typedef struct {
    pthread_t owner;
    int fallback;
} scan_cli_input;
#define BANNER_MAX 72
int portscanner_with_io(const char *, const scan_io *);
int select_port_menu(int *, int);
int scan_cli_cancel(void *);
void report_scan_summary(nero_scan_session *);
void scan_error_text(int, char *, size_t);
void grab_banners(nero_scan_session *, const char *, const int *, int, char (*)[BANNER_MAX]);
host_state host_probe_alive(nero_scan_session *, const char *);
#endif
