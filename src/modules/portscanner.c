/* Concurrent TCP discovery and port selection.
 * Workers own their sockets; the controller owns the result buffer, mutex
 * and operation surface. UI input is consumed by one animation worker during
 * discovery, then by the selection menu after that worker has joined. */
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
#include <fcntl.h>      // for fcntl
#include <sys/select.h> // for select
#include <time.h>

#include "nero/operation_ui.h"
#include "nero/portscanner.h"

#define NUM_THREADS 200 // can be lowered if needed

// --- Global Variables ---
static struct termios orig_termios;
int *open_ports_list = NULL;
int port_counter = 0;
static int scan_alloc_failed = 0;
volatile sig_atomic_t portscanner_interrupt_requested = 0;
/* Keyboard cancellation crosses threads through an atomic flag; signal
 * cancellation uses the separate sig_atomic_t flag above. */
static atomic_int scan_cancel_requested;
const char *target_ip_global;
pthread_mutex_t ports_mutex;

// --- Terminal Functions ---
static int raw_mode_active = 0;

#define SCAN_FRAME_NANOSECONDS 70000000L

typedef struct {
    atomic_int stop;
    const char *target_ip;
    int animate;
} scan_animation_state;

/* Reduce Motion preserves input handling while suppressing animated frames. */
static int scan_motion_enabled(void)
{
    const char *reduced = getenv("NERO_REDUCE_MOTION");
    const char *term = getenv("TERM");
    return isatty(STDOUT_FILENO) &&
           reduced == NULL &&
           (term == NULL || strcmp(term, "dumb") != 0);
}

/* This worker is the sole discovery UI writer and keyboard reader. It must
 * finish before the controller redraws results or releases its stack state. */
static void *scan_animation_worker(void *arg)
{
    scan_animation_state *state = arg;
    const struct timespec frame_delay = {
        .tv_sec = 0,
        .tv_nsec = SCAN_FRAME_NANOSECONDS
    };
    unsigned frame = 0;

    while (!atomic_load_explicit(&state->stop, memory_order_relaxed)) {
        if (state->animate) {
            nero_operation_loader("Scanning", state->target_ip, frame);
            frame = (frame + 1) % 6;
        }

        if (nero_operation_read_key(0) == NERO_UI_KEY_CANCEL)
            atomic_store_explicit(&scan_cancel_requested, 1, memory_order_relaxed);
        nanosleep(&frame_delay, NULL);
    }
    return NULL;
}

void disable_raw_mode(void) {
    if (!raw_mode_active)
        return;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &orig_termios);
    raw_mode_active = 0;
}

int enable_raw_mode(void) {
    /* The shell in terminal.c already installs its own atexit cleanup; register
     * our fallback only once so repeated port scans do not stack duplicate
     * entries on the atexit list. */
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

/* Ascending numeric order gives the selection menu a stable initial item. */
int compare_desc(const void *a, const void *b) {
    return (*(int*)a - *(int*)b);
}

// --- Port Menu (sorted entries) ---
/* A selection borrows the controller's result buffer. Enter confirms a port;
 * cancellation and input failure remain distinct negative return values. */
static int select_port_menu_in_ui(
    int *open_ports,
    int count,
    nero_operation_ui *ui
) {
    (void)ui;
    if (count == 0) {
        return 0;
    }

    // Sort ports before displaying the menu
    qsort(open_ports, count, sizeof(int), compare_desc);

    int raw_mode = enable_raw_mode() == 0;
    int current_selection = 0;
    int input_ok = 1;

    nero_operation_section("OPEN PORTS");
    printf("\033[2mUp/Down  Move    Enter  Select    Esc  Back\033[0m\n\n");

    // Draw the menu for the first time (using the sorted list)
    for (int i = 0; i < count; i++) {
        if (i == 0) printf("  \033[36m>\033[0m \033[7m %d \033[0m\n", open_ports[i]);
        else printf("    %d\n", open_ports[i]);
    }
    fflush(stdout);

    while (1) {
        printf("\x1b[%dA", count); // Move cursor to the top of the menu

        // Redraw the menu
        for (int i = 0; i < count; i++) {
            printf("\x1b[K"); // Clear the line
            if (i == current_selection)
                printf("  \033[36m>\033[0m \033[7m %d \033[0m\n", open_ports[i]);
            else
                printf("    %d\n", open_ports[i]);
        }
        fflush(stdout);

        nero_ui_key key = nero_operation_read_key(-1);
        if (key == NERO_UI_KEY_EOF) {
            input_ok = 0;
            break;
        }
        if (key == NERO_UI_KEY_CANCEL) {
            if (raw_mode)
                disable_raw_mode();
            return PORTSCANNER_CANCELLED;
        }
        if (key == NERO_UI_KEY_UP && current_selection > 0)
            current_selection--;
        else if (key == NERO_UI_KEY_DOWN && current_selection < count - 1)
            current_selection++;
        else if (key == NERO_UI_KEY_ENTER)
            break;
    }
    printf("\x1b[%dA", count);
    printf("\x1b[0J");
    if (raw_mode)
        disable_raw_mode();
    if (!input_ok) {
        printf("[!] Port selection aborted (input ended).\n");
        return -1;
    }
    return open_ports[current_selection];
}

int select_port_menu(int *open_ports, int count)
{
    nero_operation_ui ui = {0};
    nero_operation_begin(
        &ui,
        "PORT SELECTION",
        "Choose an open port",
        "Esc · Back"
    );
    int result = select_port_menu_in_ui(open_ports, count, &ui);
    nero_operation_end(&ui);
    return result;
}

// Thread args struct
typedef struct {
    int start_port;
    int end_port;
} thread_args;

// Thread function for scanning with non-blocking connect + select
void *scan_ports_thread(void *args) {
    thread_args *t_args = (thread_args *)args;
    struct sockaddr_in addr;
    int sockfd;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, target_ip_global, &addr.sin_addr) != 1) {
        // Invalid IP, just exit
        pthread_exit(NULL);
    }

    for (int port = t_args->start_port; port <= t_args->end_port; port++) {
        if (portscanner_interrupt_requested ||
            atomic_load_explicit(&scan_cancel_requested, memory_order_relaxed))
            break;

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            continue;
        }

        addr.sin_port = htons(port);

        // Make the socket non-blocking
        int flags = fcntl(sockfd, F_GETFL, 0);
        if (flags < 0) { close(sockfd); continue; }
        if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) { close(sockfd); continue; }

        int ret = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));
        if (ret == 0) {
            // Connected immediately
            pthread_mutex_lock(&ports_mutex);
            port_counter++;
            int *tmp = realloc(open_ports_list, port_counter * sizeof(int));
            if (!tmp) {
                scan_alloc_failed = 1;
                pthread_mutex_unlock(&ports_mutex);
                close(sockfd);
                pthread_exit(NULL);
            }
            open_ports_list = tmp;
            open_ports_list[port_counter - 1] = port;
            pthread_mutex_unlock(&ports_mutex);
            close(sockfd);
            continue;
        } else if (ret < 0 && errno != EINPROGRESS) {
            // Immediate error (e.g., connection refused)
            close(sockfd);
            continue;
        }

        // connect returned EINPROGRESS -> wait with select
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sockfd, &wfds);
        struct timeval tv;
        tv.tv_sec = 1;  // 1 second timeout (can be reduced)
        tv.tv_usec = 0;

        int sel = select(sockfd + 1, NULL, &wfds, NULL, &tv);
        if (sel > 0 && FD_ISSET(sockfd, &wfds)) {
            // Check for errors
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
                close(sockfd);
                continue;
            }
            if (so_error == 0) {
                // Connection successful
                pthread_mutex_lock(&ports_mutex);
                port_counter++;
                int *tmp = realloc(open_ports_list, port_counter * sizeof(int));
                if (!tmp) {
                    scan_alloc_failed = 1;
                    pthread_mutex_unlock(&ports_mutex);
                    close(sockfd);
                    pthread_exit(NULL);
                }
                open_ports_list = tmp;
                open_ports_list[port_counter - 1] = port;
                pthread_mutex_unlock(&ports_mutex);
            }
        }
        // Close the socket
        close(sockfd);
    }
    pthread_exit(NULL);
}

// Function to clear lines
void clear_lines(int n) {
    if (n <= 0) return;
    printf("\x1b[%dA", n);
    for (int i = 0; i < n; i++) {
        printf("\x1b[2K");
        printf("\x1b[1B");
    }
    printf("\x1b[%dA", n);
    fflush(stdout);
}

/* Session controller: initialize, discover, join, select, then release.
 * Returns a positive selected port, zero for an empty result, -2 for user
 * cancellation or -1 for failure. Partial scans never become valid results. */
int portscanner(const char *target_ip) {
    target_ip_global = target_ip;
    pthread_t threads[NUM_THREADS];
    thread_args t_args[NUM_THREADS];

    // Initial setup
    open_ports_list = NULL;
    port_counter = 0;
    scan_alloc_failed = 0;
    portscanner_interrupt_requested = 0;
    atomic_init(&scan_cancel_requested, 0);

    if (pthread_mutex_init(&ports_mutex, NULL) != 0) {
        fprintf(stderr, "[-] Failed to initialize mutex.\n");
        return -1;
    }

    int ports_per_thread = 65535 / NUM_THREADS;
    if (ports_per_thread == 0) ports_per_thread = 65535;

    nero_operation_ui ui = {0};
    char operation_detail[96];
    snprintf(
        operation_detail,
        sizeof(operation_detail),
        "%s",
        target_ip
    );
    nero_operation_begin(
        &ui,
        "Port scan",
        operation_detail,
        "Esc · Back"
    );

    int threads_created = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        t_args[i].start_port = (i * ports_per_thread) + 1;
        if (i == NUM_THREADS - 1) t_args[i].end_port = 65535;
        else t_args[i].end_port = (i + 1) * ports_per_thread;

        if (pthread_create(&threads[i], NULL, scan_ports_thread, &t_args[i]) != 0) {
            fprintf(stderr, "[-] Failed to create thread %d.\n", i);
            break;
        }
        threads_created++;
    }

    scan_animation_state animation = {
        .target_ip = target_ip,
        .animate = scan_motion_enabled()
    };
    atomic_init(&animation.stop, 0);
    pthread_t animation_thread;
    int animation_started = 0;

    if (isatty(STDIN_FILENO)) {
        animation_started = pthread_create(
            &animation_thread,
            NULL,
            scan_animation_worker,
            &animation
        ) == 0;
    }
    if (!animation_started || !animation.animate)
        nero_operation_loader("Scanning", target_ip, 6);

    /* Join only successfully created workers. Their socket/result accesses
     * must end before the mutex or shared result buffer can be released. */
    for (int i = 0; i < threads_created; i++) {
        pthread_join(threads[i], NULL);
    }

    if (animation_started) {
        atomic_store_explicit(&animation.stop, 1, memory_order_relaxed);
        pthread_join(animation_thread, NULL);
    }
    if (portscanner_interrupt_requested ||
        atomic_load_explicit(&scan_cancel_requested, memory_order_relaxed)) {
        pthread_mutex_destroy(&ports_mutex);
        free(open_ports_list);
        open_ports_list = NULL;
        port_counter = 0;
        nero_operation_end(&ui);
        puts("Scan cancelled. Returned to terminal.");
        return PORTSCANNER_CANCELLED;
    }

    pthread_mutex_destroy(&ports_mutex);

    if (threads_created != NUM_THREADS) {
        nero_operation_end(&ui);
        fprintf(stderr, "[-] Scan incomplete: only %d of %d workers started.\n",
                threads_created, NUM_THREADS);
        free(open_ports_list);
        open_ports_list = NULL;
        port_counter = 0;
        return -1;
    }

    if (scan_alloc_failed) {
        nero_operation_end(&ui);
        fprintf(stderr, "[-] Memory allocation failed during scan.\n");
        free(open_ports_list);
        open_ports_list = NULL;
        port_counter = 0;
        return -1;
    }

    char result_detail[64];
    snprintf(
        result_detail,
        sizeof(result_detail),
        "%d open port%s found",
        port_counter,
        port_counter == 1 ? "" : "s"
    );
    nero_operation_status("32", "Scan complete", result_detail);

    int selected_port = select_port_menu_in_ui(open_ports_list, port_counter, &ui);
    free(open_ports_list);
    open_ports_list = NULL;
    nero_operation_end(&ui);
    if (selected_port == PORTSCANNER_CANCELLED)
        puts("Port selection cancelled. Returned to terminal.");
    return selected_port;
}
