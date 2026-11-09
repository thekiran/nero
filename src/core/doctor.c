/* Privacy by construction: no collected string enters the report. No shell,
 * sockets, logger, history reader or filesystem writer is used here. */
#include "nero/doctor.h"
#include "nero/printing.h"
#include "doctor_internal.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define doctor_access _access
#define doctor_isatty _isatty
#define doctor_fileno _fileno
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <unistd.h>
#define doctor_access access
#define doctor_isatty isatty
#define doctor_fileno fileno
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#define DOCTOR_PATH_CAP 4096

static int executable_path(char *out, size_t capacity)
{
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)capacity);
    return n > 0 && n < capacity;
#elif defined(__APPLE__)
    uint32_t n = (uint32_t)capacity;
    char raw[DOCTOR_PATH_CAP];
    return _NSGetExecutablePath(raw, &n) == 0 && realpath(raw, out) != NULL;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, capacity - 1);
    if (n <= 0 || (size_t)n >= capacity - 1) return 0;
    out[n] = 0;
    return 1;
#else
    (void)out; (void)capacity;
    return 0;
#endif
}

static int same_executable(const char *candidate, const char *executable)
{
#ifdef _WIN32
    /* Compare file identity, not spelling/case or junction aliases. */
    HANDLE a = CreateFileA(candidate, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    HANDLE b = CreateFileA(executable, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    BY_HANDLE_FILE_INFORMATION x, y;
    int same = a != INVALID_HANDLE_VALUE && b != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(a, &x) && GetFileInformationByHandle(b, &y) &&
        x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
        x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
    if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
    if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
    return same;
#else
    struct stat a, b;
    return stat(candidate, &a) == 0 && S_ISREG(a.st_mode) && stat(executable, &b) == 0 &&
           a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#endif
}

static int path_status(const char *executable)
{
    const char *path = getenv("PATH");
    if (!path) return -1;
    size_t n = 0;
    while (n < 32768 && path[n]) n++;
    if (n == 32768) return -1;
#ifdef _WIN32
    const char separator = ';';
    const char suffix[] = "/nero.exe";
    const int mode = 0;
#else
    const char separator = ':';
    const char suffix[] = "/nero";
    const int mode = X_OK;
#endif
    const char *p = path;
    for (int i = 0; i < 256; ++i) {
        const char *end = strchr(p, separator);
        size_t length = end ? (size_t)(end - p) : strlen(p);
        char candidate[DOCTOR_PATH_CAP];
        if (length + sizeof(suffix) >= sizeof(candidate)) return -1;
        if (!length) { strcpy(candidate, "."); length = 1; }
        else memcpy(candidate, p, length);
        memcpy(candidate + length, suffix, sizeof(suffix));
        if (doctor_access(candidate, mode) == 0) {
            struct stat info;
            if (stat(candidate, &info) == 0 && S_ISREG(info.st_mode))
                return same_executable(candidate, executable) ? 1 : 2;
        }
        if (!end) return 0;
        p = end + 1;
    }
    return -1;
}

void doctor_collect(doctor_snapshot *s)
{
    memset(s, 0, sizeof(*s));
#if defined(_WIN32)
    s->os = DOCTOR_WINDOWS;
#elif defined(__APPLE__)
    s->os = DOCTOR_MACOS;
#elif defined(__linux__)
    s->os = DOCTOR_LINUX;
#else
    s->os = DOCTOR_UNKNOWN;
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    s->architecture = 1;
#elif defined(__x86_64__) || defined(_M_X64)
    s->architecture = 2;
#endif
    char executable[DOCTOR_PATH_CAP];
    if (executable_path(executable, sizeof(executable))) {
        s->executable = 2;
#ifndef _WIN32
        if (strcmp(executable, "/usr/local/bin/nero") == 0 ||
            strcmp(executable, "/usr/bin/nero") == 0 ||
            strcmp(executable, "/opt/homebrew/bin/nero") == 0) s->executable = 1;
#endif
        s->path = path_status(executable);
    } else s->path = -1;
    const char *term = getenv("TERM");
    int tty = doctor_isatty(doctor_fileno(stdout));
    s->terminal = !tty ? 0 : (getenv("NO_COLOR") || !term || !*term || strcmp(term, "dumb") == 0 ? 1 : 2);
#ifdef _WIN32
    DWORD mode;
    if (s->terminal == 2 && (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) ||
        !(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING))) s->terminal = 1;
    const char *home = getenv("USERPROFILE");
    s->privilege = -1;
#else
    const char *home = getenv("HOME");
    s->privilege = geteuid() == 0;
#endif
    struct stat info;
    s->config = !home || !*home ? -1 :
        (stat(home, &info) == 0 && S_ISDIR(info.st_mode) ? 1 : 0);
#if !defined(NERO_SAFE_TEST) && !defined(NERO_NETWORK_UNAVAILABLE)
    s->scanner = s->os != DOCTOR_UNKNOWN;
#ifndef NERO_SYN_UNAVAILABLE
    s->raw_engine = s->os == DOCTOR_LINUX || s->os == DOCTOR_MACOS;
#endif
#endif
}

int doctor_render(FILE *out, const doctor_snapshot *s)
{
    static const char *const systems[] = {"Unsupported/unknown", "macOS", "Linux", "Windows"};
    const char *os = (unsigned)s->os < 4 ? systems[s->os] : systems[0];
    const char *arch = s->architecture == 1 ? "ARM64" : s->architecture == 2 ? "x86_64" : "Unknown";
    if (s->terminal == 2) fputs(PRINTING_LINE_1, out);
    fprintf(out, "\nNERO - Offline Doctor\nVersion: %s\nBuild: C11 / %s\nOS: %s | CPU: %s\n",
            NERO_BUILD_VERSION,
#ifdef __clang__
            "Clang",
#elif defined(_MSC_VER)
            "MSVC",
#elif defined(__GNUC__)
            "GCC",
#else
            "unknown compiler",
#endif
            os, arch);
    fputs("Privacy: personal paths, addresses, credentials, history and logs omitted.\n", out);
    fprintf(out, "Installation: %s\n", !s->executable ? "unavailable; executable lookup failed" :
            s->executable == 1 ? "standard system bin directory (path redacted)" : "[redacted]/nero (private or portable location)");
    fprintf(out, "PATH: %s\n", s->path == 1 ? "nero resolves to this executable" :
            s->path == 2 ? "another nero precedes this executable; review installation order" :
            s->path == 0 ? "nero not found; use the documented installer/global command setup" :
            "not verifiable (missing, oversized or unsupported lookup)");
    fprintf(out, "Terminal: %s\n", s->terminal == 2 ? "TTY with color support" :
            s->terminal == 1 ? "plain TTY (NO_COLOR, dumb/unset TERM or no VT)" : "redirected/plain output");
    fprintf(out, "User storage root: %s; no history/log contents read\n", s->config == 1 ? "directory exists (permissions not tested)" :
            s->config == 0 ? "missing or inaccessible; session-only storage may be needed" : "not configured");
    fprintf(out, "TCP scanner: %s\nRaw engine: %s\n", s->scanner ? "compiled; runtime connectivity not tested" : "unavailable in this build/platform",
            s->raw_engine ? "compiled; runtime capability not tested" : "unavailable in this build/platform");
    fprintf(out, "Privilege: %s\n", s->privilege == 1 ? "elevated process; no elevation requested" :
            s->privilege == 0 ? "unprivileged; raw socket capability not tested" : "not verified; no elevation requested");
    fputs("Prerequisites: C runtime loaded; optional socket/adapter access not probed.\n"
          "Build-only compilers and package managers are not runtime prerequisites.\n"
          "Help: docs/installation.md and docs/troubleshooting.md\n"
          "No network tests, file changes or automatic repairs performed.\n", out);
    if (s->terminal == 2) fputs(PRINTING_LINE_1, out);
    return ferror(out) ? 1 : 0;
}
int nero_doctor(FILE *out)
{
    doctor_snapshot s;
    doctor_collect(&s);
    return doctor_render(out, &s);
}
