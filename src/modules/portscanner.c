/* Concurrent TCP discovery with iOS-focused adaptive scanning.
 * Fast phase and deep phase use event-driven select() batching:
 * each worker thread manages BATCH_SIZE sockets concurrently,
 * so 8 threads * 400 sockets = 3200 simultaneous probes without
 * spawning thousands of threads or hitting macOS ulimit -n. */
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

/* Bir tarama hız profili. Daha düşük eşzamanlılık = hedefi/WiFi'yi daha az
 * boğar = daha kararlı; daha kısa zaman aşımı = daha hızlı bitiş. */
typedef struct {
    const char *name;
    int fast_threads, fast_batch, fast_timeout_ms;
    int deep_threads, deep_batch, deep_timeout_ms;
    int deep_max_candidates; /* bu kadar filtrelenmiş porttan fazlası varsa derin taramayı atla */
} scan_profile;

enum { PROFILE_FAST = 0, PROFILE_MEDIUM, PROFILE_SLOW, PROFILE_MANUAL, PROFILE_COUNT };

/* name alanları hız menüsünde de gösterilir. */
static const scan_profile SCAN_PROFILES[] = {
    /* Hızlı: kısa zaman aşımı, yüksek eşzamanlılık. Birkaç saniyede biter;
     * yoğun WiFi'de çok seyrek portları kaçırabilir. */
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

typedef struct {
    int port;
    int filtered;
} port_result;

typedef struct {
    int start_port;
    int end_port;
    int timeout_ms;
    int batch;
} batch_thread_args;

typedef struct {
    int start_idx;
    int end_idx;
    int timeout_ms;
    int batch;
} deep_batch_args;

static struct termios orig_termios;
static port_result *results_list = NULL;
static int result_count = 0;
static int *candidates = NULL;
static int candidate_count = 0;
static int scan_alloc_failed = 0;
volatile sig_atomic_t portscanner_interrupt_requested = 0;
static atomic_int scan_cancel_requested;
static atomic_int ports_scanned;
static atomic_int latest_probe_port;
static const char *target_ip_global;
static pthread_mutex_t results_mutex;

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
        for (int i = 0; i < result_count; ++i) {
            if (!results_list[i].filtered) {
                if (shown < DISCOVERY_VIEW_CAP)
                    ports[shown++] = results_list[i].port;
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

/* Bir batch taraması aynı anda binlerce soket açabilir. Varsayılan
 * RLIMIT_NOFILE (macOS'ta sıklıkla 256) buna yetmez ve socket() EMFILE ile
 * başarısız olur. Soft limiti, hard limiti aşmadan ihtiyaç kadar yükselt.
 * Windows'ta RLIMIT_NOFILE yoktur; soket sınırı sistem belleğine bağlıdır. */
#ifndef _WIN32
static void raise_fd_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    rlim_t needed = (rlim_t)MAX_THREADS * MAX_BATCH + 64;
    if (rl.rlim_cur >= needed) return;
    rlim_t target = needed;
    if (rl.rlim_max != RLIM_INFINITY && target > rl.rlim_max)
        target = rl.rlim_max;
    rl.rlim_cur = target;
    setrlimit(RLIMIT_NOFILE, &rl);
}
#else
static void raise_fd_limit(void) { /* no per-process fd limit on Windows */ }
#endif

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

/* --- Idempotent sonuç ekleme --- */
static void add_open_result(int port) {
    pthread_mutex_lock(&results_mutex);
    for (int i = 0; i < result_count; ++i) {
        if (results_list[i].port == port) {
            results_list[i].filtered = 0;
            pthread_mutex_unlock(&results_mutex);
            return;
        }
    }
    port_result *tmp = realloc(results_list, (result_count + 1) * sizeof(port_result));
    if (!tmp) {
        scan_alloc_failed = 1;
        pthread_mutex_unlock(&results_mutex);
        return;
    }
    results_list = tmp;
    results_list[result_count].port = port;
    results_list[result_count].filtered = 0;
    result_count++;
    pthread_mutex_unlock(&results_mutex);
}

/* Yanıt vermeyen (filtrelenmiş olabilecek) bir portu, derin taramada yeniden
 * denenmek üzere aday olarak kaydeder. Aynı port zaten varsa tekrar eklenmez. */
static void add_filtered_candidate(int port) {
    pthread_mutex_lock(&results_mutex);
    for (int i = 0; i < result_count; ++i) {
        if (results_list[i].port == port) {
            pthread_mutex_unlock(&results_mutex);
            return;
        }
    }
    port_result *tmp = realloc(results_list, (result_count + 1) * sizeof(port_result));
    if (!tmp) {
        scan_alloc_failed = 1;
        pthread_mutex_unlock(&results_mutex);
        return;
    }
    results_list = tmp;
    results_list[result_count].port = port;
    results_list[result_count].filtered = 1;
    result_count++;
    pthread_mutex_unlock(&results_mutex);
}

/* --- Batch select ile eşzamanlı port tarama ---
 *
 * Her çağrıda `ports[]` dizisindeki portlar için bir soket açar,
 * hepsine non-blocking connect() gönderir, sonra poll() ile yanıtları
 * bekler. Bu sayede aynı anda yüzlerce port probe edilir; tek tek
 * beklemek yerine toplu bekleriz. */
static void scan_ports_batch(
    const char *ip,
    const int *ports,
    int count,
    int timeout_ms
) {
    if (count > MAX_BATCH) count = MAX_BATCH;
    if (count <= 0) return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
#if defined(__APPLE__)
    addr.sin_len = sizeof(addr);
#endif
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return;

    struct pollfd pfds[MAX_BATCH];
    int slot_port[MAX_BATCH];
    int nfds = 0;

    /* 1. Soketleri aç ve non-blocking connect() başlat. */
    for (int i = 0; i < count; ++i) {
        nero_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == NERO_INVALID_SOCKET) {
            /* Tanımlayıcı tükendi: portu sessizce "kapalı" saymak yerine
             * belirsiz (filtered) olarak işaretle; derin tarama yeniden dener. */
            if (errno == EMFILE || errno == ENFILE)
                add_filtered_candidate(ports[i]);
            continue;
        }

#ifdef _WIN32
        u_long nonblocking = 1;
        if (ioctlsocket((SOCKET)fd, FIONBIO, &nonblocking) != 0) {
            close(fd);
            continue;
        }
#else
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            continue;
        }
#endif

        addr.sin_port = htons((uint16_t)ports[i]);
        atomic_store_explicit(&latest_probe_port, ports[i], memory_order_relaxed);

        int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (ret == 0) {
            /* Anında açık */
            add_open_result(ports[i]);
            close(fd);
            continue;
        }
        if (ret < 0 && errno == ECONNREFUSED) {
            /* Anında kapalı — listeye ekleme */
            close(fd);
            continue;
        }
        if (ret < 0 && errno != EINPROGRESS) {
            close(fd);
            continue;
        }

        pfds[nfds].fd = fd;
        pfds[nfds].events = POLLOUT;
        pfds[nfds].revents = 0;
        slot_port[nfds] = ports[i];
        nfds++;
    }

    if (nfds == 0) return;

    /* 2. Bekleyen connect'leri son-tarihe kadar poll() ile çöz. poll()'un
     * FD_SETSIZE sınırı yoktur; binlerce eşzamanlı tanımlayıcı güvenlidir —
     * select()'te değeri FD_SETSIZE'ı aşan bir tanımlayıcı fd_set'i bozar.
     *
     * ÖNEMLİ: Tek bir poll() geçişiyle yetinilemez. Kapalı portlar connect
     * sonrası neredeyse anında POLLERR verir; poll onlar hazır olur olmaz
     * döner. O an hâlâ bağlanmakta olan açık bir portun revents'i 0 olabilir.
     * Tek geçiş bunu yanlışlıkla "zaman aşımı" sayıp açık portu kaçırırdı.
     * Bu yüzden çözülen soketleri (fd = -1 ile) poll'dan çıkarıp kalanları
     * gerçek zaman aşımına kadar yeniden bekleriz. */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int remaining = nfds;

    while (remaining > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L
                        + (now.tv_nsec - start.tv_nsec) / 1000000L;
        int wait_ms = timeout_ms - (int)elapsed_ms;
        if (wait_ms <= 0) break;

        int ready;
        do {
            ready = nero_socket_poll(pfds, (nfds_t)nfds, wait_ms);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0) break; /* hata veya zaman aşımı → kalanlar filtered */

        for (int s = 0; s < nfds && ready > 0; ++s) {
            if (pfds[s].fd == NERO_INVALID_SOCKET || pfds[s].revents == 0) continue;
            ready--;
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(pfds[s].fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0) {
                if (so_error == 0)
                    add_open_result(slot_port[s]);
                else if (so_error != ECONNREFUSED)
                    /* ETIMEDOUT, EHOSTUNREACH vb. → filtrelenmiş aday */
                    add_filtered_candidate(slot_port[s]);
                /* ECONNREFUSED → kapalı → listeye ekleme */
            } else {
                add_filtered_candidate(slot_port[s]);
            }
            close(pfds[s].fd);
            pfds[s].fd = NERO_INVALID_SOCKET; /* poll bundan sonra yok sayar (revents=0) */
            remaining--;
        }
    }

    /* 3. Zaman aşımına kadar yanıt vermeyen soketler → sessiz → filtered. */
    for (int s = 0; s < nfds; ++s) {
        if (pfds[s].fd == NERO_INVALID_SOCKET) continue;
        add_filtered_candidate(slot_port[s]);
        close(pfds[s].fd);
    }
}

/* --- Fast batch thread --- */
void *fast_scan_thread(void *args) {
    batch_thread_args *t = args;
    int ports[MAX_BATCH];
    int batch = t->batch > 0 && t->batch <= MAX_BATCH ? t->batch : MAX_BATCH;

    for (int base = t->start_port; base <= t->end_port; base += batch) {
        if (portscanner_interrupt_requested ||
            atomic_load_explicit(&scan_cancel_requested, memory_order_relaxed))
            break;

        int count = 0;
        int end = base + batch - 1;
        if (end > t->end_port) end = t->end_port;

        for (int port = base; port <= end; ++port)
            ports[count++] = port;

        scan_ports_batch(target_ip_global, ports, count, t->timeout_ms);
        atomic_fetch_add_explicit(&ports_scanned, count, memory_order_relaxed);
    }
    pthread_exit(NULL);
    return NULL; /* MinGW pthread_exit declarations may lack noreturn. */
}

/* --- Deep batch thread (yalnızca candidates üzerinde) --- */
void *deep_scan_thread(void *args) {
    deep_batch_args *t = args;
    int ports[MAX_BATCH];
    int batch = t->batch > 0 && t->batch <= MAX_BATCH ? t->batch : MAX_BATCH;

    for (int base = t->start_idx; base < t->end_idx; base += batch) {
        if (portscanner_interrupt_requested ||
            atomic_load_explicit(&scan_cancel_requested, memory_order_relaxed))
            break;

        int end = base + batch;
        if (end > t->end_idx) end = t->end_idx;

        int count = 0;
        for (int i = base; i < end && i < candidate_count; ++i)
            ports[count++] = candidates[i];

        if (count == 0) continue;

        scan_ports_batch(target_ip_global, ports, count, t->timeout_ms);
        atomic_fetch_add_explicit(&ports_scanned, count, memory_order_relaxed);
    }
    pthread_exit(NULL);
    return NULL; /* MinGW pthread_exit declarations may lack noreturn. */
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

/* Tek bir açık porttan ilk banner satırını `out`'a (en çok outsz-1 bayt) yazar;
 * yanıt yoksa boş bırakır. Bağlantı non-blocking connect+poll ile, okuma ise
 * SO_RCVTIMEO zaman aşımıyla yapılır. */
static void grab_banner(const char *ip, int port, char *out, size_t outsz) {
    if (outsz == 0) return;
    out[0] = '\0';

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
#ifdef _WIN32
    u_long nonblocking = 1, blocking = 0;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &nonblocking) != 0) { close(fd); return; }
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif

    int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0 && errno == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        int pr;
        do { pr = nero_socket_poll(&p, 1, 1200); } while (pr < 0 && errno == EINTR);
        if (pr <= 0) { close(fd); return; }
        int se = 0; socklen_t l = sizeof(se);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &se, &l) != 0 || se != 0) { close(fd); return; }
    } else if (r < 0) {
        close(fd);
        return;
    }

    /* Bloklamaya dön ve bir okuma zaman aşımı koy. */
#ifdef _WIN32
    if (ioctlsocket((SOCKET)fd, FIONBIO, &blocking) != 0) { close(fd); return; }
    DWORD tv = 1200; /* milisaniye: 1.2 sn */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, sizeof(tv));
#else
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval tv = { 1, 200000 }; /* 1.2 sn */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    if (is_http_port(port)) {
        const char *req = "HEAD / HTTP/1.0\r\n\r\n";
        (void)send(fd, req, strlen(req), 0);
    }

    char buf[256];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0) return;

    size_t o = 0;
    for (ssize_t i = 0; i < n && o < outsz - 1; ++i) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r' || c == '\n') break;         /* yalnızca ilk satır */
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
        if (pthread_create(&tids[i], NULL, banner_worker, &args[i]) != 0) break;
        created++;
    }
    for (int i = 0; i < created; ++i) pthread_join(tids[i], NULL);
}

/* --- Hızlı erişilebilirlik ön kontrolü ---
 * Tam 65535 portu taramadan önce, hostun orada olup olmadığını birkaç yüz
 * ms'de anlar: yaygın portlara eşzamanlı SYN gönderir. Herhangi bir porttan
 * SYN-ACK (açık) VEYA RST (kapalı) gelmesi hostun var olduğunu kanıtlar;
 * hepsi sessiz kalırsa host muhtemelen kapalı/erişilemez. 1: canlı, 0: sessiz. */
static int host_probe_alive(const char *ip) {
    static const int probe_ports[] = {
        80, 443, 22, 53, 445, 139, 8080, 3389, 21, 23, 25, 111, 135, 993, 62078, 548
    };
    const int n = (int)(sizeof(probe_ports) / sizeof(probe_ports[0]));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
#if defined(__APPLE__)
    addr.sin_len = sizeof(addr);
#endif
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return 0;

    struct pollfd pfds[32];
    int nfds = 0;
    for (int i = 0; i < n; ++i) {
        nero_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == NERO_INVALID_SOCKET) continue;
#ifdef _WIN32
        u_long nonblocking = 1;
        if (ioctlsocket((SOCKET)fd, FIONBIO, &nonblocking) != 0) { close(fd); continue; }
#else
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { close(fd); continue; }
#endif
        addr.sin_port = htons((uint16_t)probe_ports[i]);
        int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (r == 0) { close(fd); return 1; }                       /* anında açık */
        if (r < 0 && errno == ECONNREFUSED) { close(fd); return 1; } /* RST → canlı */
        if (r < 0 && errno != EINPROGRESS) { close(fd); continue; }
        pfds[nfds].fd = fd;
        pfds[nfds].events = POLLOUT;
        pfds[nfds].revents = 0;
        nfds++;
    }

    int alive = 0;
    if (nfds > 0) {
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        int remaining = nfds;
        while (remaining > 0 && !alive) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long el = (now.tv_sec - start.tv_sec) * 1000L
                    + (now.tv_nsec - start.tv_nsec) / 1000000L;
            int wait = 700 - (int)el; /* sabit, kısa canlılık penceresi */
            if (wait <= 0) break;
            int ready;
            do { ready = nero_socket_poll(pfds, (nfds_t)nfds, wait); } while (ready < 0 && errno == EINTR);
            if (ready <= 0) break;
            for (int s = 0; s < nfds && ready > 0; ++s) {
                if (pfds[s].fd == NERO_INVALID_SOCKET || pfds[s].revents == 0) continue;
                ready--;
                int se = 0;
                socklen_t l = sizeof(se);
                if (getsockopt(pfds[s].fd, SOL_SOCKET, SO_ERROR, &se, &l) == 0) {
                    if (se == 0 || se == ECONNREFUSED) alive = 1; /* açık veya kapalı → host var */
                }
                close(pfds[s].fd);
                pfds[s].fd = NERO_INVALID_SOCKET;
                remaining--;
            }
        }
    }
    for (int s = 0; s < nfds; ++s)
        if (pfds[s].fd != NERO_INVALID_SOCKET) close(pfds[s].fd);
    return alive;
}

/* Host sessizse: interaktifte "yine de taransın mı?" sorar (varsayılan Hayır).
 * 1 döndürürse taramaya devam, 0 döndürürse vazgeç. Non-interaktif ortamda
 * (betik/boru) hiçbir hostu atlamamak için devam eder (1). */
static int confirm_scan_unreachable(nero_operation_ui *ui) {
    if (!ui->interactive) return 1;
    int raw = enable_raw_mode() == 0;
    if (raw) set_mouse_reporting(1);
    const char *labels[2] = { "Hayir  (geri don)", "Evet  (yine de tara)" };
    int sel = 0, res = 0;
    ui->controls = "Up/Down: Sec   Enter: Onayla   Esc: Iptal";
    for (;;) {
        nero_operation_menu(ui, "Host yanit vermiyor — yine de taransin mi?",
                            labels, NULL, 2, sel);
        if (portscanner_interrupt_requested) { res = 0; break; }
        nero_ui_key k = nero_operation_read_key(100);
        if (k == NERO_UI_KEY_CANCEL || k == NERO_UI_KEY_EOF) { res = 0; break; }
        if (k == NERO_UI_KEY_UP || k == NERO_UI_KEY_WHEEL_UP) { if (sel > 0) sel--; }
        else if (k == NERO_UI_KEY_DOWN || k == NERO_UI_KEY_WHEEL_DOWN) { if (sel < 1) sel++; }
        else if (k == NERO_UI_KEY_ENTER) { res = sel; break; }
    }
    if (raw) { set_mouse_reporting(0); disable_raw_mode(); }
    return res;
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
    target_ip_global = target_ip;
    results_list = NULL;
    result_count = 0;
    candidates = NULL;
    candidate_count = 0;
    scan_alloc_failed = 0;
    portscanner_interrupt_requested = 0;
    atomic_init(&scan_cancel_requested, 0);
    atomic_init(&latest_probe_port, 0);
    atomic_init(&ports_scanned, 0);

    if (pthread_mutex_init(&results_mutex, NULL) != 0) {
        fprintf(stderr, "[-] Failed to initialize mutex.\n");
        return -1;
    }

    raise_fd_limit();

    nero_operation_ui ui = {0};
    char detail[96];
    snprintf(detail, sizeof(detail), "%s", target_ip);
    nero_operation_begin(&ui, "Port scan", detail, "Esc · Back");

    /* Aşama 0: tarama hızını seç (Hızlı/Orta/Yavaş/Manuel). İnteraktif
     * değilse önerilen profil kullanılır; kullanıcı iptal ederse çıkılır. */
    active_profile = select_scan_profile(&ui);
    if (active_profile == NULL) {
        pthread_mutex_destroy(&results_mutex);
        free(results_list);
        results_list = NULL;
        result_count = 0;
        nero_operation_end(&ui);
        puts("Scan cancelled. Returned to terminal.");
        return PORTSCANNER_CANCELLED;
    }

    /* Aşama 0.5: hostun orada olup olmadığını hızlıca anla. Ulaşılamayan bir
     * hedefte 65535 portu tam zaman aşımıyla taramak dakikalarca sürebilir;
     * önce kısa bir canlılık kontrolü yapıp kullanıcıyı bekletme. */
    if (!host_probe_alive(target_ip)) {
        if (confirm_scan_unreachable(&ui) != 1) {
            pthread_mutex_destroy(&results_mutex);
            free(results_list);
            results_list = NULL;
            result_count = 0;
            nero_operation_end(&ui);
            printf("Host %s yanit vermiyor (erisilemiyor). Tarama yapilmadi.\n", target_ip);
            return 0;
        }
    }

    const scan_profile *prof = active_profile;
    int fast_thread_count = prof->fast_threads;
    if (fast_thread_count < 1) fast_thread_count = 1;
    if (fast_thread_count > MAX_THREADS) fast_thread_count = MAX_THREADS;

    /* Aşama 1: Event-driven batch hızlı keşif (tam 1-65535 aralığı) */
    pthread_t fast_threads[MAX_THREADS];
    batch_thread_args fast_args[MAX_THREADS];
    int ports_per_fast = SCAN_TOTAL_PORTS / fast_thread_count;
    if (ports_per_fast == 0) ports_per_fast = SCAN_TOTAL_PORTS;

    int fast_created = 0;
    for (int i = 0; i < fast_thread_count; ++i) {
        fast_args[i].start_port = (i * ports_per_fast) + 1;
        fast_args[i].end_port = (i == fast_thread_count - 1)
            ? SCAN_TOTAL_PORTS
            : (i + 1) * ports_per_fast;
        fast_args[i].timeout_ms = prof->fast_timeout_ms;
        fast_args[i].batch = prof->fast_batch;
        if (pthread_create(&fast_threads[i], NULL, fast_scan_thread, &fast_args[i]) != 0) {
            fprintf(stderr, "[-] Failed to create fast thread %d.\n", i);
            break;
        }
        fast_created++;
    }

    scan_animation_state animation = { .target_ip = target_ip, .animate = scan_motion_enabled() };
    atomic_init(&animation.stop, 0);
    pthread_t animation_thread;
    int animation_started = 0;
    int discovery_raw = 0;

    if (isatty(STDIN_FILENO)) {
        discovery_raw = (enable_raw_mode() == 0);
        if (discovery_raw) set_mouse_reporting(1);
        animation_started = pthread_create(&animation_thread, NULL, scan_animation_worker, &animation) == 0;
    }
    if (!animation_started) nero_operation_loader("Scanning", target_ip, 6);

    for (int i = 0; i < fast_created; ++i) pthread_join(fast_threads[i], NULL);

    /* İptal edildiyse önce animasyon/girdi thread'ini durdur ki temizlik
     * sırasında serbest bırakılan belleğe erişmesin. */
    if (portscanner_interrupt_requested || atomic_load_explicit(&scan_cancel_requested, memory_order_relaxed)) {
        if (animation_started) {
            atomic_store_explicit(&animation.stop, 1, memory_order_relaxed);
            pthread_join(animation_thread, NULL);
        }
        if (discovery_raw) { set_mouse_reporting(0); disable_raw_mode(); }
        pthread_mutex_destroy(&results_mutex);
        free(results_list);
        results_list = NULL;
        result_count = 0;
        free(candidates);
        candidates = NULL;
        candidate_count = 0;
        nero_operation_end(&ui);
        puts("Scan cancelled. Returned to terminal.");
        return PORTSCANNER_CANCELLED;
    }

    /* Candidates dizisini oluştur: yalnızca filtrelenmiş portlar.
     * Animasyon thread'i candidate oluşturma sırasında results_list'i okur;
     * tutarlı bir anlık görüntü için kilit altında topla. */
    pthread_mutex_lock(&results_mutex);
    int filtered_total = 0;
    for (int i = 0; i < result_count; ++i)
        if (results_list[i].filtered) filtered_total++;

    if (filtered_total > 0 && filtered_total <= prof->deep_max_candidates) {
        candidates = malloc(filtered_total * sizeof(int));
        if (candidates) {
            int idx = 0;
            for (int i = 0; i < result_count; ++i) {
                if (results_list[i].filtered)
                    candidates[idx++] = results_list[i].port;
            }
            candidate_count = filtered_total;
        }
    }
    pthread_mutex_unlock(&results_mutex);

    /* Aşama 2: Event-driven batch derin tarama. Animasyon/girdi thread'i
     * çalışmaya devam eder; böylece ekran donmaz ve kullanıcı bu aşamada da
     * iptal edebilir. İlerleme çubuğu hızlı aşamada doldu; derin aşamada
     * latest_probe_port canlı etkinliği gösterir. */
    if (candidate_count > 0 && candidates) {
        int deep_thread_count = prof->deep_threads;
        if (deep_thread_count < 1) deep_thread_count = 1;
        if (deep_thread_count > MAX_THREADS) deep_thread_count = MAX_THREADS;
        if (deep_thread_count > candidate_count) deep_thread_count = candidate_count;

        pthread_t deep_threads[MAX_THREADS];
        deep_batch_args deep_args[MAX_THREADS];
        int created = 0;

        for (int i = 0; i < deep_thread_count; ++i) {
            deep_args[i].start_idx = (i * candidate_count) / deep_thread_count;
            deep_args[i].end_idx   = ((i + 1) * candidate_count) / deep_thread_count;
            deep_args[i].timeout_ms = prof->deep_timeout_ms;
            deep_args[i].batch = prof->deep_batch;
            if (pthread_create(&deep_threads[i], NULL, deep_scan_thread, &deep_args[i]) != 0)
                break;
            created++;
        }
        for (int i = 0; i < created; ++i) pthread_join(deep_threads[i], NULL);

        pthread_mutex_lock(&results_mutex);
        free(candidates);
        candidates = NULL;
        candidate_count = 0;
        pthread_mutex_unlock(&results_mutex);
    }

    /* Her iki tarama aşaması bitti: animasyon/girdi thread'ini şimdi durdur. */
    if (animation_started) {
        atomic_store_explicit(&animation.stop, 1, memory_order_relaxed);
        pthread_join(animation_thread, NULL);
    }
    if (discovery_raw) { set_mouse_reporting(0); disable_raw_mode(); }

    pthread_mutex_destroy(&results_mutex);

    /* Kullanıcı derin tarama sırasında iptal ettiyse menüyü atla. */
    if (portscanner_interrupt_requested || atomic_load_explicit(&scan_cancel_requested, memory_order_relaxed)) {
        free(results_list);
        results_list = NULL;
        result_count = 0;
        nero_operation_end(&ui);
        puts("Scan cancelled. Returned to terminal.");
        return PORTSCANNER_CANCELLED;
    }

    if (scan_alloc_failed) {
        nero_operation_end(&ui);
        fprintf(stderr, "[-] Memory allocation failed during scan.\n");
        free(results_list);
        results_list = NULL;
        result_count = 0;
        return -1;
    }

    /* Menü için yalnızca açık portları topla */
    int *open_ports = NULL;
    int open_count = 0;
    if (result_count > 0) {
        open_ports = malloc(result_count * sizeof(int));
        if (open_ports) {
            for (int i = 0; i < result_count; ++i) {
                if (!results_list[i].filtered)
                    open_ports[open_count++] = results_list[i].port;
            }
        }
    }

    char result_detail[96];
    if (filtered_total > 0) {
        snprintf(result_detail, sizeof(result_detail),
            "%d open, %d filtered (not shown)", open_count, filtered_total);
    } else {
        snprintf(result_detail, sizeof(result_detail),
            "%d open port%s found", open_count, open_count == 1 ? "" : "s");
    }
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

    int selected_port = select_port_menu_in_ui(open_ports, open_count, &ui);
    free(banner_port_list);
    free(banner_store);
    free(banner_texts);
    free(open_ports);
    free(results_list);
    results_list = NULL;
    nero_operation_end(&ui);
    if (selected_port == PORTSCANNER_CANCELLED)
        puts("Port selection cancelled. Returned to terminal.");
    return selected_port;
}
