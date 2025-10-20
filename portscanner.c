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
const char *target_ip_global;
pthread_mutex_t ports_mutex;

// --- Terminal Functions ---
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// --- Port Menu (unchanged logic) ---
// qsort için gerekli olan karşılaştırma fonksiyonu (büyükten küçüğe)
int compare_desc(const void *a, const void *b) {
    return (*(int*)a - *(int*)b);
}

// --- Port Menu (Sıralama eklenmiş hali) ---
int select_port_menu(int *open_ports, int count) {
    if (count == 0) {
        printf("[!] No open ports found to select.\n");
        return 0;
    }

    // *** YENİ EKLENEN SATIR: Portları büyükten küçüğe sırala ***
    qsort(open_ports, count, sizeof(int), compare_desc);

    enable_raw_mode();
    int current_selection = 0;
    char c;

    // Draw the menu for the first time (artık sıralı liste ile)
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

        if (read(STDIN_FILENO, &c, 1) != 1) continue;

        if (c == '\x1b') { // Arrow keys
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;
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
    disable_raw_mode();
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
                perror("Memory error");
                pthread_mutex_unlock(&ports_mutex);
                close(sockfd);
                exit(1);
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
                    perror("Memory error");
                    pthread_mutex_unlock(&ports_mutex);
                    close(sockfd);
                    exit(1);
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
int portscanner(const char *target_ip) {
    target_ip_global = target_ip;
    pthread_t threads[NUM_THREADS];
    thread_args t_args[NUM_THREADS];

    // Initial setup
    open_ports_list = NULL;
    port_counter = 0;

    if (pthread_mutex_init(&ports_mutex, NULL) != 0) {
        fprintf(stderr, "[-] Failed to initialize mutex.\n");
        return 1;
    }

    int ports_per_thread = 65535 / NUM_THREADS;
    if (ports_per_thread == 0) ports_per_thread = 65535;

    printf("\n[\x1b[93m*\x1b[0m] Scanning %s with %d threads...\n", target_ip, NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++) {
        t_args[i].start_port = (i * ports_per_thread) + 1;
        if (i == NUM_THREADS - 1) t_args[i].end_port = 65535;
        else t_args[i].end_port = (i + 1) * ports_per_thread;

        if (pthread_create(&threads[i], NULL, scan_ports_thread, &t_args[i]) != 0) {
            fprintf(stderr, "[-] Failed to create thread %d.\n", i);
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&ports_mutex);

    clear_lines(port_counter);
    printf("Total open ports found: \x1b[93m%d\x1b[0m\n", port_counter);

    int selected_port = select_port_menu(open_ports_list, port_counter);
    free(open_ports_list);
    return selected_port;
}
