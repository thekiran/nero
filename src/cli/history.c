#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
/* Private, bounded history storage. Only explicitly recognized command shapes
 * are persisted: arbitrary input, credentials and unknown arguments stay out.
 * Every update reloads under a nonblocking exclusive lock, so concurrent shells
 * append to current disk state instead of resurrecting a cleared snapshot. */
#include "cli_internal.h"
#include "nero/log.h"
#include "nero/private_file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

#define HISTORY_PATH_CAPACITY 4096
#define HISTORY_FILE_LIMIT (CLI_HISTORY_CAPACITY * CLI_LINE_CAPACITY)
static char history_path[HISTORY_PATH_CAPACITY];
static int storage_enabled;
static nero_state_dir history_directory = NERO_STATE_DIR_INIT;
static int warned;

static void storage_warning(void)
{
    if (!warned) {
        warned = 1;
        nero_log_write(NERO_LOG_WARN, "history", "Command history storage unavailable; session history remains usable (error=%d)", errno);
        fputs("\nHistory storage unavailable; see log -errors.\n", stderr);
    }
}

/* An allowlist is intentional: a substring blacklist cannot reliably identify
 * arbitrary secrets in unknown/future commands. Keep this list conservative. */
static int persistable(const char *line)
{
    static const char *const safe[] = {
        "help", "help -All", "help --all", "version", "clear", "history",
        "logs", "logs errors", "logs path",
        "network status", "network enable", "network disable",
        "-mode", "-mode on", "-mode off", "log", "log -errors", "log -path"
    };
    for (size_t i = 0; i < sizeof(safe) / sizeof(safe[0]); ++i)
        if (strcmp(line, safe[i]) == 0)
            return 1;
    /* Valid IPv4 -Sc commands contain no arbitrary argument values. Addresses
     * are nevertheless personal history; :memory: disables persistence. */
    unsigned octet[4];
    int end = 0;
    int alias = strncmp(line, "scan ", 5) == 0;
    const char *target = alias ? line + 5 : line;
    const char *format = alias ? "%3u.%3u.%3u.%3u%n" : "%3u.%3u.%3u.%3u -Sc%n";
    if (sscanf(target, format, &octet[0], &octet[1],
               &octet[2], &octet[3], &end) == 4 && end > 0 && target[end] == '\0') {
        for (size_t i = 0; i < 4; ++i)
            if (octet[i] > 255) return 0;
        /* Require canonical IPv4 and exactly one separator; scanf alone also
         * accepts signs, leading zeroes and a missing whitespace separator. */
        char canonical[32];
        int n = snprintf(canonical, sizeof(canonical), alias ? "scan %u.%u.%u.%u" : "%u.%u.%u.%u -Sc",
                         octet[0], octet[1], octet[2], octet[3]);
        return n > 0 && (size_t)n < sizeof(canonical) && strcmp(line, canonical) == 0;
    }
    return 0;
}

/* Lock lifetime is the FILE lifetime. Reject special files, links and broad
 * POSIX permissions before any truncation or reading of command contents. */
static FILE *open_history(void)
{
    int fd = nero_state_open(&history_directory, nero_state_basename(history_path), O_RDWR | O_CREAT, 1);
    if (fd < 0) return NULL;
#ifdef _WIN32
    FILE *file = _fdopen(fd, "r+b");
    if (!file) _close(fd);
#else
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_nlink != 1 ||
        (info.st_mode & 077) != 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd); errno = EACCES; return NULL;
    }
    FILE *file = fdopen(fd, "r+");
    if (!file) close(fd);
#endif
    return file;
}

static void append_entry(char entries[][CLI_LINE_CAPACITY], int *count, const char *line)
{
    if (*count == CLI_HISTORY_CAPACITY) {
        memmove(entries[0], entries[1], sizeof(entries[0]) * (CLI_HISTORY_CAPACITY - 1));
        --*count;
    }
    memcpy(entries[(*count)++], line, strlen(line) + 1);
}

static int read_entries(FILE *file, char entries[][CLI_LINE_CAPACITY], int *count)
{
    *count = 0;
    if (fseek(file, 0, SEEK_END) != 0) return -1;
    long size = ftell(file);
    if (size < 0) return -1;
    if (size > HISTORY_FILE_LIMIT) {
        if (fseek(file, -HISTORY_FILE_LIMIT, SEEK_END) != 0) return -1;
        int ch;
        while ((ch = fgetc(file)) != EOF && ch != '\n') {}
        if (ch == EOF) return ferror(file) ? -1 : 0;
    } else if (fseek(file, 0, SEEK_SET) != 0) return -1;
    char line[CLI_LINE_CAPACITY];
    size_t length = 0;
    int invalid = 0, ch;
    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\n') {
            line[length] = '\0';
            if (!invalid && persistable(line)) append_entry(entries, count, line);
            length = 0;
            invalid = 0;
        } else if (ch < 32 || ch > 126 || length == sizeof(line) - 1) {
            invalid = 1;
        } else line[length++] = (char)ch;
    }
    /* A truncated final record is deliberately discarded. */
    return ferror(file) ? -1 : 0;
}

static int write_entries(FILE *file, char entries[][CLI_LINE_CAPACITY], int count)
{
    if (fseek(file, 0, SEEK_SET) != 0) return -1;
    for (int i = 0; i < count; ++i)
        if (fprintf(file, "%s\n", entries[i]) < 0) return -1;
    if (fflush(file) != 0) return -1;
    long size = ftell(file);
    if (size < 0) return -1;
#ifdef _WIN32
    return _chsize_s(_fileno(file), (size_t)size) == 0 ? 0 : -1;
#else
    return ftruncate(fileno(file), (off_t)size);
#endif
}

void cli_history_load(void)
{
    nero_state_close(&history_directory);
    storage_enabled = 0;
    warned = 0;
    const char *override = getenv("NERO_HISTORY_FILE");
    if (nero_state_elevated() || (override && strcmp(override, ":memory:") == 0)) return;
    const char *home;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    const char *separator = "\\";
    if (!override && !_isatty(_fileno(stdin))) return;
#else
    home = getenv("HOME");
    const char *separator = "/";
    /* Do not populate a user's history when running a privileged shell. */
    if (!override && (!isatty(STDIN_FILENO) || geteuid() == 0 || geteuid() != getuid())) return;
#endif
    int n;
    if (override) n = snprintf(history_path, sizeof(history_path), "%s", override);
    else if (home && home[0]) n = snprintf(history_path, sizeof(history_path), "%s%s.nero_history", home, separator);
    else return;
    if (n <= 0 || (size_t)n >= sizeof(history_path)) { errno = ENAMETOOLONG; storage_warning(); return; }
    if (nero_state_parent(&history_directory, history_path, 0) != 0) { storage_warning(); return; }
    storage_enabled = 1;
    FILE *file = open_history();
    if (!file) { storage_warning(); return; }
    if (read_entries(file, history, &history_count) != 0) storage_warning();
    if (fclose(file) != 0) storage_warning();
}

void cli_history_persist(const char *command)
{
    if (!storage_enabled || !persistable(command)) return;
    char entries[CLI_HISTORY_CAPACITY][CLI_LINE_CAPACITY];
    int count;
    FILE *file = open_history();
    if (!file) { storage_warning(); return; }
    int failed = read_entries(file, entries, &count) != 0;
    if (!failed) {
        append_entry(entries, &count, command);
        failed = write_entries(file, entries, count) != 0;
    }
    if (fclose(file) != 0) failed = 1;
    if (failed) storage_warning();
}

int cli_history_clear(void)
{
    /* Memory always clears, even when persistent storage is unavailable. */
    memset(history, 0, sizeof(history));
    history_count = 0;
    if (!storage_enabled) return 0;
    FILE *file = open_history();
    if (!file) { storage_warning(); return -1; }
    int failed = write_entries(file, history, 0) != 0;
    if (fclose(file) != 0) failed = 1;
    if (failed) storage_warning();
    return failed ? -1 : 0;
}
