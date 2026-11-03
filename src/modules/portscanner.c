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
#include <fcntl.h>      // for fcntl
#include <sys/select.h> // for select

#define NUM_THREADS 200 // can be lowered if needed

// --- Global Variables ---
static struct termios orig_termios;
int *open_ports_list = NULL;
int port_counter = 0;
static int scan_alloc_failed = 0;
const char *target_ip_global;
pthread_mutex_t ports_mutex;

// --- Terminal Functions ---
static int raw_mode_active = 0;

void disable_raw_mode(void) {
    if (!raw_mode_active)
        return;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &orig_termios);
    raw_mode_active = 0;
}

int enable_raw_mode(void) {
    if (!isatty(STDIN_FILENO))
        return -1;
    if (tcgetattr(STDIN_FILENO, &orig_termios) != 0)
        return -1;
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) != 0)
        return -1;
    raw_mode_active = 1;
    atexit(disable_raw_mode);
    return 0;
}

// --- Port Menu (unchanged logic) ---
// Comparison function used by qsort
int compare_desc(const void *a, const void *b) {
    return (*(int*)a - *(int*)b);
}

// --- Port Menu (sorted entries) ---
// Returns the chosen port (>= 1), 0 when there is nothing to select, or -1
// when the menu input ends before a selection (EOF/read error) — never a
// silent "first port" guess.
int select_port_menu(int *open_ports, int count) {
    if (count == 0) {
        printf("[!] No open ports found to select.\n");
        return 0;
    }

    // Sort ports before displaying the menu
    qsort(open_ports, count, sizeof(int), compare_desc);

    int raw_mode = enable_raw_mode() == 0;
    int current_selection = 0;
    int input_ok = 1;
    char c;

    // Draw the menu for the first time (using the sorted list)
    for (int i = 0; i < count; i++) {
        if (i == 0) printf(" > \033[7m%d\033[0m\n", open_ports[i]);
        else printf("   %d\n", open_ports[i]);
    }

    while (1) {
        printf("\x1b[%dA", count); // Move cursor to the top of the menu

        // Redraw the menu
        for (int i = 0; i < count; i++) {
            printf("\x1b[K"); // Clear the line
            if (i == current_selection) printf(" > \033[7m%d\033[0m\n", open_ports[i]);
            else printf("   %d\n", open_ports[i]);
        }

        ssize_t got = read(STDIN_FILENO, &c, 1);
        if (got != 1) {
            input_ok = 0; /* EOF or read error: no valid selection. */
            break;
        }

        if (c == '\x1b') { // Arrow keys
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) { input_ok = 0; break; }
            if (read(STDIN_FILENO, &seq[1], 1) != 1) { input_ok = 0; break; }
            if (seq[0] == '[') {
                if (seq[1] == 'A' && current_selection > 0) current_selection--;
                else if (seq[1] == 'B' && current_selection < count - 1) current_selection++;
            }
        } else if (c == '\n' || c == '\r') {
            break;
        }
    }
    printf("\x1b[%dA", count);
    printf("\x1b[0J");
    if (raw_mode)
        disable_raw_mode();
    if (!input_ok) {
        printf("[!] Port selection aborted (input ended).\n");
        return -1;
    }
    printf("Selected Port: %d\n", open_ports[current_selection]);
    return open_ports[current_selection];
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
            printf("\r\x1b[K[+] \033[92mOpen Port: %d\033[0m\n", port); // Fixed typo here
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
                printf("\r\x1b[K[+] \033[92mOpen Port: %d\033[0m\n", port);
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

// Main controller
// Returns the selected port (>= 1), 0 when no open port was found, 1 on an
// internal failure (partial scan or allocation), or -1 when the user aborted
// the selection menu with EOF/input error.
int portscanner(const char *target_ip) {
    target_ip_global = target_ip;
    pthread_t threads[NUM_THREADS];
    thread_args t_args[NUM_THREADS];

    // Initial setup
    open_ports_list = NULL;
    port_counter = 0;
    scan_alloc_failed = 0;

    if (pthread_mutex_init(&ports_mutex, NULL) != 0) {
        fprintf(stderr, "[-] Failed to initialize mutex.\n");
        return 1;
    }

    int ports_per_thread = 65535 / NUM_THREADS;
    if (ports_per_thread == 0) ports_per_thread = 65535;

    printf("\n[\x1b[93m*\x1b[0m] Scanning %s with %d threads...\n", target_ip, NUM_THREADS);

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

    for (int i = 0; i < threads_created; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&ports_mutex);

    if (threads_created != NUM_THREADS) {
        fprintf(stderr, "[-] Scan incomplete: only %d of %d workers started.\n",
                threads_created, NUM_THREADS);
        free(open_ports_list);
        open_ports_list = NULL;
        port_counter = 0;
        return 1;
    }

    if (scan_alloc_failed) {
        fprintf(stderr, "[-] Memory allocation failed during scan.\n");
        free(open_ports_list);
        open_ports_list = NULL;
        port_counter = 0;
        return 1;
    }

    clear_lines(port_counter);
    printf("Total open ports found: \x1b[93m%d\x1b[0m\n", port_counter);

    int selected_port = select_port_menu(open_ports_list, port_counter);
    free(open_ports_list);
    open_ports_list = NULL;
    return selected_port;
}
