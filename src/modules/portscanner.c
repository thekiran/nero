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
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <poll.h>
#include <sys/resource.h>
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>

#include "nero/operation_ui.h"
#include "nero/socket_handle.h"
#include "nero/portscanner.h"

/* Genel amaçlı tarayıcı: her IP için tam 1-65535 aralığı taranır. */
#define SCAN_TOTAL_PORTS 65535
#define DISCOVERY_VIEW_CAP 512

/* Yığın dizilerinin derleme-zamanı üst sınırları. Tüm profiller ve manuel
 * ayar bu sınırların altında tutulur (poll() kullandığımız için FD_SETSIZE
 * sorunu yoktur; sınırlar yalnızca bellek ve hedefe nezaket içindir). */
#define MAX_THREADS 16
#define MAX_BATCH   512
#define MAX_SCAN_SOCKETS 256
#define CANCEL_POLL_MS 50

/* Bir tarama hız profili. Daha düşük eşzamanlılık = hedefi/WiFi'yi daha az
 * boğar = daha kararlı; daha kısa zaman aşımı = daha hızlı bitiş. */
typedef struct {
    const char *name;
    int fast_threads, fast_batch, fast_timeout_ms;
    int deep_threads, deep_batch, deep_timeout_ms;
    int deep_max_candidates; /* work window size, never a candidate cutoff */
} scan_profile;

enum { PROFILE_FAST = 0, PROFILE_MEDIUM, PROFILE_SLOW, PROFILE_MANUAL, PROFILE_COUNT };

/* name alanları hız menüsünde de gösterilir. */
static const scan_profile SCAN_PROFILES[] = {
    /* Fast, balanced and slow timeout/batch preferences. The global socket
     * budget is enforced independently; all profiles retry every uncertainty. */
    { "Hizli",  8, 256, 500,  4, 128, 1000, 8000 },
    /* Orta (önerilen): dengeli. */
    { "Orta",   6, 200, 1000, 4, 100, 2000, 12000 },
    /* Yavaş: düşük eşzamanlılık, uzun zaman aşımı. En kararlı/eksiksiz. */
    { "Yavas",  4, 100, 1500, 2, 64,  3000, 65535 },
};

/* Manuel profil: kullanıcının ayarladığı değerlerle doldurulur. */
static scan_profile manual_profile = { "Manuel", 6, 200, 1000, 4, 100, 2000, 12000 };

/* Tarama boyunca okunur (yalnız-okuma; thread'ler güvenle paylaşır). */
static const scan_profile *active_profile = &SCAN_PROFILES[PROFILE_MEDIUM];

typedef enum {
    PORT_UNSCANNED = 0, PORT_OPEN, PORT_CLOSED, PORT_FILTERED, PORT_UNKNOWN
} port_state;

typedef struct {
    port_state state;
    int error;
    unsigned attempts;
    unsigned retries_completed;
} port_result;

typedef struct {
    atomic_int next;
    const int *ports; /* NULL means the complete 1..65535 range. */
    int count, timeout_ms, batch;
} scan_work;

typedef struct {
    int open, closed, filtered, unknown, unscanned, retried;
} scan_summary;

typedef enum { HOST_UNKNOWN, HOST_ONLINE } host_state;
/* Silence, routing errors and stale addresses cannot establish HOST_OFFLINE. */

static struct termios orig_termios;
static port_result *results_list = NULL; /* [1..65535], allocated once */
volatile sig_atomic_t portscanner_interrupt_requested = 0;
static atomic_int scan_cancel_requested;
static atomic_int ports_scanned;
static atomic_int latest_probe_port;
static atomic_int host_responded;
static const char *target_ip_global;
static pthread_mutex_t results_mutex;
static pthread_t scan_owner;
static int scan_input_fallback;

static int scan_cancelled(void) {
    if (scan_input_fallback && pthread_equal(pthread_self(), scan_owner) &&
        nero_operation_read_key(0) == NERO_UI_KEY_CANCEL)
        atomic_store_explicit(&scan_cancel_requested, 1, memory_order_relaxed);
    return portscanner_interrupt_requested ||
        atomic_load_explicit(&scan_cancel_requested, memory_order_relaxed);
}

static int64_t scan_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static scan_summary summarize_scan(void) {
    scan_summary s = {0};
    for (int port = 1; port <= SCAN_TOTAL_PORTS; ++port) {
        switch (results_list[port].state) {
        case PORT_OPEN: s.open++; break;
        case PORT_CLOSED: s.closed++; break;
        case PORT_FILTERED: s.filtered++; break;
        case PORT_UNKNOWN: s.unknown++; break;
        case PORT_UNSCANNED: s.unscanned++; break;
        }
        if (results_list[port].retries_completed) s.retried++;
    }
    return s;
}

static void record_result(int port, port_state state, int error) {
    pthread_mutex_lock(&results_mutex);
    if (results_list[port].state == PORT_UNSCANNED)
        atomic_fetch_add_explicit(&ports_scanned, 1, memory_order_relaxed);
    else
        results_list[port].retries_completed++;
    results_list[port].state = state;
    results_list[port].error = error;
    pthread_mutex_unlock(&results_mutex);
    if (state == PORT_OPEN || state == PORT_CLOSED)
        atomic_store_explicit(&host_responded, 1, memory_order_relaxed);
}

static void report_scan_summary(void) {
    scan_summary s = summarize_scan(); /* caller has joined every writer */
    printf("Host: %s | TCP: open=%d closed=%d filtered=%d error/unknown=%d "
           "unscanned=%d total=%d | retried=%d\n",
           atomic_load_explicit(&host_responded, memory_order_relaxed) ? "online" : "unknown",
           s.open, s.closed, s.filtered, s.unknown, s.unscanned,
           s.open + s.closed + s.filtered + s.unknown + s.unscanned, s.retried);
}

/* --- Terminal yardımcıları --- */
static int raw_mode_active = 0;
#define SCAN_FRAME_NANOSECONDS 70000000L

typedef struct {
    atomic_int stop;
    const char *target_ip;
    int animate;
} scan_animation_state;

/* Animasyon yalnızca gerçek bir terminalde, NERO_REDUCE_MOTION tanımsızken ve
 * TERM "dumb" değilken etkindir. */
static int scan_motion_enabled(void) {
    const char *reduced = getenv("NERO_REDUCE_MOTION");
    const char *term = getenv("TERM");
#ifdef _WIN32
    /* Windows konsollarında TERM kuralı yoktur; VT çıkışı nero_windows_init()
     * ile açılır, canlı konsol varsayılan olarak ANSI yeteneklidir. */
    return isatty(STDOUT_FILENO) && reduced == NULL &&
           (term == NULL || strcmp(term, "dumb") != 0);
#else
    return isatty(STDOUT_FILENO) && reduced == NULL &&
           (term == NULL || strcmp(term, "dumb") != 0);
#endif
}

/* Tarama sırasında ekranı canlı tutan iş parçacığı: keşfedilen portları çizer,
 * ilerlemeyi günceller, tekerlek/ok ile kaydırmayı ve Esc ile iptali işler.
 * Hem hızlı hem derin aşama boyunca çalışır, sonunda bir kez durdurulur. */
static void *scan_animation_worker(void *arg) {
    scan_animation_state *state = arg;
    const struct timespec frame_delay = {0, SCAN_FRAME_NANOSECONDS};
    unsigned frame = 0;

    while (!atomic_load_explicit(&state->stop, memory_order_relaxed)) {
        int ports[DISCOVERY_VIEW_CAP];
        int total_open = 0;
        int shown = 0;

        pthread_mutex_lock(&results_mutex);
        for (int i = 1; i <= SCAN_TOTAL_PORTS; ++i) {
            if (results_list[i].state == PORT_OPEN) {
                if (shown < DISCOVERY_VIEW_CAP)
                    ports[shown++] = i;
                total_open++;
            }
        }
        pthread_mutex_unlock(&results_mutex);

        int scanned = atomic_load_explicit(&ports_scanned, memory_order_relaxed);
        int permille = (int)((long)scanned * 1000 / SCAN_TOTAL_PORTS);
        if (permille > 1000) permille = 1000;

        int scroll_delta = 0;
        switch (nero_operation_read_key(0)) {
        case NERO_UI_KEY_CANCEL:
            atomic_store_explicit(&scan_cancel_requested, 1, memory_order_relaxed);
            break;
        case NERO_UI_KEY_WHEEL_UP:   scroll_delta = -1; break;
        case NERO_UI_KEY_WHEEL_DOWN: scroll_delta =  1; break;
        case NERO_UI_KEY_UP:         scroll_delta = -1; break;
        case NERO_UI_KEY_DOWN:       scroll_delta =  1; break;
        default: break;
        }

        nero_operation_discovery(ports, shown, total_open, state->animate ? frame : 6,
            atomic_load_explicit(&latest_probe_port, memory_order_relaxed),
            permille, scroll_delta);
        frame = (frame + 1) % 6;
        nanosleep(&frame_delay, NULL);
    }
    return NULL;
}

/* Terminali, kaydedilen ayarlara (satır modu) geri döndürür. Idempotent. */
void disable_raw_mode(void) {
    if (!raw_mode_active) return;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &orig_termios);
    raw_mode_active = 0;
}

/* Anlık tuş/fare girişi için raw moda geçer; orijinal termios ayarlarını saklar
 * ve normal çıkışta eski haline döndürmek için bir kez atexit kaydı yapar. */
int enable_raw_mode(void) {
    static int atexit_installed = 0;
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &orig_termios) != 0) return -1;
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) != 0) return -1;
    raw_mode_active = 1;
    if (!atexit_installed) {
        if (atexit(disable_raw_mode) == 0) atexit_installed = 1;
    }
    return 0;
}

/* SGR (1006) + normal (1000) fare raporlamasını açar/kapatır; liste
 * kaydırmada tekerleği okuyabilmek için. */
static void set_mouse_reporting(int enable) {
    fputs(enable ? "\033[?1000h\033[?1006h" : "\033[?1006l\033[?1000l", stdout);
    fflush(stdout);
}

/* Port listesini artan sırada göstermek için qsort karşılaştırıcısı. */
static int compare_ports_asc(const void *a, const void *b) {
    return (*(const int *)a - *(const int *)b);
}

/* Reserve descriptors for the terminal/runtime. Never raise the user's limit.
 * This budget applies to all scanner workers together, not to each worker. */
static int scan_socket_budget(void) {
    int budget = MAX_SCAN_SOCKETS;
#ifndef _WIN32
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        rlim_t available = rl.rlim_cur > 64 ? rl.rlim_cur - 64 : 1;
        if (available < (rlim_t)budget) budget = (int)available;
    }
#endif
    return budget;
}

/* --- Port seçim menüsü --- */
static int select_port_menu_in_ui(int *open_ports, int count, nero_operation_ui *ui) {
    if (count == 0) return 0;
    qsort(open_ports, count, sizeof(int), compare_ports_asc);
    int raw_mode = enable_raw_mode() == 0;
    if (raw_mode) set_mouse_reporting(1);
    int current_selection = 0;
    int input_ok = 1;

    while (1) {
        nero_operation_ports(ui, open_ports, count, current_selection);
        if (portscanner_interrupt_requested) {
            if (raw_mode) { set_mouse_reporting(0); disable_raw_mode(); }
            return PORTSCANNER_CANCELLED;
        }
        nero_ui_key key = nero_operation_read_key(100);
        if (key == NERO_UI_KEY_EOF) { input_ok = 0; break; }
        if (key == NERO_UI_KEY_CANCEL) {
            if (raw_mode) { set_mouse_reporting(0); disable_raw_mode(); }
            return PORTSCANNER_CANCELLED;
        }
        if (key == NERO_UI_KEY_UP && current_selection > 0) current_selection--;
        else if (key == NERO_UI_KEY_DOWN && current_selection < count - 1) current_selection++;
        else if (key == NERO_UI_KEY_WHEEL_UP) { current_selection--; if (current_selection < 0) current_selection = 0; }
        else if (key == NERO_UI_KEY_WHEEL_DOWN) { current_selection++; if (current_selection > count - 1) current_selection = count - 1; }
        else if (key == NERO_UI_KEY_ENTER) break;
    }
    if (raw_mode) { set_mouse_reporting(0); disable_raw_mode(); }
    if (!input_ok) { printf("[!] Port selection aborted (input ended).\n"); return -1; }
    return open_ports[current_selection];
}

/* Kendi operasyon yüzeyini açıp port seçim menüsünü gösteren genel sarmalayıcı. */
int select_port_menu(int *open_ports, int count) {
    nero_operation_ui ui = {0};
    nero_operation_begin(&ui, "PORT SELECTION", "Choose an open port", "Esc · Back");
    int result = select_port_menu_in_ui(open_ports, count, &ui);
    nero_operation_end(&ui);
    return result;
}

/* Only a refusal is classified as closed. A timeout is filtered (possibly
 * dropped); routing, permission, resource and API failures stay unknown. */
static port_state classify_connect_error(int error) {
    if (error == 0) return PORT_OPEN;
    if (error == ECONNREFUSED) return PORT_CLOSED;
    if (error == ETIMEDOUT) return PORT_FILTERED;
    return PORT_UNKNOWN;
}

static int connect_pending(int error) {
    return error == EINPROGRESS || error == EALREADY ||
           error == EWOULDBLOCK || error == EINTR
#ifdef _WIN32
           || error == WSAEINTR || error == WSAEINPROGRESS
#endif
           ;
}

static int make_nonblocking(nero_socket_t fd) {
#ifdef _WIN32
    u_long value = 1;
    if (ioctlsocket(fd, FIONBIO, &value) == 0) return 0;
    errno = WSAGetLastError();
    return -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void scan_ports_batch(const char *ip, const int *ports, int count, int timeout_ms) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
#if defined(__APPLE__)
    addr.sin_len = sizeof(addr);
#endif
    if (count <= 0 || count > MAX_BATCH || timeout_ms <= 0 ||
        inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return;

    struct pollfd pfds[MAX_BATCH];
    int slot_port[MAX_BATCH];
    int64_t deadline[MAX_BATCH];
    int nfds = 0, remaining = 0, resource_error = 0;
    for (int i = 0; i < count && !scan_cancelled(); ++i) {
        int port = ports[i];
        pthread_mutex_lock(&results_mutex);
        results_list[port].attempts++;
        pthread_mutex_unlock(&results_mutex);
        atomic_store_explicit(&latest_probe_port, port, memory_order_relaxed);
        nero_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == NERO_INVALID_SOCKET) {
            int error = errno;
            record_result(port, PORT_UNKNOWN, error);
            if (error == EMFILE || error == ENFILE || error == ENOBUFS || error == ENOMEM)
                resource_error = 1;
            continue;
        }
        if (make_nonblocking(fd) != 0) {
            int error = errno;
            close(fd);
            record_result(port, PORT_UNKNOWN, error);
            continue;
        }
        addr.sin_port = htons((uint16_t)port);
        deadline[nfds] = scan_now_ms() + timeout_ms;
        int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        int error = ret == 0 ? 0 : errno;
        if (ret == 0 || !connect_pending(error)) {
            close(fd);
            record_result(port, classify_connect_error(error), error);
            continue;
        }
        pfds[nfds].fd = fd;
        pfds[nfds].events = POLLOUT;
        pfds[nfds].revents = 0;
        slot_port[nfds++] = port;
        remaining++;
    }

    while (remaining && !scan_cancelled()) {
        int wait_ms = CANCEL_POLL_MS;
        int64_t now = scan_now_ms();
        for (int s = 0; s < nfds; ++s) {
            if (pfds[s].fd == NERO_INVALID_SOCKET) continue;
            int64_t left = deadline[s] - now;
            if (left <= 0) wait_ms = 0;
            else if (left < wait_ms) wait_ms = (int)left;
        }
        for (int s = 0; s < nfds; ++s) pfds[s].revents = 0;
        int before = remaining;
        int ready = nero_socket_poll(pfds, (nfds_t)nfds, wait_ms);
        if (scan_cancelled()) break;
        if (ready < 0) {
            int error = errno;
            if (error == EINTR
#ifdef _WIN32
                || error == WSAEINTR
#endif
               ) {
                /* poll may leave revents unspecified on error. Still expire
                 * absolute deadlines even under a stream of interruptions. */
                ready = 0;
                for (int s = 0; s < nfds; ++s) pfds[s].revents = 0;
            } else {
                for (int s = 0; s < nfds; ++s) {
                    if (pfds[s].fd == NERO_INVALID_SOCKET) continue;
                    close(pfds[s].fd);
                    pfds[s].fd = NERO_INVALID_SOCKET;
                    record_result(slot_port[s], PORT_UNKNOWN, error);
                }
                break;
            }
        }
        for (int s = 0; s < nfds; ++s) {
            if (pfds[s].fd == NERO_INVALID_SOCKET || !pfds[s].revents) continue;
            int error = 0;
            socklen_t len = sizeof(error);
            port_state state;
            if (pfds[s].revents & POLLNVAL) {
                error = EBADF;
                state = PORT_UNKNOWN;
            } else if (getsockopt(pfds[s].fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0) {
                error = errno;
                state = PORT_UNKNOWN;
            } else if (connect_pending(error)) {
                continue; /* readiness is not itself successful connection evidence */
            } else if (!error && (!(pfds[s].revents & POLLOUT) ||
                                 (pfds[s].revents & (POLLERR | POLLHUP)))) {
                state = PORT_UNKNOWN;
            } else state = classify_connect_error(error);
            close(pfds[s].fd);
            pfds[s].fd = NERO_INVALID_SOCKET;
            record_result(slot_port[s], state, error);
            remaining--;
        }
        now = scan_now_ms();
        for (int s = 0; s < nfds; ++s) {
            if (pfds[s].fd == NERO_INVALID_SOCKET || deadline[s] > now) continue;
            close(pfds[s].fd);
            pfds[s].fd = NERO_INVALID_SOCKET;
            record_result(slot_port[s], PORT_FILTERED, ETIMEDOUT);
            remaining--;
        }
        if (ready > 0 && remaining == before) {
            const struct timespec delay = {0, 1000000L};
            nanosleep(&delay, NULL); /* spurious/pending readiness must not spin */
        }
    }
    /* Cancellation leaves unfinished slots unscanned, or retains their first
     * phase outcome. No cancelled probe is reported as a completed attempt. */
    for (int s = 0; s < nfds; ++s)
        if (pfds[s].fd != NERO_INVALID_SOCKET) close(pfds[s].fd);
    if (resource_error && !scan_cancelled()) {
        const struct timespec delay = {0, 5000000L};
        nanosleep(&delay, NULL); /* bounded backoff, not a busy socket retry loop */
    }
}

static void *scan_worker(void *arg) {
    scan_work *work = arg;
    int ports[MAX_BATCH];
    while (!scan_cancelled()) {
        int base = atomic_fetch_add_explicit(&work->next, work->batch, memory_order_relaxed);
        if (base >= work->count) break;
        int count = work->count - base;
        if (count > work->batch) count = work->batch;
        for (int i = 0; i < count; ++i)
            ports[i] = work->ports ? work->ports[base + i] : base + i + 1;
        scan_ports_batch(target_ip_global, ports, count, work->timeout_ms);
    }
    return NULL;
}

static void run_scan_phase(const int *ports, int count, int threads, int batch, int timeout_ms) {
    int budget = scan_socket_budget();
    if (threads < 1) threads = 1;
    if (threads > MAX_THREADS) threads = MAX_THREADS;
    if (threads > budget) threads = budget;
    if (batch < 1) batch = 1;
    if (batch > MAX_BATCH) batch = MAX_BATCH;
    if (batch > budget / threads) batch = budget / threads;
    scan_work work = { .ports = ports, .count = count,
                       .timeout_ms = timeout_ms, .batch = batch };
    atomic_init(&work.next, 0);
    pthread_t tids[MAX_THREADS];
    int created = 0;
    /* Caller is also a worker, so partial/all pthread_create failure cannot
     * lose a statically assigned range. Total scanner workers <= threads. */
    for (int i = 1; i < threads; ++i) {
        if (pthread_create(&tids[created], NULL, scan_worker, &work) != 0) break;
        created++;
    }
    scan_worker(&work);
    for (int i = 0; i < created; ++i) pthread_join(tids[i], NULL);
}

static int collect_retry_candidates(int *ports) {
    int count = 0;
    for (int port = 1; port <= SCAN_TOTAL_PORTS; ++port)
        if (results_list[port].state == PORT_FILTERED || results_list[port].state == PORT_UNKNOWN)
            ports[count++] = port;
    return count;
}

static void run_deep_scan(int *ports, int count, const scan_profile *profile) {
    int window = profile->deep_max_candidates;
    if (window < 1 || window > SCAN_TOTAL_PORTS) window = SCAN_TOTAL_PORTS;
    for (int start = 0; start < count && !scan_cancelled(); start += window) {
        int chunk = count - start;
        if (chunk > window) chunk = window;
        /* Exactly one bounded retry, with a smaller global socket budget from
         * the deep profile and a longer (at most 5000 ms) timeout. */
        int timeout = profile->deep_timeout_ms;
        if (timeout < profile->fast_timeout_ms) timeout = profile->fast_timeout_ms;
        if (timeout > 5000) timeout = 5000;
        int threads = profile->deep_threads;
        if (threads < 1) threads = 1;
        if (threads > MAX_THREADS) threads = MAX_THREADS;
        int batch = profile->deep_batch;
        if (batch > (MAX_SCAN_SOCKETS / 2) / threads)
            batch = (MAX_SCAN_SOCKETS / 2) / threads;
        run_scan_phase(ports + start, chunk, threads, batch, timeout);
    }
}

/* --- Banner / sürüm yakalama ---
 * Açık bir porta bağlanıp gelen ilk satırı (banner) okur. SSH/FTP/SMTP/POP3/
 * IMAP gibi servisler bağlantıda kendiliğinden banner yollar; HTTP portlarına
 * küçük bir istek gönderilir. Yazdırılamayan baytlar '.' ile değiştirilir ve
 * yalnızca ilk satır, kısaltılarak saklanır. */
#define BANNER_MAX 72

/* Banner için küçük bir HTTP isteği gönderilecek yaygın düz-metin HTTP portları. */
static int is_http_port(int port) {
    return port == 80 || port == 8080 || port == 8000 || port == 8888 ||
           port == 9000 || port == 591 || port == 8008;
}

/* Nonblocking banner I/O shares the scanner's bounded cancellation latency. */
static int banner_wait(nero_socket_t fd, short events, int64_t deadline) {
    while (!scan_cancelled()) {
        int64_t left = deadline - scan_now_ms();
        if (left <= 0) return 0;
        struct pollfd p = {fd, events, 0};
        int result = nero_socket_poll(&p, 1, left < CANCEL_POLL_MS ? (int)left : CANCEL_POLL_MS);
        if (result > 0) return (p.revents & events) && !(p.revents & POLLNVAL);
        if (result < 0 && errno != EINTR
#ifdef _WIN32
            && errno != WSAEINTR
#endif
           ) return 0;
    }
    return 0;
}

static void grab_banner(const char *ip, int port, char *out, size_t outsz) {
    if (!outsz) return;
    out[0] = '\0';
    if (scan_cancelled()) return;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
#if defined(__APPLE__)
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return;
    nero_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == NERO_INVALID_SOCKET) return;
    if (make_nonblocking(fd) != 0) { close(fd); return; }
    int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0) {
        int error = errno;
        if (!connect_pending(error) || !banner_wait(fd, POLLOUT, scan_now_ms() + 1200)) {
            close(fd); return;
        }
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error) {
            close(fd); return;
        }
    }
    if (is_http_port(port) && !scan_cancelled()) {
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
    if (banner_wait(fd, POLLIN, scan_now_ms() + 1200) && !scan_cancelled())
        n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0) return;
    size_t o = 0;
    for (ssize_t i = 0; i < n && o < outsz - 1; ++i) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r' || c == '\n') break;
        out[o++] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    out[o] = '\0';
}

typedef struct {
    const char *ip;
    const int *ports;
    int start, end;
    char (*store)[BANNER_MAX];
} banner_args;

/* Banner yakalamayı paralelleştiren iş parçacığı: kendine düşen port aralığını
 * işler ve yalnızca kendi store[] satırlarına yazdığından kilit gerekmez. */
static void *banner_worker(void *p) {
    banner_args *b = p;
    for (int i = b->start; i < b->end; ++i) {
        if (portscanner_interrupt_requested ||
            atomic_load_explicit(&scan_cancel_requested, memory_order_relaxed))
            break;
        grab_banner(b->ip, b->ports[i], b->store[i], BANNER_MAX);
    }
    return NULL;
}

/* Açık portlar için banner'ları paralel yakalar (çok iş parçacığı, her biri
 * kendi dizin aralığına yazar; kilit gerekmez). En çok `cap` port denenir. */
#define BANNER_THREADS 12
#define BANNER_PORT_CAP 128
static void grab_banners(const char *ip, const int *ports, int count,
                         char (*store)[BANNER_MAX]) {
    if (count <= 0) return;
    int threads = count < BANNER_THREADS ? count : BANNER_THREADS;
    pthread_t tids[BANNER_THREADS];
    banner_args args[BANNER_THREADS];
    int created = 0;
    for (int i = 0; i < threads; ++i) {
        args[i].ip = ip;
        args[i].ports = ports;
        args[i].store = store;
        args[i].start = (i * count) / threads;
        args[i].end   = ((i + 1) * count) / threads;
        if (pthread_create(&tids[i], NULL, banner_worker, &args[i]) != 0) {
            fprintf(stderr, "[!] Optional banner checks incomplete (thread creation failed).\n");
            break;
        }
        created++;
    }
    for (int i = 0; i < created; ++i) pthread_join(tids[i], NULL);
}

/* Optional ICMP discovery is advisory and opt-in. Only numeric, validated
 * IPv4 addresses are passed as an argv element (never through a shell). Lack
 * of ping permissions, binary, response or route always means unknown. */
static host_state host_probe_alive(const char *ip) {
    struct in_addr parsed;
    if (!ip || inet_pton(AF_INET, ip, &parsed) != 1) return HOST_UNKNOWN;
    const char *icmp = getenv("NERO_SCAN_ICMP");
    if (!icmp || strcmp(icmp, "1") != 0 || scan_cancelled()) return HOST_UNKNOWN;
#ifdef _WIN32
    char command[128];
    snprintf(command, sizeof(command), "ping.exe -n 1 -w 1000 %s", ip);
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) return HOST_UNKNOWN;
    int64_t deadline = scan_now_ms() + 1700;
    DWORD code = 1;
    while (WaitForSingleObject(process.hProcess, CANCEL_POLL_MS) == WAIT_TIMEOUT) {
        if (scan_cancelled() || scan_now_ms() >= deadline) {
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
    if (posix_spawn_file_actions_init(&actions) != 0) return HOST_UNKNOWN;
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
    if (spawned != 0) return HOST_UNKNOWN;
    int64_t deadline = scan_now_ms() + 1700;
    int status = 0;
    for (;;) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? HOST_ONLINE : HOST_UNKNOWN;
        if (result < 0 && errno != EINTR) return HOST_UNKNOWN;
        if (scan_cancelled() || scan_now_ms() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
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
static int adjust_manual_profile(nero_operation_ui *ui) {
    int threads = manual_profile.fast_threads;
    int batch   = manual_profile.fast_batch;
    int timeout = manual_profile.fast_timeout_ms;
    int row = 0;
    ui->controls = "Up/Down: Satir   Left/Right: Degistir   Enter: Basla   Esc: Geri";

    for (;;) {
        char v0[24], v1[24], v2[24];
        snprintf(v0, sizeof v0, "%d", threads);
        snprintf(v1, sizeof v1, "%d", batch);
        snprintf(v2, sizeof v2, "%d ms", timeout);
        const char *labels[3] = { "Is parcacigi", "Parti basina soket", "Zaman asimi" };
        const char *values[3] = { v0, v1, v2 };
        nero_operation_menu(ui, "Manuel hiz ayari", labels, values, 3, row);
        if (portscanner_interrupt_requested) return 0;

        nero_ui_key k = nero_operation_read_key(100);
        if (k == NERO_UI_KEY_CANCEL || k == NERO_UI_KEY_EOF) return 0;
        if (k == NERO_UI_KEY_ENTER) break;
        if (k == NERO_UI_KEY_UP || k == NERO_UI_KEY_WHEEL_UP) { if (row > 0) row--; }
        else if (k == NERO_UI_KEY_DOWN || k == NERO_UI_KEY_WHEEL_DOWN) { if (row < 2) row++; }
        else if (k == NERO_UI_KEY_LEFT || k == NERO_UI_KEY_RIGHT) {
            int dir = (k == NERO_UI_KEY_RIGHT) ? 1 : -1;
            if (row == 0) {
                threads += dir;
                if (threads < 1) threads = 1;
                if (threads > MAX_THREADS) threads = MAX_THREADS;
            } else if (row == 1) {
                batch += dir * 16;
                if (batch < 16) batch = 16;
                if (batch > MAX_BATCH) batch = MAX_BATCH;
            } else {
                timeout += dir * 100;
                if (timeout < 200) timeout = 200;
                if (timeout > 5000) timeout = 5000;
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
    return 1;
}

/* --- Tarama hızı seçim menüsü ---
 * İnteraktif değilse menü göstermeden önerilen (Orta) profili kullanır.
 * Kullanıcı iptal ederse NULL döndürür. */
static const scan_profile *select_scan_profile(nero_operation_ui *ui) {
    if (!ui->interactive) return &SCAN_PROFILES[PROFILE_MEDIUM];

    int raw = enable_raw_mode() == 0;
    if (raw) set_mouse_reporting(1);

    const char *labels[PROFILE_COUNT] = {
        "Hizli", "Orta  (onerilen)", "Yavas", "Manuel ayar"
    };
    char vbuf[PROFILE_COUNT][40];
    const char *values[PROFILE_COUNT];
    for (int i = 0; i < 3; ++i) {
        snprintf(vbuf[i], sizeof vbuf[i], "%dx%d soket, %d ms",
                 SCAN_PROFILES[i].fast_threads, SCAN_PROFILES[i].fast_batch,
                 SCAN_PROFILES[i].fast_timeout_ms);
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
        if (portscanner_interrupt_requested) { chosen = NULL; break; }

        nero_ui_key k = nero_operation_read_key(100);
        if (k == NERO_UI_KEY_CANCEL || k == NERO_UI_KEY_EOF) { chosen = NULL; break; }
        if (k == NERO_UI_KEY_UP || k == NERO_UI_KEY_WHEEL_UP) { if (sel > 0) sel--; }
        else if (k == NERO_UI_KEY_DOWN || k == NERO_UI_KEY_WHEEL_DOWN) { if (sel < PROFILE_COUNT - 1) sel++; }
        else if (k == NERO_UI_KEY_ENTER) {
            if (sel == PROFILE_MANUAL) {
                if (adjust_manual_profile(ui)) { chosen = &manual_profile; break; }
                if (portscanner_interrupt_requested) { chosen = NULL; break; }
                ui->controls = "Up/Down: Sec   Enter: Onayla   Esc: Iptal";
            } else {
                chosen = &SCAN_PROFILES[sel];
                break;
            }
        }
    }

    if (raw) { set_mouse_reporting(0); disable_raw_mode(); }
    return chosen;
}

/* --- Ana kontrolcü --- */
int portscanner(const char *target_ip) {
    struct in_addr address;
    if (!target_ip || inet_pton(AF_INET, target_ip, &address) != 1) {
        fprintf(stderr, "[-] Invalid IPv4 target.\n");
        return PORTSCANNER_FAILED;
    }
    target_ip_global = target_ip;
    scan_input_fallback = 0;
    scan_owner = pthread_self();
    portscanner_interrupt_requested = 0;
    atomic_init(&scan_cancel_requested, 0);
    atomic_init(&latest_probe_port, 0);
    atomic_init(&ports_scanned, 0);
    atomic_init(&host_responded, 0);
    results_list = calloc(SCAN_TOTAL_PORTS + 1, sizeof(*results_list));
    int *work_ports = malloc(SCAN_TOTAL_PORTS * sizeof(*work_ports));
    if (!results_list || !work_ports) {
        free(results_list); results_list = NULL;
        free(work_ports);
        fprintf(stderr, "[-] Failed to allocate scan ledger.\n");
        return PORTSCANNER_FAILED;
    }
    if (pthread_mutex_init(&results_mutex, NULL) != 0) {
        free(results_list); results_list = NULL;
        free(work_ports);
        return PORTSCANNER_FAILED;
    }

    nero_operation_ui ui = {0};
    char detail[96];
    snprintf(detail, sizeof(detail), "%s", target_ip);
    nero_operation_begin(&ui, "Port scan", detail, "Esc · Back");
    active_profile = select_scan_profile(&ui);
    if (active_profile == NULL) {
        pthread_mutex_destroy(&results_mutex);
        free(results_list); results_list = NULL;
        free(work_ports);
        nero_operation_end(&ui);
        puts("Scan cancelled. Returned to terminal.");
        return PORTSCANNER_CANCELLED;
    }

    /* Discovery never gates a valid target. The full fast phase supplies TCP
     * reachability evidence itself, without an extra 16-port pre-scan. */
    if (host_probe_alive(target_ip) == HOST_ONLINE)
        atomic_store_explicit(&host_responded, 1, memory_order_relaxed);
    nero_operation_status("36", "Host discovery",
        atomic_load_explicit(&host_responded, memory_order_relaxed)
            ? "online; scanning all TCP ports" : "unknown; scanning all TCP ports");
    const scan_profile *prof = active_profile;
    scan_animation_state animation = { .target_ip = target_ip, .animate = scan_motion_enabled() };
    atomic_init(&animation.stop, 0);
    pthread_t animation_thread;
    int animation_started = 0, discovery_raw = 0;
    if (isatty(STDIN_FILENO)) {
        discovery_raw = (enable_raw_mode() == 0);
        if (discovery_raw) set_mouse_reporting(1);
        animation_started = pthread_create(&animation_thread, NULL, scan_animation_worker, &animation) == 0;
    }
    scan_input_fallback = discovery_raw && !animation_started;
    if (!animation_started) nero_operation_loader("Scanning", target_ip, 6);
    run_scan_phase(NULL, SCAN_TOTAL_PORTS, prof->fast_threads,
                   prof->fast_batch, prof->fast_timeout_ms);
    int candidate_count = collect_retry_candidates(work_ports);
    if (!scan_cancelled()) run_deep_scan(work_ports, candidate_count, prof);

    if (animation_started) {
        atomic_store_explicit(&animation.stop, 1, memory_order_relaxed);
        pthread_join(animation_thread, NULL);
    }
    if (discovery_raw) { set_mouse_reporting(0); disable_raw_mode(); }
    scan_input_fallback = 0;
    pthread_mutex_destroy(&results_mutex);
    scan_summary summary = summarize_scan();
    int cancelled = scan_cancelled();
    int accounted = summary.open + summary.closed + summary.filtered + summary.unknown;
    if (cancelled || accounted != SCAN_TOTAL_PORTS || summary.retried < candidate_count) {
        nero_operation_end(&ui);
        report_scan_summary();
        if (summary.retried < candidate_count)
            printf("Retry checks incomplete: %d of %d candidates.\n", summary.retried, candidate_count);
        free(results_list); results_list = NULL;
        free(work_ports);
        puts(cancelled ? "Scan cancelled. Returned to terminal." : "[-] Scan coverage incomplete.");
        return cancelled ? PORTSCANNER_CANCELLED : PORTSCANNER_FAILED;
    }

    /* Reuse the work array for the unchanged open-port selection screen. */
    int *open_ports = work_ports;
    int open_count = 0;
    for (int port = 1; port <= SCAN_TOTAL_PORTS; ++port)
        if (results_list[port].state == PORT_OPEN) open_ports[open_count++] = port;
    char result_detail[160];
    snprintf(result_detail, sizeof(result_detail),
        "%d open, %d closed, %d filtered, %d error/unknown",
        summary.open, summary.closed, summary.filtered, summary.unknown);
    nero_operation_status("32", "Scan complete", result_detail);

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
            grab_banners(target_ip, banner_port_list, banner_n, banner_store);
            for (int i = 0; i < banner_n; ++i) banner_texts[i] = banner_store[i];
            ui.banner_ports = banner_port_list;
            ui.banner_texts = (const char *const *)banner_texts;
            ui.banner_count = banner_n;
            nero_operation_status("32", "Scan complete", result_detail);
        } else {
            free(banner_port_list); free(banner_store); free(banner_texts);
            banner_port_list = NULL; banner_store = NULL; banner_texts = NULL;
        }
    }

    int selected_port = scan_cancelled() ? PORTSCANNER_CANCELLED :
        select_port_menu_in_ui(open_ports, open_count, &ui);
    free(banner_port_list);
    free(banner_store);
    free(banner_texts);
    free(open_ports);
    nero_operation_end(&ui);
    report_scan_summary();
    if (summary.filtered || summary.unknown)
        puts("Some port checks remain inconclusive after one bounded retry.");
    free(results_list);
    results_list = NULL;
    if (selected_port == PORTSCANNER_CANCELLED)
        puts("Port selection cancelled. Returned to terminal.");
    return selected_port == 0 && summary.unknown > 0 ? PORTSCANNER_FAILED : selected_port;
}
