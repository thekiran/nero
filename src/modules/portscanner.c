/* Bounded poll-based TCP connect scanner. Discovery is advisory; every port
 * has an indexed result. A shared queue assigns each phase's work exactly once,
 * including when a worker cannot be created. No raw packets are generated. */
#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "nero/log.h"
#include "nero/operation_ui.h"
#include "nero/portscanner.h"
#include "nero/printing.h"
#include "nero/socket_handle.h"

/* Deterministic, test-only lifecycle signalling. Compiled in only when the PTY
 * live-select fixture defines NERO_SCAN_TEST_HOOKS; a release build expands
 * SCAN_TEST_SIGNAL() to nothing and keeps the production control flow, terminal
 * output and timing byte-identical. The hook writes "<tag>\n" to the descriptor
 * named by NERO_TEST_SYNC_FD so a controlling process can observe scan progress
 * (scan start, first open port, Enter accepted/ignored) without timing guesses. */
#ifdef NERO_SCAN_TEST_HOOKS
static void scan_test_signal(const char *tag)
{
    const char *fd_env = getenv("NERO_TEST_SYNC_FD");
    if (!fd_env)
        return;
    int fd = atoi(fd_env);
    char line[32];
    int n = snprintf(line, sizeof line, "%s\n", tag);
    if (n > 0) {
        ssize_t w = write(fd, line, (size_t)n);
        (void)w;
    }
}
#define SCAN_TEST_SIGNAL(tag) scan_test_signal(tag)
#else
#define SCAN_TEST_SIGNAL(tag) ((void)0)
#endif

#include "scan_cli_internal.h"
static scan_profile manual_profile = {"Manuel", 6, 200, 1000, 4, 100, 2000, 12000, 2};
static struct termios orig_termios;
volatile sig_atomic_t portscanner_interrupt_requested;
static atomic_uint scan_sequence;
/* Only finalized unknown outcomes are grouped. A retry replaces its earlier
 * error; timeouts/closed/open/unscanned are not counted as syscall failures. */

static const char *scan_stage_name(scan_error_stage stage)
{
    switch (stage) {
    case SCAN_SOCKET:
        return "socket";
    case SCAN_NONBLOCKING:
        return "nonblocking setup";
    case SCAN_CONNECT:
        return "connect";
    case SCAN_POLL:
        return "poll";
    case SCAN_SO_ERROR:
        return "SO_ERROR";
    case SCAN_POLL_EVENT:
        return "poll event";
    case SCAN_DEADLINE:
        return "deadline";
    }
    return "unknown operation";
}

void scan_error_text(int error, char *text, size_t size)
{
    if (!error) {
        snprintf(text, size, "completion not confirmed by poll readiness");
        return;
    }
#ifdef _WIN32
    if (error >= WSABASEERR) {
        DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                 (DWORD)error, 0, text, (DWORD)size, NULL);
        if (!n)
            snprintf(text, size, "Winsock error %d", error);
        else
            while (n && (text[n - 1] == '\r' || text[n - 1] == '\n'))
                text[--n] = '\0';
        return;
    }
#endif
    snprintf(text, size, "%s", strerror(error));
}

/* Map the internal completion string to a structured phase event name. */
static const char *scan_completion_event(const char *completion)
{
    if (!completion)
        return "SCAN_COMPLETED";
    if (strcmp(completion, "cancelled") == 0)
        return "SCAN_CANCELLED";
    if (strcmp(completion, "complete") != 0)
        return "SCAN_PARTIAL";
    return "SCAN_COMPLETED";
}
void report_scan_summary(nero_scan_session *s)
{
    scan_summary counts = summarize_scan(s); /* caller has joined every writer */
    int workers_failed = atomic_load_explicit(&s->worker_failures, memory_order_relaxed);
    int resources_failed = atomic_load_explicit(&s->resource_failures, memory_order_relaxed);
    nero_log_write(
        counts.unknown ? NERO_LOG_ERROR
                       : (counts.filtered || counts.unscanned ? NERO_LOG_WARN : NERO_LOG_INFO),
        "scanner",
        "op=%s target=%s host=%s open=%d closed=%d filtered=%d error/unknown=%d unscanned=%d "
        "retried=%d retry_probes=%d worker_failures=%d resource_failures=%d",
        s->scan_operation_id, s->target_ip_global,
        atomic_load_explicit(&s->host_responded, memory_order_relaxed) ? "online" : "unknown",
        counts.open, counts.closed, counts.filtered, counts.unknown, counts.unscanned,
        counts.retried, counts.retry_probes, workers_failed, resources_failed);
    printf("Host: %s | TCP: open=%d closed=%d filtered=%d error/unknown=%d "
           "unscanned=%d total=%d | retried=%d retry_probes=%d\n",
           atomic_load_explicit(&s->host_responded, memory_order_relaxed) ? "online" : "unknown",
           counts.open, counts.closed, counts.filtered, counts.unknown, counts.unscanned,
           counts.open + counts.closed + counts.filtered + counts.unknown + counts.unscanned,
           counts.retried, counts.retry_probes);
    if (s->scan_completion != NULL) {
        printf("Scan: target=%s profile=%s duration=%lld.%03llds status=%s\n", s->target_ip_global,
               s->scan_profile_label, (long long)(s->scan_duration_ms / 1000),
               (long long)(s->scan_duration_ms % 1000), s->scan_completion);
        int incomplete = strcmp(s->scan_completion, "complete") != 0;
        nero_log_write(incomplete ? NERO_LOG_WARN : NERO_LOG_INFO, "scanner",
                       "event=%s op=%s target=%s profile=%s duration_ms=%lld status=%s",
                       scan_completion_event(s->scan_completion), s->scan_operation_id,
                       s->target_ip_global, s->scan_profile_label, (long long)s->scan_duration_ms,
                       s->scan_completion);
    }
    scan_error_group groups[SCAN_ERROR_GROUPS];
    int overflow = 0;
    int count = summarize_scan_errors(s, groups, &overflow);
    for (int i = 0; i < count; ++i) {
        char text[256];
        scan_error_text(groups[i].error, text, sizeof(text));
        nero_log_write(NERO_LOG_ERROR, "scanner",
                       "op=%s target=%s phase=%s operation=%s error=%d (%s) ports=%d",
                       s->scan_operation_id, s->target_ip_global, "probe",
                       scan_stage_name(groups[i].stage), groups[i].error, text, groups[i].count);
        printf("  %s: error=%d (%s), ports=%d\n", scan_stage_name(groups[i].stage), groups[i].error,
               text, groups[i].count);
    }
    if (overflow)
        printf("  Additional ungrouped error outcomes: %d\n", overflow);
}

/* --- Terminal yardımcıları --- */
static int raw_mode_active = 0;
#define SCAN_FRAME_NANOSECONDS 70000000L

typedef struct {
    atomic_int stop;
    nero_scan_session *session;
    const char *target_ip;
    int animate;
} scan_animation_state;

/* Animasyon yalnızca gerçek bir terminalde, NERO_REDUCE_MOTION tanımsızken ve
 * TERM "dumb" değilken etkindir. */
static int scan_motion_enabled(void)
{
    const char *reduced = getenv("NERO_REDUCE_MOTION");
    const char *term = getenv("TERM");
#ifdef _WIN32
    /* Windows konsollarında TERM kuralı yoktur; VT çıkışı nero_windows_init()
     * ile açılır, canlı konsol varsayılan olarak ANSI yeteneklidir. */
    return isatty(STDOUT_FILENO) && reduced == NULL && (term == NULL || strcmp(term, "dumb") != 0);
#else
    return isatty(STDOUT_FILENO) && reduced == NULL && (term == NULL || strcmp(term, "dumb") != 0);
#endif
}

/* Tarama sırasında ekranı canlı tutan iş parçacığı: keşfedilen portları çizer,
 * ilerlemeyi günceller, tekerlek/ok ile kaydırmayı ve Esc ile iptali işler.
 * Hem hızlı hem derin aşama boyunca çalışır, sonunda bir kez durdurulur. */
static void *scan_animation_worker(void *arg)
{
    scan_animation_state *state = arg;
    nero_scan_session *s = state->session;
    const struct timespec frame_delay = {0, SCAN_FRAME_NANOSECONDS};
    unsigned frame = 0;
    int shown_permille = 0;

    for (;;) {
        /* One last frame after stop shows the final ledger and progress. */
        int stopping = atomic_load_explicit(&state->stop, memory_order_relaxed);
        int ports[DISCOVERY_VIEW_CAP];
        int total_open = 0;
        int shown = 0;

        pthread_mutex_lock(&s->results_mutex);
        for (int i = 1; i <= SCAN_TOTAL_PORTS; ++i) {
            if (s->results_list[i].state == PORT_OPEN) {
                if (shown < DISCOVERY_VIEW_CAP)
                    ports[shown++] = i;
                total_open++;
            }
        }
        pthread_mutex_unlock(&s->results_mutex);

        /* A later deep round enlarges the denominator; hold the bar rather
         * than letting it move backwards, and never show 100% early. */
        int permille = scan_progress_permille(s);
        if (permille < shown_permille)
            permille = shown_permille;
        shown_permille = permille;

        int scroll_delta = 0;
        /* Once Enter (select) or Esc (cancel) has ended discovery, stop reading
         * keys: the next keystroke belongs to the port menu, and consuming it
         * here would swallow a quick second Enter and leave the menu waiting. */
        int handed_off = stopping ||
                         atomic_load_explicit(&s->scan_select_requested, memory_order_relaxed) ||
                         atomic_load_explicit(&s->scan_cancel_requested, memory_order_relaxed);
        switch (handed_off ? NERO_UI_KEY_NONE : nero_operation_read_key(0)) {
        case NERO_UI_KEY_CANCEL:
            atomic_store_explicit(&s->scan_cancel_requested, 1, memory_order_relaxed);
            break;
        case NERO_UI_KEY_WHEEL_UP:
            scroll_delta = -1;
            break;
        case NERO_UI_KEY_WHEEL_DOWN:
            scroll_delta = 1;
            break;
        case NERO_UI_KEY_UP:
            scroll_delta = -1;
            break;
        case NERO_UI_KEY_DOWN:
            scroll_delta = 1;
            break;
        case NERO_UI_KEY_ENTER:
            /* Stop the scan early and let the user pick from the ports already
             * found, but only once at least one open port exists. */
            if (total_open > 0) {
                atomic_store_explicit(&s->scan_select_requested, 1, memory_order_relaxed);
                SCAN_TEST_SIGNAL("ENTER-STOP");
            } else {
                SCAN_TEST_SIGNAL("ENTER-IGNORED");
            }
            break;
        default:
            break;
        }

        nero_operation_discovery(ports, shown, total_open, state->animate ? frame : 6,
                                 atomic_load_explicit(&s->latest_probe_port, memory_order_relaxed),
                                 permille, scroll_delta);
        if (stopping)
            break;
        frame = (frame + 1) % 6;
        nanosleep(&frame_delay, NULL);
    }
    return NULL;
}

/* Terminali, kaydedilen ayarlara (satır modu) geri döndürür. Idempotent. */
void disable_raw_mode(void)
{
    if (!raw_mode_active)
        return;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &orig_termios);
    raw_mode_active = 0;
}

/* Anlık tuş/fare girişi için raw moda geçer; orijinal termios ayarlarını saklar
 * ve normal çıkışta eski haline döndürmek için bir kez atexit kaydı yapar. */
int enable_raw_mode(void)
{
    static int atexit_installed = 0;
    if (!isatty(STDIN_FILENO))
        return -1;
    if (tcgetattr(STDIN_FILENO, &orig_termios) != 0)
        return -1;
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) != 0)
        return -1;
    raw_mode_active = 1;
    if (!atexit_installed) {
        if (atexit(disable_raw_mode) == 0)
            atexit_installed = 1;
    }
    return 0;
}

/* SGR (1006) + normal (1000) fare raporlamasını açar/kapatır; liste
 * kaydırmada tekerleği okuyabilmek için. */
static void set_mouse_reporting(int enable)
{
    fputs(enable ? "\033[?1000h\033[?1006h" : "\033[?1006l\033[?1000l", stdout);
    fflush(stdout);
}

/* Port listesini artan sırada göstermek için qsort karşılaştırıcısı. */
static int compare_ports_asc(const void *a, const void *b)
{
    return (*(const int *)a - *(const int *)b);
}

/* Reserve descriptors for the terminal/runtime. Never raise the user's limit.
 * This budget applies to all scanner workers together, not to each worker. */

/* --- Port seçim menüsü --- */
static int select_port_menu_in_ui(int *open_ports, int count, nero_operation_ui *ui)
{
    if (count == 0)
        return 0;
    qsort(open_ports, count, sizeof(int), compare_ports_asc);
    int raw_mode = enable_raw_mode() == 0;
    if (raw_mode)
        set_mouse_reporting(1);
    int current_selection = 0;
    int input_ok = 1;

    while (1) {
        nero_operation_ports(ui, open_ports, count, current_selection);
        if (portscanner_interrupt_requested) {
            if (raw_mode) {
                set_mouse_reporting(0);
                disable_raw_mode();
            }
            return PORTSCANNER_CANCELLED;
        }
        nero_ui_key key = nero_operation_read_key(100);
        if (key == NERO_UI_KEY_EOF) {
            input_ok = 0;
            break;
        }
        if (key == NERO_UI_KEY_CANCEL) {
            if (raw_mode) {
                set_mouse_reporting(0);
                disable_raw_mode();
            }
            return PORTSCANNER_CANCELLED;
        }
        if (key == NERO_UI_KEY_UP && current_selection > 0)
            current_selection--;
        else if (key == NERO_UI_KEY_DOWN && current_selection < count - 1)
            current_selection++;
        else if (key == NERO_UI_KEY_WHEEL_UP) {
            current_selection--;
            if (current_selection < 0)
                current_selection = 0;
        } else if (key == NERO_UI_KEY_WHEEL_DOWN) {
            current_selection++;
            if (current_selection > count - 1)
                current_selection = count - 1;
        } else if (key == NERO_UI_KEY_ENTER)
            break;
    }
    if (raw_mode) {
        set_mouse_reporting(0);
        disable_raw_mode();
    }
    if (!input_ok) {
        printf("[!] Port selection aborted (input ended).\n");
        return -1;
    }
    return open_ports[current_selection];
}

/* Kendi operasyon yüzeyini açıp port seçim menüsünü gösteren genel sarmalayıcı. */
int select_port_menu(int *open_ports, int count)
{
    nero_operation_ui ui = {0};
    nero_operation_begin(&ui, "PORT SELECTION", "Choose an open port", "Esc · Back");
    int result = select_port_menu_in_ui(open_ports, count, &ui);
    nero_operation_end(&ui);
    return result;
}

/* --- Banner / sürüm yakalama ---
 * Açık bir porta bağlanıp gelen ilk satırı (banner) okur. SSH/FTP/SMTP/POP3/
 * IMAP gibi servisler bağlantıda kendiliğinden banner yollar; HTTP portlarına
 * küçük bir istek gönderilir. Yazdırılamayan baytlar '.' ile değiştirilir ve
 * yalnızca ilk satır, kısaltılarak saklanır. */
#define BANNER_MAX 72

/* Banner için küçük bir HTTP isteği gönderilecek yaygın düz-metin HTTP portları. */
static int is_http_port(int port)
{
    return port == 80 || port == 8080 || port == 8000 || port == 8888 || port == 9000 ||
           port == 591 || port == 8008;
}

/* Nonblocking banner I/O shares the scanner's bounded cancellation latency. */
static int banner_wait(nero_scan_session *s, nero_socket_t fd, short events, int64_t deadline)
{
    while (!scan_cancelled(s)) {
        int64_t left = deadline - scan_now_ms(s);
        if (left <= 0)
            return 0;
        struct pollfd p = {fd, events, 0};
        int result = s->io.poll_fn(&p, 1, left < CANCEL_POLL_MS ? (int)left : CANCEL_POLL_MS);
        if (result > 0)
            return (p.revents & events) && !(p.revents & POLLNVAL);
        if (result < 0 && errno != EINTR
#ifdef _WIN32
            && errno != WSAEINTR
#endif
        )
            return 0;
    }
    return 0;
}

static void grab_banner(nero_scan_session *s, const char *ip, int port, char *out, size_t outsz)
{
    if (!outsz)
        return;
    out[0] = '\0';
    if (scan_cancelled(s))
        return;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
#if defined(__APPLE__)
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
        return;
    nero_socket_t fd = s->io.socket_fn(AF_INET, SOCK_STREAM, 0);
    if (fd == NERO_INVALID_SOCKET)
        return;
    if (s->io.nonblocking_fn(fd) != 0) {
        s->io.close_fn(fd);
        return;
    }
    int r = s->io.connect_fn(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0) {
        int error = errno;
        if (!connect_pending(error) || !banner_wait(s, fd, POLLOUT, scan_now_ms(s) + 1200)) {
            s->io.close_fn(fd);
            return;
        }
        socklen_t len = sizeof(error);
        if (s->io.getsockopt_fn(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error) {
            s->io.close_fn(fd);
            return;
        }
    }
    if (is_http_port(port) && !scan_cancelled(s)) {
        const char *req = "HEAD / HTTP/1.0\r\n\r\n";
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
        int no_sigpipe = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
        (void)send(fd, req, strlen(req), flags);
    }
    char buf[256];
    ssize_t n = -1;
    if (banner_wait(s, fd, POLLIN, scan_now_ms(s) + 1200) && !scan_cancelled(s))
        n = recv(fd, buf, sizeof(buf) - 1, 0);
    s->io.close_fn(fd);
    if (n <= 0)
        return;
    size_t o = 0;
    for (ssize_t i = 0; i < n && o < outsz - 1; ++i) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r' || c == '\n')
            break;
        out[o++] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    out[o] = '\0';
}

typedef struct {
    nero_scan_session *session;
    const char *ip;
    const int *ports;
    int start, end;
    char (*store)[BANNER_MAX];
} banner_args;

/* Banner yakalamayı paralelleştiren iş parçacığı: kendine düşen port aralığını
 * işler ve yalnızca kendi store[] satırlarına yazdığından kilit gerekmez. */
static void *banner_worker(void *p)
{
    banner_args *b = p;
    nero_scan_session *s = b->session;
    for (int i = b->start; i < b->end; ++i) {
        if (portscanner_interrupt_requested ||
            atomic_load_explicit(&s->scan_cancel_requested, memory_order_relaxed))
            break;
        grab_banner(s, b->ip, b->ports[i], b->store[i], BANNER_MAX);
    }
    return NULL;
}

/* Açık portlar için banner'ları paralel yakalar (çok iş parçacığı, her biri
 * kendi dizin aralığına yazar; kilit gerekmez). En çok `cap` port denenir. */
#define BANNER_THREADS 12
#define BANNER_PORT_CAP 128
void grab_banners(nero_scan_session *s, const char *ip, const int *ports, int count,
                  char (*store)[BANNER_MAX])
{
    if (count <= 0)
        return;
    int threads = count < BANNER_THREADS ? count : BANNER_THREADS;
    pthread_t tids[BANNER_THREADS];
    banner_args args[BANNER_THREADS];
    int created = 0;
    for (int i = 0; i < threads; ++i) {
        args[i].session = s;
        args[i].ip = ip;
        args[i].ports = ports;
        args[i].store = store;
        args[i].start = (i * count) / threads;
        args[i].end = ((i + 1) * count) / threads;
        int thread_error = s->io.thread_create_fn(&tids[i], NULL, banner_worker, &args[i]);
        if (thread_error != 0) {
            atomic_fetch_add(&s->worker_failures, 1);
            nero_log_write(NERO_LOG_WARN, "scanner", "Banner worker creation failed: error=%d (%s)",
                           thread_error, strerror(thread_error));
            fprintf(stderr, "[!] Optional banner checks incomplete (thread creation failed).\n");
            break;
        }
        created++;
    }
    for (int i = 0; i < created; ++i)
        pthread_join(tids[i], NULL);
}

/* Optional ICMP discovery is advisory and opt-in. Only numeric, validated
 * IPv4 addresses are passed as an argv element (never through a shell). Lack
 * of ping permissions, binary, response or route always means unknown. */
host_state host_probe_alive(nero_scan_session *s, const char *ip)
{
    struct in_addr parsed;
    if (!ip || inet_pton(AF_INET, ip, &parsed) != 1)
        return HOST_UNKNOWN;
    const char *icmp = getenv("NERO_SCAN_ICMP");
    if (!icmp || strcmp(icmp, "1") != 0 || scan_cancelled(s))
        return HOST_UNKNOWN;
#ifdef _WIN32
    char command[128];
    snprintf(command, sizeof(command), "ping.exe -n 1 -w 1000 %s", ip);
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup,
                        &process))
        return HOST_UNKNOWN;
    int64_t deadline = scan_now_ms(s) + 1700;
    DWORD code = 1;
    while (WaitForSingleObject(process.hProcess, CANCEL_POLL_MS) == WAIT_TIMEOUT) {
        if (scan_cancelled(s) || scan_now_ms(s) >= deadline) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
    }
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0 ? HOST_ONLINE : HOST_UNKNOWN;
#else
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0)
        return HOST_UNKNOWN;
    int ok = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    ok |= posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    ok |= posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
#if defined(__APPLE__)
    char *args[] = {"ping", "-n", "-c", "1", "-W", "1000", (char *)ip, NULL};
#else
    char *args[] = {"ping", "-n", "-c", "1", "-W", "1", (char *)ip, NULL};
#endif
    extern char **environ;
    pid_t pid = -1;
    int spawned = ok == 0 ? posix_spawnp(&pid, "ping", &actions, NULL, args, environ) : -1;
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0)
        return HOST_UNKNOWN;
    int64_t deadline = scan_now_ms(s) + 1700;
    int status = 0;
    for (;;) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? HOST_ONLINE : HOST_UNKNOWN;
        if (result < 0 && errno != EINTR)
            return HOST_UNKNOWN;
        if (scan_cancelled(s) || scan_now_ms(s) >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            return HOST_UNKNOWN;
        }
        const struct timespec delay = {0, CANCEL_POLL_MS * 1000000L};
        nanosleep(&delay, NULL);
    }
#endif
}

/* --- Manuel hız ayarı ekranı ---
 * Üç satır (iş parçacığı, parti başına soket, zaman aşımı) ←/→ ile ayarlanır.
 * Enter taramayı başlatır, Esc menüye döner. manual_profile'ı günceller.
 * 1 döndürürse kullanıcı onayladı, 0 döndürürse iptal etti. */
static int adjust_manual_profile(nero_operation_ui *ui)
{
    int threads = manual_profile.fast_threads;
    int batch = manual_profile.fast_batch;
    int timeout = manual_profile.fast_timeout_ms;
    int row = 0;
    ui->controls = "Up/Down: Satir   Left/Right: Degistir   Enter: Basla   Esc: Geri";

    for (;;) {
        char v0[24], v1[24], v2[24];
        snprintf(v0, sizeof v0, "%d", threads);
        snprintf(v1, sizeof v1, "%d", batch);
        snprintf(v2, sizeof v2, "%d ms", timeout);
        const char *labels[3] = {"Is parcacigi", "Parti basina soket", "Zaman asimi"};
        const char *values[3] = {v0, v1, v2};
        nero_operation_menu(ui, "Manuel hiz ayari", labels, values, 3, row);
        if (portscanner_interrupt_requested)
            return 0;

        nero_ui_key k = nero_operation_read_key(100);
        if (k == NERO_UI_KEY_CANCEL || k == NERO_UI_KEY_EOF)
            return 0;
        if (k == NERO_UI_KEY_ENTER)
            break;
        if (k == NERO_UI_KEY_UP || k == NERO_UI_KEY_WHEEL_UP) {
            if (row > 0)
                row--;
        } else if (k == NERO_UI_KEY_DOWN || k == NERO_UI_KEY_WHEEL_DOWN) {
            if (row < 2)
                row++;
        } else if (k == NERO_UI_KEY_LEFT || k == NERO_UI_KEY_RIGHT) {
            int dir = (k == NERO_UI_KEY_RIGHT) ? 1 : -1;
            if (row == 0) {
                threads += dir;
                if (threads < 1)
                    threads = 1;
                if (threads > MAX_THREADS)
                    threads = MAX_THREADS;
            } else if (row == 1) {
                batch += dir * 16;
                if (batch < 16)
                    batch = 16;
                if (batch > MAX_BATCH)
                    batch = MAX_BATCH;
            } else {
                timeout += dir * 100;
                if (timeout < 200)
                    timeout = 200;
                if (timeout > 5000)
                    timeout = 5000;
            }
        }
    }

    manual_profile.fast_threads = threads;
    manual_profile.fast_batch = batch;
    manual_profile.fast_timeout_ms = timeout;
    /* Derin aşama: daha nazik ve daha uzun bekleyen türev değerler. */
    manual_profile.deep_threads = threads > 1 ? threads / 2 : 1;
    manual_profile.deep_batch = batch > 32 ? batch / 2 : 16;
    manual_profile.deep_timeout_ms = timeout * 2 > 5000 ? 5000 : timeout * 2;
    manual_profile.deep_max_candidates = 65535; /* manuelde her zaman derin tara */
    manual_profile.deep_rounds = 2;
    return 1;
}

/* --- Tarama hızı seçim menüsü ---
 * İnteraktif değilse menü göstermeden önerilen (Orta) profili kullanır.
 * Kullanıcı iptal ederse NULL döndürür. */
static const scan_profile *select_scan_profile(nero_operation_ui *ui)
{
    if (!ui->interactive)
        return &SCAN_PROFILES[PROFILE_MEDIUM];

    int raw = enable_raw_mode() == 0;
    if (raw)
        set_mouse_reporting(1);

    const char *labels[PROFILE_COUNT] = {"Hizli", "Orta  (onerilen)", "Yavas", "Manuel ayar"};
    char vbuf[PROFILE_COUNT][40];
    const char *values[PROFILE_COUNT];
    for (int i = 0; i < 3; ++i) {
        snprintf(vbuf[i], sizeof vbuf[i], "%dx%d soket, %d ms", SCAN_PROFILES[i].fast_threads,
                 SCAN_PROFILES[i].fast_batch, SCAN_PROFILES[i].fast_timeout_ms);
        values[i] = vbuf[i];
    }

    int sel = PROFILE_MEDIUM;
    const scan_profile *chosen = NULL;
    ui->controls = "Up/Down: Sec   Enter: Onayla   Esc: Iptal";

    for (;;) {
        snprintf(vbuf[PROFILE_MANUAL], sizeof vbuf[PROFILE_MANUAL], "%dx%d soket, %d ms",
                 manual_profile.fast_threads, manual_profile.fast_batch,
                 manual_profile.fast_timeout_ms);
        values[PROFILE_MANUAL] = vbuf[PROFILE_MANUAL];

        nero_operation_menu(ui, "Tarama hizi secin", labels, values, PROFILE_COUNT, sel);
        if (portscanner_interrupt_requested) {
            chosen = NULL;
            break;
        }

        nero_ui_key k = nero_operation_read_key(100);
        if (k == NERO_UI_KEY_CANCEL || k == NERO_UI_KEY_EOF) {
            chosen = NULL;
            break;
        }
        if (k == NERO_UI_KEY_UP || k == NERO_UI_KEY_WHEEL_UP) {
            if (sel > 0)
                sel--;
        } else if (k == NERO_UI_KEY_DOWN || k == NERO_UI_KEY_WHEEL_DOWN) {
            if (sel < PROFILE_COUNT - 1)
                sel++;
        } else if (k == NERO_UI_KEY_ENTER) {
            if (sel == PROFILE_MANUAL) {
                if (adjust_manual_profile(ui)) {
                    chosen = &manual_profile;
                    break;
                }
                if (portscanner_interrupt_requested) {
                    chosen = NULL;
                    break;
                }
                ui->controls = "Up/Down: Sec   Enter: Onayla   Esc: Iptal";
            } else {
                chosen = &SCAN_PROFILES[sel];
                break;
            }
        }
    }

    if (raw) {
        set_mouse_reporting(0);
        disable_raw_mode();
    }
    return chosen;
}

int scan_cli_cancel(void *context)
{
    scan_cli_input *input = context;
    return input->fallback && pthread_equal(pthread_self(), input->owner) &&
           nero_operation_read_key(0) == NERO_UI_KEY_CANCEL;
}
static void scan_cli_open(void *context)
{
    (void)context;
    SCAN_TEST_SIGNAL("OPEN");
}
/* --- Ana kontrolcü --- */
int portscanner_with_io(const char *target_ip, const scan_io *io)
{
    struct in_addr address;
    if (!target_ip || inet_pton(AF_INET, target_ip, &address) != 1) {
        nero_log_write(NERO_LOG_ERROR, "scanner", "Invalid IPv4 target (input omitted)");
        fprintf(stderr, "[-] Invalid IPv4 target.\n");
        return PORTSCANNER_FAILED;
    }
    nero_scan_session *s = scan_create_with_io(target_ip, NULL, io);
    if (!s)
        return PORTSCANNER_FAILED;
    scan_cli_input input = {.owner = pthread_self(), .fallback = 0};
    s->check_cancel = scan_cli_cancel;
    s->cancel_context = &input;
    s->interrupt_requested = &portscanner_interrupt_requested;
    s->on_open = scan_cli_open;
    portscanner_interrupt_requested = 0;
    snprintf(s->scan_operation_id, sizeof s->scan_operation_id, "%ld-%u", (long)getpid(),
             atomic_fetch_add(&scan_sequence, 1) + 1);
    nero_log_write(NERO_LOG_INFO, "scanner", "op=%s TCP scan started: target=%s ports=1-65535",
                   s->scan_operation_id, target_ip);
    int *work_ports = s->work_ports;
    const scan_profile *active_profile;
    nero_operation_ui ui = {0};
    char detail[96];
    snprintf(detail, sizeof(detail), "%s", target_ip);
    nero_operation_begin(&ui, "Port scan", detail, "Enter: port sec  ·  Esc: Geri");
    active_profile = select_scan_profile(&ui);
    if (active_profile == NULL) {
        nero_scan_destroy(s);
        nero_operation_end(&ui);
        nero_log_write(NERO_LOG_WARN, "scanner", "Scan cancelled at profile selection");
        puts("Scan cancelled. Returned to terminal.");
        return PORTSCANNER_CANCELLED;
    }

    /* Discovery never gates a valid target. The full fast phase supplies TCP
     * reachability evidence itself, without an extra 16-port pre-scan. */
    const int64_t scan_started_ms = scan_now_ms(s);
    if (host_probe_alive(s, target_ip) == HOST_ONLINE)
        atomic_store_explicit(&s->host_responded, 1, memory_order_relaxed);
    nero_operation_status("36", "Host discovery",
                          atomic_load_explicit(&s->host_responded, memory_order_relaxed)
                              ? "online; scanning all TCP ports"
                              : "unknown; scanning all TCP ports");
    const scan_profile *prof = active_profile;
    s->scan_profile_label = prof->name;
    scan_animation_state animation = {
        .session = s, .target_ip = target_ip, .animate = scan_motion_enabled()};
    atomic_init(&animation.stop, 0);
    pthread_t animation_thread;
    int animation_started = 0, discovery_raw = 0;
    if (isatty(STDIN_FILENO)) {
        discovery_raw = (enable_raw_mode() == 0);
        if (discovery_raw)
            set_mouse_reporting(1);
        int animation_error =
            s->io.thread_create_fn(&animation_thread, NULL, scan_animation_worker, &animation);
        animation_started = animation_error == 0;
        if (animation_error) {
            atomic_fetch_add(&s->worker_failures, 1);
            nero_log_write(NERO_LOG_WARN, "scanner", "op=%s Animation worker unavailable: error=%d",
                           s->scan_operation_id, animation_error);
        }
    }
    input.fallback = discovery_raw && !animation_started;
    if (!animation_started)
        nero_operation_loader("Scanning", target_ip, 6);
    SCAN_TEST_SIGNAL("SCAN-START");
    s->profile = *prof;
    nero_log_write(NERO_LOG_INFO, "scanner",
                   "event=SCAN_STARTED op=%s version=%s target=%s profile=%s "
                   "fast_threads=%d fast_batch=%d fast_timeout_ms=%d deep_threads=%d "
                   "deep_batch=%d deep_timeout_ms=%d deep_rounds=%d socket_budget=%d",
                   s->scan_operation_id, NERO_BUILD_VERSION, target_ip, prof->name,
                   prof->fast_threads, prof->fast_batch, prof->fast_timeout_ms, prof->deep_threads,
                   prof->deep_batch, prof->deep_timeout_ms, prof->deep_rounds,
                   scan_socket_budget());
    nero_scan_status run_status = nero_scan_run(s);
    int candidate_count = s->retry_candidates;

    if (animation_started) {
        atomic_store_explicit(&animation.stop, 1, memory_order_relaxed);
        pthread_join(animation_thread, NULL);
    }
    if (discovery_raw) {
        set_mouse_reporting(0);
        disable_raw_mode();
    }
    input.fallback = 0;
    scan_summary summary = summarize_scan(s);
    int fatal = atomic_load(&s->fatal_error);
    int cancelled = scan_cancelled(s) && !fatal;
    /* A deliberate early stop (the user pressed Enter to pick from the ports
     * found so far). It is not a cancel and not a coverage failure: the menu is
     * still shown, and the summary reports the partial coverage honestly. */
    int selected_early =
        !cancelled && atomic_load_explicit(&s->scan_select_requested, memory_order_relaxed);
    int accounted = summary.open + summary.closed + summary.filtered + summary.unknown;
    /* A monotonic-clock failure makes scan_now_ms() return 0; never let that, or
     * any clock anomaly, print a negative duration. */
    int64_t scan_ended_ms = scan_now_ms(s);
    s->scan_duration_ms = scan_ended_ms > scan_started_ms ? scan_ended_ms - scan_started_ms : 0;
    s->scan_completion = cancelled ? "cancelled"
                         : selected_early
                             ? "stopped early by user (partial coverage)"
                             : (run_status == NERO_SCAN_COMPLETE ? "complete" : "incomplete");
    if (fatal || cancelled ||
        (!selected_early && (accounted != SCAN_TOTAL_PORTS || summary.retried < candidate_count))) {
        nero_operation_end(&ui);
        report_scan_summary(s);
        if (!cancelled && summary.retried < candidate_count)
            printf("Retry checks incomplete: %d of %d candidates.\n", summary.retried,
                   candidate_count);
        nero_scan_destroy(s);
        puts(cancelled ? "Scan cancelled. Returned to terminal." : "[-] Scan coverage incomplete.");
        return cancelled ? PORTSCANNER_CANCELLED : PORTSCANNER_FAILED;
    }

    /* Reuse the work array for the unchanged open-port selection screen. */
    int *open_ports = work_ports;
    int open_count = 0;
    for (int port = 1; port <= SCAN_TOTAL_PORTS; ++port)
        if (s->results_list[port].state == PORT_OPEN)
            open_ports[open_count++] = port;
    char result_detail[160];
    snprintf(result_detail, sizeof(result_detail),
             "%d open, %d closed, %d filtered, %d error/unknown", summary.open, summary.closed,
             summary.filtered, summary.unknown);
    nero_operation_status(selected_early ? "33" : "32",
                          selected_early ? "Scan stopped early (partial)" : "Scan complete",
                          result_detail);

    /* Açık portlar için servis banner'larını yakala (varsa menüde gösterilir).
     * Port değerine göre arandığından menüdeki sıralamadan bağımsızdır. */
    int *banner_port_list = NULL;
    char (*banner_store)[BANNER_MAX] = NULL;
    const char **banner_texts = NULL;
    int banner_n = open_count < BANNER_PORT_CAP ? open_count : BANNER_PORT_CAP;
    if (open_ports && banner_n > 0) {
        banner_port_list = malloc(banner_n * sizeof(int));
        banner_store = malloc(banner_n * sizeof(*banner_store));
        banner_texts = malloc(banner_n * sizeof(*banner_texts));
        if (banner_port_list && banner_store && banner_texts) {
            for (int i = 0; i < banner_n; ++i) {
                banner_port_list[i] = open_ports[i];
                banner_store[i][0] = '\0';
            }
            nero_operation_status("36", "Servis bilgileri toplaniyor", result_detail);
            grab_banners(s, target_ip, banner_port_list, banner_n, banner_store);
            for (int i = 0; i < banner_n; ++i)
                banner_texts[i] = banner_store[i];
            ui.banner_ports = banner_port_list;
            ui.banner_texts = (const char *const *)banner_texts;
            ui.banner_count = banner_n;
            nero_operation_status(selected_early ? "33" : "32",
                                  selected_early ? "Scan stopped early (partial)" : "Scan complete",
                                  result_detail);
        } else {
            nero_log_write(NERO_LOG_WARN, "scanner",
                           "Optional banner buffers could not be allocated");
            free(banner_port_list);
            free(banner_store);
            free(banner_texts);
            banner_port_list = NULL;
            banner_store = NULL;
            banner_texts = NULL;
        }
    }

    int selected_port = scan_cancelled(s) ? PORTSCANNER_CANCELLED
                                          : select_port_menu_in_ui(open_ports, open_count, &ui);
    free(banner_port_list);
    free(banner_store);
    free(banner_texts);

    nero_operation_end(&ui);
    report_scan_summary(s);
    if (selected_early && summary.unscanned)
        printf("Scan stopped early at your request: %d of %d TCP ports were not scanned.\n",
               summary.unscanned, SCAN_TOTAL_PORTS);
    if (summary.filtered || summary.unknown)
        puts("Some port checks remain inconclusive after bounded retries.");
    nero_scan_destroy(s);
    if (selected_port == PORTSCANNER_CANCELLED)
        puts("Port selection cancelled. Returned to terminal.");
    return selected_port == 0 && summary.unknown > 0 ? PORTSCANNER_FAILED : selected_port;
}

int portscanner(const char *target_ip)
{
    return portscanner_with_io(target_ip, NULL);
}
