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
#include <windows.h>
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
static pthread_once_t log_once = PTHREAD_ONCE_INIT;
static log_entry entries[LOG_CAPACITY];
static size_t head, used;
static int persistent, storage_error;
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
/* Validate the opened descriptor before truncation or stdio buffering. A
 * special file must fall back to memory rather than blocking shell startup. */
static FILE *open_log(const char *name, int truncate, int reading) {
#ifdef _WIN32
    if (_strnicmp(name, "\\\\.\\pipe\\", 9) == 0) { errno = EINVAL; return NULL; }
    int flags = reading ? _O_RDONLY : (_O_RDWR | _O_CREAT | _O_APPEND);
    int fd = _open(name, flags | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
    if (fd < 0) return NULL;
    struct _stat64 info;
    if (_fstat64(fd, &info) != 0) { int error = errno; log_close(fd); errno = error; return NULL; }
    if ((info.st_mode & _S_IFMT) != _S_IFREG) { log_close(fd); errno = EINVAL; return NULL; }
    if (truncate) {
        int error = (int)_chsize_s(fd, 0);
        if (error) { log_close(fd); errno = error; return NULL; }
    }
    FILE *file = _fdopen(fd, reading ? "rb" : "a+b");
#else
    int flags = (reading ? O_RDONLY : (O_RDWR | O_CREAT | O_APPEND)) | O_NONBLOCK;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(name, flags, 0600);
    if (fd < 0) return NULL;
    struct stat info;
    if (fstat(fd, &info) != 0) { int error = errno; log_close(fd); errno = error; return NULL; }
    if (!S_ISREG(info.st_mode)) { log_close(fd); errno = EINVAL; return NULL; }
    if (truncate && ftruncate(fd, 0) != 0) { int error = errno; log_close(fd); errno = error; return NULL; }
    FILE *file = fdopen(fd, reading ? "r" : "a+");
#endif
    if (!file) { int error = errno; log_close(fd); errno = error; }
    return file;
}

/* Compact oversized inherited files before rotating them. Write a private,
 * exclusive temporary file and replace only after a complete successful copy.
 * Keep complete recent lines; discard an over-limit single malformed record. */
static int bound_existing_file(const char *name) {
    errno = 0;
    FILE *source = open_log(name, 0, 1);
    if (!source) return errno == ENOENT ? 0 : -1;
    if (fseek(source, 0, SEEK_END) != 0) { int error = errno; fclose(source); errno = error; return -1; }
    long size = ftell(source);
    if (size < 0) { int error = errno; fclose(source); errno = error; return -1; }
    if (size <= LOG_LIMIT) { fclose(source); return 0; }
    char *tail = malloc((size_t)LOG_LIMIT);
    if (!tail) { fclose(source); errno = ENOMEM; return -1; }
    int failed = fseek(source, -LOG_LIMIT, SEEK_END) != 0;
    if (!failed && fread(tail, 1, (size_t)LOG_LIMIT, source) != (size_t)LOG_LIMIT) failed = 1;
    int error = failed ? (errno ? errno : EIO) : 0;
    fclose(source);
    if (failed) { free(tail); errno = error; return -1; }
    size_t start = 0;
    while (start < (size_t)LOG_LIMIT && tail[start++] != '\n') {}
    size_t length = (size_t)LOG_LIMIT - start;
    char temporary[LOG_PATH + 16];
    snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", name);
#ifdef _WIN32
    if (_mktemp_s(temporary, sizeof(temporary)) != 0) { free(tail); return -1; }
    int fd = _open(temporary, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
    int fd = mkstemp(temporary);
#endif
    if (fd < 0) { error = errno; free(tail); errno = error; return -1; }
#ifdef _WIN32
    FILE *dest = _fdopen(fd, "wb");
#else
    FILE *dest = fdopen(fd, "w");
#endif
    if (!dest) { error = errno; log_close(fd); remove(temporary); free(tail); errno = error; return -1; }
    failed = length && fwrite(tail + start, 1, length, dest) != length;
    error = failed ? (errno ? errno : EIO) : 0;
    if (fclose(dest) != 0) { failed = 1; error = errno; }
    free(tail);
    if (!failed) {
#ifdef _WIN32
        if (!MoveFileExA(temporary, name, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD cause = GetLastError();
            failed = 1;
            error = cause == ERROR_DISK_FULL ? ENOSPC :
                (cause == ERROR_ACCESS_DENIED || cause == ERROR_SHARING_VIOLATION ? EACCES : EIO);
        }
#else
        if (rename(temporary, name) != 0) { failed = 1; error = errno; }
#endif
    }
    if (failed) { remove(temporary); errno = error; return -1; }
    return 0;
}
static void remember_import(char *line, size_t length) {
    while (length && line[length - 1] == '\r') length--;
    line[length] = '\0';
    const char *prefix_end = strchr(line, ']');
    nero_log_level level = prefix_end && strncmp(prefix_end, "] [ERROR] [", 11) == 0 ? NERO_LOG_ERROR :
        (prefix_end && strncmp(prefix_end, "] [WARN] [", 10) == 0 ? NERO_LOG_WARN : NERO_LOG_INFO);
    if (length) remember(level, line);
}
/* Called by pthread_once, before acquiring the writer lock. Import at most
 * one MiB per file; avoid stream-position/EOF ambiguities after failed seeks. */
static void load_recent(const char *name) {
    FILE *file = open_log(name, 0, 1);
    if (!file) return;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return; }
    long end = ftell(file);
    if (end < 0) { fclose(file); return; }
    long start = end > LOG_LIMIT ? end - LOG_LIMIT : 0;
    if (fseek(file, start, SEEK_SET) != 0) { fclose(file); return; }
    long remaining = end - start;
    int discard = start > 0;
    char chunk[8192], line[LOG_LINE];
    size_t length = 0;
    while (remaining > 0) {
        size_t wanted = remaining < (long)sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
        size_t read_count = fread(chunk, 1, wanted, file);
        if (!read_count) break;
        remaining -= (long)read_count;
        for (size_t i = 0; i < read_count; ++i) {
            if (chunk[i] == '\n') {
                if (!discard) remember_import(line, length);
                discard = 0; length = 0;
            } else if (!discard && length + 1 < sizeof(line)) line[length++] = chunk[i];
        }
        if (read_count < wanted) break;
    }
    if (!discard && length) remember_import(line, length);
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
        errno = 0;
        if (create_parent() != 0) storage_error = errno ? errno : EIO;
        else {
            errno = 0;
            FILE *file = open_log(path, 0, 0);
            if (!file) storage_error = errno ? errno : EIO;
            else {
                fclose(file);
                if (bound_existing_file(backup) != 0 || bound_existing_file(path) != 0)
                    storage_error = errno ? errno : EIO;
                else persistent = 1;
            }
        }
    }
    /* Readable historical records remain available even without write access. */
    if (path[0]) { load_recent(backup); load_recent(path); }
    if (storage_error) {
        char message[LOG_LINE];
        snprintf(message, sizeof(message), "[logger] [WARN] [storage] File logging unavailable: %s; using memory", strerror(storage_error));
        remember(NERO_LOG_WARN, message);
    }
}
static void persist_line(const char *line) {
    if (!persistent) return;
    FILE *file = open_log(path, 0, 0);
    if (!file) { storage_error = errno; persistent = 0; return; }
    int error = 0;
    if (fseek(file, 0, SEEK_END) != 0) error = errno ? errno : EIO;
    long size = error ? -1 : ftell(file);
    if (!error && size < 0) error = errno ? errno : EIO;
    int separator = 0;
    if (!error && size > 0) {
        if (fseek(file, -1, SEEK_END) != 0) error = errno ? errno : EIO;
        else {
            int last = fgetc(file);
            if (last == EOF) error = EIO;
            else separator = last != '\n';
        }
    }
    if (!error && fseek(file, 0, SEEK_END) != 0) error = errno ? errno : EIO;
    if (error) { fclose(file); storage_error = error; persistent = 0; return; }
    if (size + separator + (long)strlen(line) + 1 > LOG_LIMIT) {
        fclose(file);
        if (bound_existing_file(backup) != 0 || bound_existing_file(path) != 0 ||
            (remove(backup) != 0 && errno != ENOENT) || rename(path, backup) != 0) {
            storage_error = errno ? errno : EIO; persistent = 0; return;
        }
        file = open_log(path, 0, 0);
        if (!file) { storage_error = errno; persistent = 0; return; }
        separator = 0;
    }
    /* A truncated tail must never absorb the next event's timestamp/severity. */
    int failed = fprintf(file, "%s%s\n", separator ? "\n" : "", line) < 0;
    error = failed ? (errno ? errno : EIO) : 0;
    if (fclose(file) != 0) { failed = 1; error = errno; }
    if (failed) { storage_error = error ? error : EIO; persistent = 0; }
}
void nero_log_write(nero_log_level level, const char *component, const char *format, ...) {
    int saved_errno = errno;
    pthread_once(&log_once, initialize);
    pthread_mutex_lock(&log_mutex);
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
    pthread_once(&log_once, initialize);
    pthread_mutex_lock(&log_mutex); show_path(out); pthread_mutex_unlock(&log_mutex);
    errno = saved_errno;
}
void nero_log_dump(FILE *out, int errors_only) {
    int saved_errno = errno;
    pthread_once(&log_once, initialize);
    pthread_mutex_lock(&log_mutex); show_path(out);
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
    pthread_once(&log_once, initialize);
    pthread_mutex_lock(&log_mutex);
    /* A failed append does not make historical files cease to exist. Retry
     * explicit clearing even in memory-fallback mode; do not report success
     * until both configured persistent files have actually been cleared. */
    if (path[0]) {
        errno = 0;
        if (create_parent() != 0) { result = -1; error = errno; }
        FILE *check = result ? NULL : open_log(path, 0, 0);
        if (!result && !check) { result = -1; error = errno; }
        if (check && fclose(check) != 0) { result = -1; error = errno; }
        if (!result) {
            FILE *previous = open_log(backup, 0, 1);
            if (previous) fclose(previous);
            else if (errno != ENOENT) { result = -1; error = errno; }
        }
        if (!result && remove(backup) != 0 && errno != ENOENT) { result = -1; error = errno; }
        if (!result) {
            FILE *file = open_log(path, 1, 0);
            if (!file) { result = -1; error = errno; }
            else if (fclose(file) != 0) { result = -1; error = errno; }
        }
        if (!result) { persistent = 1; storage_error = 0; }
    } else if (storage_error) { result = -1; error = storage_error; }
    if (!result) { head = used = 0; memset(entries, 0, sizeof(entries)); }
    else { storage_error = error ? error : EIO; persistent = 0; }
    pthread_mutex_unlock(&log_mutex);
    errno = result ? (error ? error : EIO) : saved_errno;
    return result;
}
