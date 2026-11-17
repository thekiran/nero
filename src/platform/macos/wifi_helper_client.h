#ifndef NERO_WIFI_HELPER_CLIENT_H
#define NERO_WIFI_HELPER_CLIENT_H
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include "wifi_helper_protocol.h"
extern char **environ;

/* Resolve NEROWiFi.app: next to the running executable, else /Applications. */
static int helper_locate_app(char *app, size_t app_size)
{
    char executable[PATH_MAX], resolved[PATH_MAX];
    uint32_t size = sizeof(executable);
    app[0] = '\0';
    if (!_NSGetExecutablePath(executable, &size) && realpath(executable, resolved)) {
        char *slash = strrchr(resolved, '/');
        if (slash) {
            *slash = '\0';
            if (strlen(resolved) + sizeof("/NEROWiFi.app") <= app_size) {
                strcpy(app, resolved);
                strcat(app, "/NEROWiFi.app");
            }
        }
    }
    if (!app[0] || access(app, R_OK)) {
        if (sizeof("/Applications/NEROWiFi.app") > app_size)
            return -1;
        strcpy(app, "/Applications/NEROWiFi.app");
    }
    return access(app, R_OK) ? -1 : 0;
}

/*
 * Launch NEROWiFi.app via LaunchServices (`open`), so it runs as its own
 * signed, responsible process. `mode` is "--scan" or "--neighbors". On success
 * returns 0 and sets *out_data (NUL-terminated, caller frees) and *out_length.
 */
static int helper_invoke(const char *mode, const char *interface_name, char **out_data,
                         size_t *out_length, char *error, size_t error_len)
{
    *out_data = NULL;
    *out_length = 0;
    char app[PATH_MAX];
    if (helper_locate_app(app, sizeof(app))) {
        set_error(error, error_len,
                  "NEROWiFi.app missing: run make wifi-helper; open the app to grant Location access");
        return -1;
    }
    char directory[] = "/tmp/nero-wifi.XXXXXX";
    if (!mkdtemp(directory)) {
        set_error(error, error_len, "cannot create private helper request");
        return -1;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/result.json", directory);
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    int result = -1;
    pid_t pid = -1;
    const char *message = "Wi-Fi helper failed to launch";
    if (fd < 0)
        goto done;
    char *args[] = {"/usr/bin/open", "-n", "-W", "-g", app, "--args", (char *)mode, "--output",
                    path, (char *)interface_name, NULL};
    /* No shell, environment overrides, shared response path, or root helper. */
    if (posix_spawn(&pid, "/usr/bin/open", NULL, NULL, args, environ))
        goto done;
    int status = 0, finished = 0;
    struct timespec delay = {0, 100000000};
    for (int i = 0; i < 300; ++i) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) { finished = 1; break; }
        if (waited < 0 && errno != EINTR) break;
        nanosleep(&delay, NULL);
    }
    if (!finished) {
        kill(pid, SIGTERM);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        message = "Wi-Fi helper timed out after 30 seconds; close NERO Wi-Fi and retry";
        goto done;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        goto done;
    struct stat st;
    if (fstat(fd, &st) || st.st_size <= 0 || st.st_size > 131072) {
        message = "invalid or empty Wi-Fi helper response";
        goto done;
    }
    size_t length = (size_t)st.st_size;
    char *data = calloc(length + 1, 1);
    if (!data)
        goto done;
    size_t used = 0;
    while (used < length) {
        ssize_t n = read(fd, data + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    if (used == length && !memchr(data, '\0', length)) {
        *out_data = data;
        *out_length = length;
        result = 0;
        message = NULL;
    } else {
        free(data);
        message = "truncated Wi-Fi helper response";
    }
done:
    if (fd >= 0) close(fd);
    unlink(path);
    rmdir(directory);
    if (result && message) set_error(error, error_len, message);
    return result;
}

static int helper_scan(const char *interface_name, nero_wifi_ap *out, int capacity, int *count,
                       nero_wifi_platform_capabilities *caps, char *error, size_t error_len)
{
    *count = 0;
    if (caps) memset(caps, 0, sizeof(*caps));
    if (geteuid() == 0 || getuid() != geteuid()) {
        set_error(error, error_len, "run Wi-Fi app scanning as the logged-in user, without sudo");
        return -1;
    }
    char *data = NULL;
    size_t length = 0;
    if (helper_invoke("--scan", interface_name, &data, &length, error, error_len))
        return -1;
    int result = helper_parse(data, length + 1, out, capacity, count, caps, error, error_len);
    free(data);
    return result;
}

/*
 * Read the OS neighbor (ARP) cache through the helper. Used as a fallback when
 * the in-process routing-table read returns nothing (recent macOS hands the
 * sandboxed CLI an empty table). Needs no Location permission.
 */
static int helper_neighbors(const char *interface_name, nero_wifi_neighbor *out, int capacity,
                            int *count, char *error, size_t error_len)
{
    *count = 0;
    if (geteuid() == 0 || getuid() != geteuid()) {
        set_error(error, error_len, "run Wi-Fi neighbor lookup as the logged-in user, without sudo");
        return -1;
    }
    char *data = NULL;
    size_t length = 0;
    if (helper_invoke("--neighbors", interface_name, &data, &length, error, error_len))
        return -1;
    int result = helper_parse_neighbors(data, length + 1, out, capacity, count, error, error_len);
    free(data);
    return result;
}
#endif
