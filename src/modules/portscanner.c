/* Concurrent TCP discovery with iOS-focused adaptive scanning.
 * Fast phase and deep phase use event-driven select() batching:
 * each worker thread manages BATCH_SIZE sockets concurrently,
 * so 8 threads * 400 sockets = 3200 simultaneous probes without
 * spawning thousands of threads or hitting macOS ulimit -n. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <termios.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <time.h>
#include <stdint.h>

#include "nero/operation_ui.h"
#include "nero/portscanner.h"

/* iOS odaklı öncelikli portlar */
static const int IOS_PRIORITY_PORTS[] = { 62078, 49152, 32498 };
#define IOS_PRIORITY_COUNT (sizeof(IOS_PRIORITY_PORTS) / sizeof(IOS_PRIORITY_PORTS[0]))

/* Batch scanner parametreleri.
 * 8 thread * 400 soket = 3200 eşzamanlı probe.
 * macOS'ta FD_SETSIZE 1024'tür, BATCH_SIZE bunun çok altında kalmalı.
 * ulimit -n değerinin en az 4000 olduğundan emin olun:
 *   ulimit -n 8192 */
#define FAST_BATCH_THREADS 8
#define FAST_BATCH_SIZE    400
#define FAST_TIMEOUT_MS    1200

#define DEEP_BATCH_THREADS 4
#define DEEP_BATCH_SIZE    200
#define DEEP_TIMEOUT_MS    2500

/* Çok fazla "filtered" varsa deep scan anlamsız olur; bu eşiğin
 * üzerinde atla. */
#define DEEP_SCAN_MAX_CANDIDATES 5000

#define SCAN_TOTAL_PORTS 65535
#define DISCOVERY_VIEW_CAP 512

/* Port başına kesin probe sonucu. */
typedef enum {
    PROBE_OPEN     =  1,
    PROBE_CLOSED   =  0,
    PROBE_FILTERED = -1,
    PROBE_ERROR    = -2
} probe_status;

typedef struct {
    int port;
    int filtered;
} port_result;

typedef struct {
    int start_port;
    int end_port;
    int timeout_ms;
} batch_thread_args;

typedef struct {
    int start_idx;
    int end_idx;
    int timeout_ms;
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

static int scan_motion_enabled(void) {
    const char *reduced = getenv("NERO_REDUCE_MOTION");
    const char *term = getenv("TERM");
    return isatty(STDOUT_FILENO) && reduced == NULL &&
           (term == NULL || strcmp(term, "dumb") != 0);
}

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

void disable_raw_mode(void) {
    if (!raw_mode_active) return;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &orig_termios);
    raw_mode_active = 0;
}

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

static void set_mouse_reporting(int enable) {
    fputs(enable ? "\033[?1000h\033[?1006h" : "\033[?1006l\033[?1000l", stdout);
    fflush(stdout);
}

static int compare_ports_asc(const void *a, const void *b) {
    return (*(const int *)a - *(const int *)b);
}

/* Bir batch taraması aynı anda binlerce soket açabilir. Varsayılan
 * RLIMIT_NOFILE (macOS'ta sıklıkla 256) buna yetmez ve socket() EMFILE ile
 * başarısız olur. Soft limiti, hard limiti aşmadan ihtiyaç kadar yükselt. */
static void raise_fd_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    rlim_t needed = (rlim_t)FAST_BATCH_THREADS * FAST_BATCH_SIZE + 64;
    if (rl.rlim_cur >= needed) return;
    rlim_t target = needed;
    if (rl.rlim_max != RLIM_INFINITY && target > rl.rlim_max)
        target = rl.rlim_max;
    rl.rlim_cur = target;
    setrlimit(RLIMIT_NOFILE, &rl);
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

int select_port_menu(int *open_ports, int count) {
    nero_operation_ui ui = {0};
    nero_operation_begin(&ui, "PORT SELECTION", "Choose an open port", "Esc · Back");
    int result = select_port_menu_in_ui(open_ports, count, &ui);
    nero_operation_end(&ui);
    return result;
}

/* --- Tek port probe (yalnızca iOS öncelikli portlar için kullanılır) --- */
static probe_status probe_single_port(const char *ip, int port, int timeout_ms) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return PROBE_ERROR;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return PROBE_ERROR;

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0) { close(sockfd); return PROBE_ERROR; }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) { close(sockfd); return PROBE_ERROR; }

    int ret = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) { close(sockfd); return PROBE_OPEN; }
    if (ret < 0) {
        if (errno == ECONNREFUSED) { close(sockfd); return PROBE_CLOSED; }
        if (errno != EINPROGRESS) { close(sockfd); return PROBE_ERROR; }
    }

    /* poll() yerine select() kullanmamanın nedeni: fd değeri FD_SETSIZE'ı
     * aşarsa select/FD_SET tanımsız davranıştır. poll bu sınırdan muaftır. */
    struct pollfd pfd = { sockfd, POLLOUT, 0 };
    int pret;
    do { pret = poll(&pfd, 1, timeout_ms); } while (pret < 0 && errno == EINTR);
    if (pret < 0) { close(sockfd); return PROBE_ERROR; }
    if (pret == 0) { close(sockfd); return PROBE_FILTERED; }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
        close(sockfd);
        return PROBE_ERROR;
    }
    close(sockfd);

    if (so_error == 0)           return PROBE_OPEN;
    if (so_error == ECONNREFUSED) return PROBE_CLOSED;
    if (so_error == ETIMEDOUT)    return PROBE_FILTERED;
    return PROBE_ERROR;
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
 * hepsine non-blocking connect() gönderir, sonra tek bir select()
 * ile yanıtları bekler. Bu sayede aynı anda yüzlerce port probe
 * edilir; tek tek 1500 ms beklemek yerine toplu bekleriz. */
static void scan_ports_batch(
    const char *ip,
    const int *ports,
    int count,
    int timeout_ms
) {
    if (count > FAST_BATCH_SIZE) count = FAST_BATCH_SIZE;
    if (count <= 0) return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return;

    struct pollfd pfds[FAST_BATCH_SIZE];
    int slot_port[FAST_BATCH_SIZE];
    int nfds = 0;

    /* 1. Soketleri aç ve non-blocking connect() başlat. */
    for (int i = 0; i < count; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            /* Tanımlayıcı tükendi: portu sessizce "kapalı" saymak yerine
             * belirsiz (filtered) olarak işaretle; derin tarama yeniden dener. */
            if (errno == EMFILE || errno == ENFILE)
                add_filtered_candidate(ports[i]);
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            continue;
        }

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
            ready = poll(pfds, (nfds_t)nfds, wait_ms);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0) break; /* hata veya zaman aşımı → kalanlar filtered */

        for (int s = 0; s < nfds && ready > 0; ++s) {
            if (pfds[s].fd < 0 || pfds[s].revents == 0) continue;
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
            pfds[s].fd = -1; /* poll bundan sonra yok sayar (revents=0) */
            remaining--;
        }
    }

    /* 3. Zaman aşımına kadar yanıt vermeyen soketler → sessiz → filtered. */
    for (int s = 0; s < nfds; ++s) {
        if (pfds[s].fd < 0) continue;
        add_filtered_candidate(slot_port[s]);
        close(pfds[s].fd);
    }
}

/* --- Fast batch thread --- */
void *fast_scan_thread(void *args) {
    batch_thread_args *t = args;
    int ports[FAST_BATCH_SIZE];

    for (int base = t->start_port; base <= t->end_port; base += FAST_BATCH_SIZE) {
        if (portscanner_interrupt_requested ||
            atomic_load_explicit(&scan_cancel_requested, memory_order_relaxed))
            break;

        int count = 0;
        int end = base + FAST_BATCH_SIZE - 1;
        if (end > t->end_port) end = t->end_port;

        for (int port = base; port <= end; ++port)
            ports[count++] = port;

        scan_ports_batch(target_ip_global, ports, count, t->timeout_ms);
        atomic_fetch_add_explicit(&ports_scanned, count, memory_order_relaxed);
    }
    pthread_exit(NULL);
}

/* --- Deep batch thread (yalnızca candidates üzerinde) --- */
void *deep_scan_thread(void *args) {
    deep_batch_args *t = args;
    int ports[DEEP_BATCH_SIZE];

    for (int base = t->start_idx; base < t->end_idx; base += DEEP_BATCH_SIZE) {
        if (portscanner_interrupt_requested ||
            atomic_load_explicit(&scan_cancel_requested, memory_order_relaxed))
            break;

        int end = base + DEEP_BATCH_SIZE;
        if (end > t->end_idx) end = t->end_idx;

        int count = 0;
        for (int i = base; i < end && i < candidate_count; ++i)
            ports[count++] = candidates[i];

        if (count == 0) continue;

        scan_ports_batch(target_ip_global, ports, count, t->timeout_ms);
        atomic_fetch_add_explicit(&ports_scanned, count, memory_order_relaxed);
    }
    pthread_exit(NULL);
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

    /* Aşama 0: iOS öncelikli portları tek tek dene (yalnızca 3 port) */
    for (size_t i = 0; i < IOS_PRIORITY_COUNT; ++i) {
        int port = IOS_PRIORITY_PORTS[i];
        probe_status status = probe_single_port(target_ip, port, FAST_TIMEOUT_MS);
        if (status == PROBE_OPEN) {
            add_open_result(port);
        } else if (status == PROBE_FILTERED) {
            add_filtered_candidate(port);
        }
        atomic_fetch_add_explicit(&ports_scanned, 1, memory_order_relaxed);
    }

    /* Aşama 1: Event-driven batch hızlı keşif */
    pthread_t fast_threads[FAST_BATCH_THREADS];
    batch_thread_args fast_args[FAST_BATCH_THREADS];
    int ports_per_fast = SCAN_TOTAL_PORTS / FAST_BATCH_THREADS;
    if (ports_per_fast == 0) ports_per_fast = SCAN_TOTAL_PORTS;

    for (int i = 0; i < FAST_BATCH_THREADS; ++i) {
        fast_args[i].start_port = (i * ports_per_fast) + 1;
        fast_args[i].end_port = (i == FAST_BATCH_THREADS - 1)
            ? SCAN_TOTAL_PORTS
            : (i + 1) * ports_per_fast;
        fast_args[i].timeout_ms = FAST_TIMEOUT_MS;
        if (pthread_create(&fast_threads[i], NULL, fast_scan_thread, &fast_args[i]) != 0) {
            fprintf(stderr, "[-] Failed to create fast thread %d.\n", i);
            break;
        }
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

    for (int i = 0; i < FAST_BATCH_THREADS; ++i) pthread_join(fast_threads[i], NULL);

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

    if (filtered_total > 0 && filtered_total <= DEEP_SCAN_MAX_CANDIDATES) {
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
        pthread_t deep_threads[DEEP_BATCH_THREADS];
        deep_batch_args deep_args[DEEP_BATCH_THREADS];
        int created = 0;

        for (int i = 0; i < DEEP_BATCH_THREADS; ++i) {
            deep_args[i].start_idx = (i * candidate_count) / DEEP_BATCH_THREADS;
            deep_args[i].end_idx   = ((i + 1) * candidate_count) / DEEP_BATCH_THREADS;
            deep_args[i].timeout_ms = DEEP_TIMEOUT_MS;
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

    int selected_port = select_port_menu_in_ui(open_ports, open_count, &ui);
    free(open_ports);
    free(results_list);
    results_list = NULL;
    nero_operation_end(&ui);
    if (selected_port == PORTSCANNER_CANCELLED)
        puts("Port selection cancelled. Returned to terminal.");
    return selected_port;
}