/* Bounded local application logs. No stdout/stderr interception and no raw
 * command-line capture: callers publish explicit operational events. */
#include "nero/log.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_private(p) _mkdir(p)
#define log_close _close
#else
#include <unistd.h>
#define mkdir_private(p) mkdir(p, 0700)
#define log_close close
#endif

#define LOG_CAPACITY 256
#define LOG_LINE 768
#define LOG_PATH 1024
#define LOG_LIMIT (1024L * 1024L)
typedef struct { nero_log_level level; char text[LOG_LINE]; } log_entry;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static log_entry entries[LOG_CAPACITY];
static size_t head, used;
static int initialized, persistent, storage_error;
static char path[LOG_PATH], backup[LOG_PATH + 3];

static void clean_text(char *out, size_t size, const char *in) {
    size_t i = 0;
    if (!in) in = "";
    while (in[i] && i + 1 < size) {
        unsigned char ch = (unsigned char)in[i];
        out[i] = ch < 32 || ch == 127 ? ' ' : (char)ch;
        i++;
    }
    out[i] = '\0';
}
static void remember(nero_log_level level, const char *line) {
    size_t at;
    if (used < LOG_CAPACITY) at = (head + used++) % LOG_CAPACITY;
    else { at = head; head = (head + 1) % LOG_CAPACITY; }
    entries[at].level = level;
    clean_text(entries[at].text, sizeof(entries[at].text), line);
}
static FILE *open_log(const char *name, int truncate, int reading) {
#ifdef _WIN32
    int flags = reading ? _O_RDONLY : (_O_WRONLY | _O_CREAT | (truncate ? _O_TRUNC : _O_APPEND));
    int fd = _open(name, flags | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
    if (fd < 0) return NULL;
    FILE *file = _fdopen(fd, reading ? "rb" : "ab");
#else
    int flags = reading ? O_RDONLY : (O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND));
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(name, flags, 0600);
    if (fd < 0) return NULL;
    FILE *file = fdopen(fd, reading ? "r" : "a");
#endif
    if (!file) { int error = errno; log_close(fd); errno = error; }
    return file;
}
static void load_recent(const char *name) {
    FILE *file = open_log(name, 0, 1);
    if (!file) return;
    char line[LOG_LINE];
    if (fseek(file, 0, SEEK_END) == 0) {
        long bytes = ftell(file);
        if (bytes > LOG_LIMIT) {
            if (fseek(file, -LOG_LIMIT, SEEK_END) == 0) (void)fgets(line, sizeof(line), file);
        } else rewind(file);
    }
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        const char *prefix_end = strchr(line, ']');
        nero_log_level level = prefix_end && strncmp(prefix_end, "] [ERROR] [", 11) == 0 ? NERO_LOG_ERROR :
            (prefix_end && strncmp(prefix_end, "] [WARN] [", 10) == 0 ? NERO_LOG_WARN : NERO_LOG_INFO);
        if (n) remember(level, line);
    }
    fclose(file);
}
static int create_parent(void) {
    char parent[LOG_PATH];
    memcpy(parent, path, strlen(path) + 1);
    for (size_t i = 1; parent[i]; ++i) {
        if (parent[i] != '/' && parent[i] != '\\') continue;
#ifdef _WIN32
        if (i == 2 && parent[1] == ':') continue;
#else
        if (parent[i] == '\\') continue;
#endif
        char saved = parent[i]; parent[i] = '\0';
        if (mkdir_private(parent) != 0 && errno != EEXIST) return -1;
        parent[i] = saved;
    }
    return 0;
}
static void initialize(void) {
    if (initialized) return;
    initialized = 1;
    const char *override = getenv("NERO_LOG_FILE");
    if (override && strcmp(override, ":memory:") == 0) return;
    int length;
    if (override && *override) length = snprintf(path, sizeof(path), "%s", override);
    else {
#ifdef _WIN32
        const char *root = getenv("LOCALAPPDATA");
        length = root ? snprintf(path, sizeof(path), "%s/Nero/Logs/nero.log", root) : -1;
#elif defined(__APPLE__)
        const char *root = getenv("HOME");
        length = root ? snprintf(path, sizeof(path), "%s/Library/Logs/Nero/nero.log", root) : -1;
#else
        const char *root = getenv("XDG_STATE_HOME");
        if (root && *root) length = snprintf(path, sizeof(path), "%s/nero/nero.log", root);
        else {
            root = getenv("HOME");
            length = root ? snprintf(path, sizeof(path), "%s/.local/state/nero/nero.log", root) : -1;
        }
#endif
    }
    if (length < 0 || (size_t)length >= sizeof(path)) {
        path[0] = '\0'; storage_error = length < 0 ? ENOENT : ENAMETOOLONG;
    } else {
        snprintf(backup, sizeof(backup), "%s.1", path);
        if (create_parent() == 0) {
            FILE *file = open_log(path, 0, 0);
            if (file) { fclose(file); persistent = 1; load_recent(backup); load_recent(path); }
        }
        if (!persistent) storage_error = errno ? errno : EIO;
    }
    if (storage_error) {
        char message[LOG_LINE];
        snprintf(message, sizeof(message), "[logger] [WARN] [storage] File logging unavailable: %s; using memory", strerror(storage_error));
        remember(NERO_LOG_WARN, message);
    }
}
static void persist_line(const char *line) {
    if (!persistent) return;
    struct stat info;
    if (stat(path, &info) == 0 && info.st_size + (long)strlen(line) + 1 > LOG_LIMIT) {
        if ((remove(backup) != 0 && errno != ENOENT) || rename(path, backup) != 0) {
            storage_error = errno; persistent = 0; return;
        }
    }
    FILE *file = open_log(path, 0, 0);
    if (!file) { storage_error = errno; persistent = 0; return; }
    int failed = fprintf(file, "%s\n", line) < 0;
    int error = errno;
    if (fclose(file) != 0) { failed = 1; error = errno; }
    if (failed) { storage_error = error ? error : EIO; persistent = 0; }
}
void nero_log_write(nero_log_level level, const char *component, const char *format, ...) {
    int saved_errno = errno;
    pthread_mutex_lock(&log_mutex);
    initialize();
    char message[512] = {0}, safe_message[512], safe_component[40], timestamp[32];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format ? format : "", args);
    va_end(args);
    clean_text(safe_message, sizeof(safe_message), message);
    clean_text(safe_component, sizeof(safe_component), component);
    time_t now = time(NULL);
    struct tm tm_value;
#ifdef _WIN32
    int valid_time = gmtime_s(&tm_value, &now) == 0;
#else
    int valid_time = gmtime_r(&now, &tm_value) != NULL;
#endif
    if (!valid_time || strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm_value) == 0)
        strcpy(timestamp, "time-unavailable");
    if ((unsigned)level > NERO_LOG_ERROR) level = NERO_LOG_ERROR;
    const char *names[] = {"INFO", "WARN", "ERROR"};
    char line[LOG_LINE];
    snprintf(line, sizeof(line), "[%s] [%s] [%s] %s", timestamp, names[level], safe_component, safe_message);
    remember(level, line);
    persist_line(line);
    pthread_mutex_unlock(&log_mutex);
    errno = saved_errno;
}
static void show_path(FILE *out) {
    char safe_path[LOG_PATH];
    clean_text(safe_path, sizeof(safe_path), path);
    if (persistent) fprintf(out, "Log file: %s (rotation: 1 MiB + one backup)\n", safe_path);
    else if (storage_error) fprintf(out, "Logs: memory only; file unavailable (%s). Requested path: %s\n",
        strerror(storage_error), safe_path[0] ? safe_path : "unavailable");
    else fputs("Logs: memory only (NERO_LOG_FILE=:memory:).\n", out);
}
void nero_log_show_path(FILE *out) {
    int saved_errno = errno;
    pthread_mutex_lock(&log_mutex); initialize(); show_path(out); pthread_mutex_unlock(&log_mutex);
    errno = saved_errno;
}
void nero_log_dump(FILE *out, int errors_only) {
    int saved_errno = errno;
    pthread_mutex_lock(&log_mutex);
    initialize(); show_path(out);
    fprintf(out, "Recent logs (up to %d entries, timestamps UTC)%s:\n", LOG_CAPACITY,
            errors_only ? " — warnings/errors" : "");
    int shown = 0;
    for (size_t i = 0; i < used; ++i) {
        const log_entry *entry = &entries[(head + i) % LOG_CAPACITY];
        if (errors_only && entry->level < NERO_LOG_WARN) continue;
        fprintf(out, "%s\n", entry->text); shown++;
    }
    if (!shown) fputs(errors_only ? "No warnings or errors recorded.\n" : "No logs recorded.\n", out);
    pthread_mutex_unlock(&log_mutex);
    errno = saved_errno;
}
int nero_log_clear(void) {
    int saved_errno = errno, result = 0, error = 0;
    pthread_mutex_lock(&log_mutex);
    initialize();
    if (persistent) {
        FILE *file = open_log(path, 1, 0);
        if (!file) { result = -1; error = errno; }
        else if (fclose(file) != 0) { result = -1; error = errno; }
        if (remove(backup) != 0 && errno != ENOENT) { result = -1; error = errno; }
    }
    if (!result) { head = used = 0; memset(entries, 0, sizeof(entries)); }
    pthread_mutex_unlock(&log_mutex);
    errno = result ? error : saved_errno;
    return result;
}
