/* Bounded local application logs. No stdout/stderr interception and no raw
 * command-line capture: callers publish explicit operational events. */
#include "nero/log.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
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

/* Canonical level labels. The persisted record stores the name, not the enum
 * value, so these are the single source of truth for both writing and import. */
static const char *const LEVEL_NAMES[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};

/* Runtime floor. Read without the writer lock so a disabled level costs one
 * relaxed atomic load and no formatting. Defaults to INFO even before init. */
static atomic_int runtime_level = NERO_LOG_INFO;

/* Correlation id, stable for the life of the process. Assigned during init. */
static char session_id[17];

/* Operational counters, all guarded by log_mutex. buffer usage derives from
 * `used`; last_error mirrors the most recent storage_error. */
static struct {
    unsigned long writes;      /* records accepted into the ring */
    unsigned long failures;    /* persist attempts that disabled file logging */
    unsigned long dropped;     /* ring evictions (oldest record overwritten) */
    unsigned long rotations;   /* file rotations performed on the active path */
    unsigned long redactions;  /* records in which a secret value was masked */
    int last_error;            /* most recent storage errno, 0 if none */
} counters;

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
    else { at = head; head = (head + 1) % LOG_CAPACITY; counters.dropped++; }
    entries[at].level = level;
    clean_text(entries[at].text, sizeof(entries[at].text), line);
    counters.writes++;
}

/* Case-insensitive match of a whole word at the start of `s` (the following
 * character must not continue an identifier). Returns the matched length. */
static size_t match_secret_key(const char *s) {
    static const char *const keys[] = {
        "authorization", "access_token", "refresh_token", "private_key",
        "api_key", "apikey", "password", "passwd", "secret", "cookie",
        "bearer", "token", "pwd", "key"};
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
        size_t n = strlen(keys[k]);
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == keys[k][i]) i++;
        if (i != n) continue;
        unsigned char next = (unsigned char)s[n];
        if (isalnum(next) || next == '_') continue; /* part of a longer word */
        return n;
    }
    return 0;
}

/* Redact secret values in place before a record is stored. A sensitive key
 * (whole word) followed by '=', ':' or whitespace has its value token masked;
 * "bearer <token>" / "basic <token>" style schemes mask the following token.
 * This runs inside the logging API, so no caller can bypass it. */
static int redact_secrets(char *msg) {
    int redacted = 0;
    size_t i = 0;
    while (msg[i]) {
        int boundary = i == 0 || (!isalnum((unsigned char)msg[i - 1]) && msg[i - 1] != '_');
        size_t keylen = boundary ? match_secret_key(msg + i) : 0;
        if (!keylen) { i++; continue; }
        size_t j = i + keylen;
        while (msg[j] == ' ' || msg[j] == '\t') j++;
        if (msg[j] == '=' || msg[j] == ':') {
            j++;
            while (msg[j] == ' ' || msg[j] == '\t') j++;
        } else if (j == i + keylen) {
            /* No separator and no gap: not a key=value or scheme token. */
            i += keylen;
            continue;
        }
        size_t value_start = j;
        while (msg[j] && msg[j] != ' ' && msg[j] != '\t' && msg[j] != '&' &&
               msg[j] != ';' && msg[j] != ',' && msg[j] != ')' && msg[j] != '"')
            j++;
        if (j > value_start) {
            const char mask[] = "***";
            size_t masklen = sizeof(mask) - 1;
            size_t vallen = j - value_start;
            size_t tail = strlen(msg + j) + 1; /* include terminator */
            if (vallen != masklen) {
                if (masklen > vallen) {
                    /* Only grow when it fits the fixed message buffer (512). */
                    size_t total = value_start + masklen + tail;
                    if (total > 512) { i = j; continue; }
                }
                memmove(msg + value_start + masklen, msg + j, tail);
            }
            memcpy(msg + value_start, mask, masklen);
            j = value_start + masklen;
            redacted = 1;
        }
        i = j;
    }
    return redacted;
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
    /* Refuse a log file owned by another user or carrying extra hard links:
     * that is the case where a different local user controls or shares the
     * target, so fall back to in-memory logging rather than appending into it
     * (O_NOFOLLOW above already blocks symlink swaps). The permission bits are
     * deliberately not dictated here: unlike the always-0600 history file, the
     * log path is user-selectable via NERO_LOG_FILE, so the owner's own choice
     * of mode on a file they own is respected. */
    if (info.st_uid != geteuid() || info.st_nlink != 1) { log_close(fd); errno = EPERM; return NULL; }
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
/* Recover the severity of a persisted record of the form
 * "[timestamp] [LEVEL] [component] message". Unrecognized records default to
 * INFO so a format from an older or newer build still loads. */
static nero_log_level level_of_record(const char *line) {
    const char *prefix_end = strchr(line, ']');
    if (!prefix_end || prefix_end[1] != ' ' || prefix_end[2] != '[') return NERO_LOG_INFO;
    const char *name = prefix_end + 3;
    for (nero_log_level l = NERO_LOG_TRACE; l <= NERO_LOG_FATAL; ++l) {
        size_t n = strlen(LEVEL_NAMES[l]);
        if (strncmp(name, LEVEL_NAMES[l], n) == 0 && name[n] == ']') return l;
    }
    return NERO_LOG_INFO;
}
static void remember_import(char *line, size_t length) {
    while (length && line[length - 1] == '\r') length--;
    line[length] = '\0';
    if (length) remember(level_of_record(line), line);
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
/* Derive a short, stable correlation id. Not security-sensitive: it only ties
 * one process's records together, so a mix of pid, wall clock and a monotonic
 * reading is sufficient and needs no PRNG seeding. */
static void generate_session_id(void) {
#ifdef _WIN32
    uint64_t mix = (uint64_t)GetCurrentProcessId() * 0x9E3779B97F4A7C15ull;
    mix ^= (uint64_t)time(NULL) << 20;
    LARGE_INTEGER counter;
    if (QueryPerformanceCounter(&counter))
        mix ^= (uint64_t)counter.QuadPart * 0x100000001B3ull;
#else
    uint64_t mix = (uint64_t)getpid() * 0x9E3779B97F4A7C15ull;
    mix ^= (uint64_t)time(NULL) << 20;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        mix ^= (uint64_t)ts.tv_nsec * 0x100000001B3ull + (uint64_t)ts.tv_sec;
#endif
    mix ^= mix >> 33;
    snprintf(session_id, sizeof(session_id), "%016llx", (unsigned long long)mix);
}
static void initialize(void) {
    generate_session_id();
    nero_log_level env_level;
    const char *level_name = getenv("NERO_LOG_LEVEL");
    if (level_name && nero_log_level_from_name(level_name, &env_level) == 0)
        atomic_store_explicit(&runtime_level, (int)env_level, memory_order_relaxed);
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
        counters.failures++;
        counters.last_error = storage_error;
        char message[LOG_LINE];
        snprintf(message, sizeof(message), "[logger] [WARN] [storage] File logging unavailable: %s; using memory", strerror(storage_error));
        remember(NERO_LOG_WARN, message);
    }
}
static void persist_line(const char *line) {
    if (!persistent) return;
    FILE *file = open_log(path, 0, 0);
    if (!file) { storage_error = errno; counters.failures++; counters.last_error = errno; persistent = 0; return; }
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
    if (error) { fclose(file); storage_error = error; counters.failures++; counters.last_error = error; persistent = 0; return; }
    if (size + separator + (long)strlen(line) + 1 > LOG_LIMIT) {
        fclose(file);
        if (bound_existing_file(backup) != 0 || bound_existing_file(path) != 0 ||
            (remove(backup) != 0 && errno != ENOENT) || rename(path, backup) != 0) {
            storage_error = errno ? errno : EIO; counters.failures++; counters.last_error = storage_error; persistent = 0; return;
        }
        counters.rotations++;
        file = open_log(path, 0, 0);
        if (!file) { storage_error = errno; counters.failures++; counters.last_error = errno; persistent = 0; return; }
        separator = 0;
    }
    /* A truncated tail must never absorb the next event's timestamp/severity. */
    int failed = fprintf(file, "%s%s\n", separator ? "\n" : "", line) < 0;
    error = failed ? (errno ? errno : EIO) : 0;
    if (fclose(file) != 0) { failed = 1; error = errno; }
    if (failed) { storage_error = error ? error : EIO; counters.failures++; counters.last_error = storage_error; persistent = 0; }
}
void nero_log_write(nero_log_level level, const char *component, const char *format, ...) {
    int saved_errno = errno;
    if ((unsigned)level > NERO_LOG_FATAL) level = NERO_LOG_FATAL;
    /* Cheap suppression: below the runtime floor and not an error/fatal. No
     * lock, no formatting, no lazy init triggered. ERROR/FATAL always pass. */
    if ((int)level < atomic_load_explicit(&runtime_level, memory_order_relaxed) &&
        level < NERO_LOG_ERROR) { errno = saved_errno; return; }
    pthread_once(&log_once, initialize);
    pthread_mutex_lock(&log_mutex);
    char message[512] = {0}, safe_message[512], safe_component[40], timestamp[32];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format ? format : "", args);
    va_end(args);
    if (redact_secrets(message)) counters.redactions++;
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
    char line[LOG_LINE];
    snprintf(line, sizeof(line), "[%s] [%s] [%s] %s", timestamp, LEVEL_NAMES[level], safe_component, safe_message);
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
        if (!result) { persistent = 1; storage_error = 0; counters.last_error = 0; }
    } else if (storage_error) { result = -1; error = storage_error; }
    if (!result) { head = used = 0; memset(entries, 0, sizeof(entries)); }
    else { storage_error = error ? error : EIO; counters.last_error = storage_error; persistent = 0; }
    pthread_mutex_unlock(&log_mutex);
    errno = result ? (error ? error : EIO) : saved_errno;
    return result;
}

/* ------------------------------------------------------------------ */
/* Levels, correlation, filtered views, JSON export and health.        */
/* ------------------------------------------------------------------ */

static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}
void nero_log_set_level(nero_log_level level) {
    if ((int)level < NERO_LOG_TRACE) level = NERO_LOG_TRACE;
    if ((int)level > NERO_LOG_ERROR) level = NERO_LOG_ERROR; /* never silence ERROR/FATAL */
    atomic_store_explicit(&runtime_level, (int)level, memory_order_relaxed);
}
nero_log_level nero_log_get_level(void) {
    return (nero_log_level)atomic_load_explicit(&runtime_level, memory_order_relaxed);
}
int nero_log_level_from_name(const char *name, nero_log_level *out) {
    if (!name) return -1;
    for (int l = NERO_LOG_TRACE; l <= NERO_LOG_FATAL; ++l)
        if (ci_equal(name, LEVEL_NAMES[l])) { if (out) *out = (nero_log_level)l; return 0; }
    return -1;
}
const char *nero_log_level_name(nero_log_level level) {
    if ((unsigned)level > NERO_LOG_FATAL) return "INFO";
    return LEVEL_NAMES[level];
}
const char *nero_log_session_id(void) {
    pthread_once(&log_once, initialize);
    return session_id;
}

/* Split "[ts] [LEVEL] [component] message" into its parts without copying.
 * A record that does not match the shape yields empty fields and the whole
 * text as the message, so malformed/imported lines still render. */
static void split_record(const char *text, const char **ts, size_t *tsn,
                         const char **comp, size_t *compn, const char **msg) {
    *ts = ""; *comp = ""; *msg = text; *tsn = 0; *compn = 0;
    const char *groups[3]; size_t lengths[3];
    int found = 0;
    const char *p = text;
    for (int g = 0; g < 3 && *p == '['; ++g) {
        const char *close = strchr(p, ']');
        if (!close) break;
        groups[g] = p + 1;
        lengths[g] = (size_t)(close - (p + 1));
        found++;
        p = close + 1;
        if (*p == ' ') p++;
    }
    if (found >= 1) { *ts = groups[0]; *tsn = lengths[0]; }
    if (found >= 3) { *comp = groups[2]; *compn = lengths[2]; *msg = p; }
}
static int record_matches(const log_entry *e, const nero_log_query *q) {
    if (e->level < q->min_level) return 0;
    if (q->substring && q->substring[0] && !strstr(e->text, q->substring)) return 0;
    if (q->component && q->component[0]) {
        const char *ts, *comp, *msg; size_t tsn, compn;
        split_record(e->text, &ts, &tsn, &comp, &compn, &msg);
        size_t want = strlen(q->component);
        if (want != compn || strncmp(comp, q->component, compn) != 0) return 0;
    }
    return 1;
}
/* First index into the ring to show, honoring query->tail. */
static size_t query_start(const nero_log_query *q, int *total) {
    int matches = 0;
    for (size_t i = 0; i < used; ++i)
        if (record_matches(&entries[(head + i) % LOG_CAPACITY], q)) matches++;
    *total = matches;
    if (q->tail > 0 && matches > q->tail) {
        int skip = matches - q->tail, seen = 0;
        for (size_t i = 0; i < used; ++i) {
            if (record_matches(&entries[(head + i) % LOG_CAPACITY], q) && ++seen == skip)
                return i + 1;
        }
    }
    return 0;
}
void nero_log_dump_query(FILE *out, const nero_log_query *query) {
    int saved_errno = errno;
    nero_log_query local = query ? *query : (nero_log_query){0};
    pthread_once(&log_once, initialize);
    pthread_mutex_lock(&log_mutex);
    int total = 0;
    size_t start = query_start(&local, &total);
    int shown = 0;
    for (size_t i = start; i < used; ++i) {
        const log_entry *entry = &entries[(head + i) % LOG_CAPACITY];
        if (!record_matches(entry, &local)) continue;
        fprintf(out, "%s\n", entry->text);
        shown++;
    }
    if (!shown) fputs("No matching log records.\n", out);
    else if (local.tail > 0 && total > shown)
        fprintf(out, "(showing last %d of %d matching records)\n", shown, total);
    pthread_mutex_unlock(&log_mutex);
    errno = saved_errno;
}
static void json_escape(FILE *out, const char *s, size_t n) {
    for (size_t i = 0; i < n && s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
}
void nero_log_export_json(FILE *out, const nero_log_query *query) {
    int saved_errno = errno;
    nero_log_query local = query ? *query : (nero_log_query){0};
    pthread_once(&log_once, initialize);
    pthread_mutex_lock(&log_mutex);
    int total = 0;
    size_t start = query_start(&local, &total);
    for (size_t i = start; i < used; ++i) {
        const log_entry *entry = &entries[(head + i) % LOG_CAPACITY];
        if (!record_matches(entry, &local)) continue;
        const char *ts, *comp, *msg; size_t tsn, compn;
        split_record(entry->text, &ts, &tsn, &comp, &compn, &msg);
        fputs("{\"schema_version\":1,\"session_id\":\"", out);
        json_escape(out, session_id, strlen(session_id));
        fputs("\",\"timestamp\":\"", out);
        json_escape(out, ts, tsn);
        fprintf(out, "\",\"level\":\"%s\",\"component\":\"", LEVEL_NAMES[entry->level]);
        json_escape(out, comp, compn);
        fputs("\",\"message\":\"", out);
        json_escape(out, msg, strlen(msg));
        fputs("\"}\n", out);
    }
    pthread_mutex_unlock(&log_mutex);
    errno = saved_errno;
}
void nero_log_health(FILE *out) {
    int saved_errno = errno;
    pthread_once(&log_once, initialize);
    pthread_mutex_lock(&log_mutex);
    show_path(out);
    nero_log_level floor = (nero_log_level)atomic_load_explicit(&runtime_level, memory_order_relaxed);
    fprintf(out, "Log health (session %s, level %s):\n", session_id, LEVEL_NAMES[floor]);
    fprintf(out, "  writes=%lu failures=%lu dropped=%lu rotations=%lu redactions=%lu\n",
            counters.writes, counters.failures, counters.dropped, counters.rotations, counters.redactions);
    fprintf(out, "  buffer=%zu/%d records (%d%% full)\n", used, LOG_CAPACITY,
            (int)(used * 100 / LOG_CAPACITY));
    fprintf(out, "  storage=%s last_error=%s\n", persistent ? "file" : "memory",
            counters.last_error ? strerror(counters.last_error) : "none");
    pthread_mutex_unlock(&log_mutex);
    errno = saved_errno;
}
void nero_log_stats(FILE *out) {
    int saved_errno = errno;
    pthread_once(&log_once, initialize);
    pthread_mutex_lock(&log_mutex);
    int by_level[NERO_LOG_FATAL + 1] = {0};
    struct { char name[40]; int count; } comps[16];
    int ncomp = 0;
    for (size_t i = 0; i < used; ++i) {
        const log_entry *entry = &entries[(head + i) % LOG_CAPACITY];
        by_level[entry->level]++;
        const char *ts, *comp, *msg; size_t tsn, compn;
        split_record(entry->text, &ts, &tsn, &comp, &compn, &msg);
        if (compn == 0 || compn >= sizeof(comps[0].name)) continue;
        int j;
        for (j = 0; j < ncomp; ++j)
            if (strncmp(comps[j].name, comp, compn) == 0 && comps[j].name[compn] == '\0') break;
        if (j == ncomp) {
            if (ncomp == (int)(sizeof(comps) / sizeof(comps[0]))) continue; /* bounded */
            memcpy(comps[ncomp].name, comp, compn);
            comps[ncomp].name[compn] = '\0';
            comps[ncomp].count = 0;
            ncomp++;
        }
        comps[j].count++;
    }
    fprintf(out, "Log statistics (%zu of %d records in buffer):\n", used, LOG_CAPACITY);
    fputs("  By level:", out);
    for (int l = NERO_LOG_TRACE; l <= NERO_LOG_FATAL; ++l)
        if (by_level[l]) fprintf(out, " %s=%d", LEVEL_NAMES[l], by_level[l]);
    fputc('\n', out);
    for (int j = 0; j < ncomp; ++j)
        fprintf(out, "  [%s] %d\n", comps[j].name, comps[j].count);
    if (!used) fputs("  (no records)\n", out);
    pthread_mutex_unlock(&log_mutex);
    errno = saved_errno;
}
