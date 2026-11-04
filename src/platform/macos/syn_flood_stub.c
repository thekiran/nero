/*
 * NERO macOS IPv4 SYN diagnostic.
 *
 * Sends at most one TCP SYN to an authorized private-network or loopback
 * destination. The legacy SYN_Flood() name is retained for NERO compatibility.
 * No worker threads, transmission loops, source spoofing, or port scans.
 *
 * The adapter does not determine whether the destination port is open.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "nero/syn_flood_powered.h"

#define NERO_IPV4_HEADER_LEN 20U
#define NERO_TCP_HEADER_LEN 20U
#define NERO_PACKET_LEN (NERO_IPV4_HEADER_LEN + NERO_TCP_HEADER_LEN)
#define NERO_DEFAULT_PORT 80U

/* Store integers in network byte order without relying on packed structs. */
static void write_u16_be(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value >> 8);
    buffer[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value >> 24);
    buffer[1] = (uint8_t)(value >> 16);
    buffer[2] = (uint8_t)(value >> 8);
    buffer[3] = (uint8_t)value;
}

/* RFC 1071 one's-complement checksum over the supplied bytes. */
static uint16_t checksum16(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    size_t index = 0;

    for (; index + 1 < length; index += 2) {
        sum += ((uint32_t)data[index] << 8) | data[index + 1];
    }
    if (index < length) {
        sum += (uint32_t)data[index] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* Restrict raw-packet diagnostics to RFC 1918 and loopback IPv4 targets. */
static int permitted_lab_ip(struct in_addr address)
{
    const uint32_t ip = ntohl(address.s_addr);

    return (ip & 0xff000000U) == 0x0a000000U ||  /* 10.0.0.0/8 */
           (ip & 0xfff00000U) == 0xac100000U ||  /* 172.16.0.0/12 */
           (ip & 0xffff0000U) == 0xc0a80000U ||  /* 192.168.0.0/16 */
           (ip & 0xff000000U) == 0x7f000000U;    /* 127.0.0.0/8 */
}

/* Determine the local source address from the route; send no UDP payload. */
static int local_source_for(const struct sockaddr_in *destination,
                            struct in_addr *source)
{
    struct sockaddr_in local;
    socklen_t local_length = (socklen_t)sizeof(local);
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (socket_fd < 0) {
        perror("route socket");
        return -1;
    }
    if (connect(socket_fd, (const struct sockaddr *)destination,
                sizeof(*destination)) < 0) {
        perror("route selection");
        close(socket_fd);
        return -1;
    }

    memset(&local, 0, sizeof(local));
    if (getsockname(socket_fd, (struct sockaddr *)&local, &local_length) < 0) {
        perror("getsockname");
        close(socket_fd);
        return -1;
    }

    close(socket_fd);
    *source = local.sin_addr;
    return 0;
}

/* Construct one IPv4/TCP SYN packet and attempt exactly one transmission. */
int nero_syn_probe_once(const char *target_ip, uint16_t target_port)
{
    struct sockaddr_in destination;
    struct in_addr source;
    uint8_t packet[NERO_PACKET_LEN] = {0};
    uint8_t pseudo_header[12U + NERO_TCP_HEADER_LEN] = {0};
    uint8_t ip_wire[NERO_IPV4_HEADER_LEN];
    uint32_t nonce;
    uint16_t source_port;
    int socket_fd;
    int include_header = 1;
    ssize_t bytes_sent;

    if (target_ip == NULL || target_port == 0) {
        fputs("[SYN] Invalid IP address or TCP port.\n", stderr);
        return 1;
    }

    memset(&destination, 0, sizeof(destination));
#if defined(__APPLE__)
    destination.sin_len = (uint8_t)sizeof(destination);
#endif
    destination.sin_family = AF_INET;
    destination.sin_port = htons(target_port);

    if (inet_pton(AF_INET, target_ip, &destination.sin_addr) != 1 ||
        !permitted_lab_ip(destination.sin_addr)) {
        fputs("[SYN] Only private or loopback IPv4 addresses are permitted.\n",
              stderr);
        return 1;
    }
    if (local_source_for(&destination, &source) != 0) {
        return 1;
    }

#if defined(__APPLE__)
    nonce = arc4random();
#else
    nonce = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);
#endif
    source_port = (uint16_t)(49152U + (nonce & 0x3fffU));

    /* IPv4: 20-byte header, no options, no fragmentation. */
    packet[0] = 0x45;
#if defined(__APPLE__)
    /* Darwin IP_HDRINCL expects ip_len and ip_off in host byte order. */
    {
        const uint16_t host_length = (uint16_t)NERO_PACKET_LEN;
        const uint16_t host_offset = 0;
        memcpy(packet + 2, &host_length, sizeof(host_length));
        memcpy(packet + 6, &host_offset, sizeof(host_offset));
    }
#else
    write_u16_be(packet + 2, (uint16_t)NERO_PACKET_LEN);
    write_u16_be(packet + 6, 0);
#endif
    write_u16_be(packet + 4, (uint16_t)(nonce & 0xffffU));
    packet[8] = 64;
    packet[9] = IPPROTO_TCP;
    memcpy(packet + 12, &source.s_addr, 4);
    memcpy(packet + 16, &destination.sin_addr.s_addr, 4);

    /* TCP: SYN flag only; no payload or TCP options. */
    write_u16_be(packet + 20, source_port);
    write_u16_be(packet + 22, target_port);
    write_u32_be(packet + 24, nonce);
    packet[32] = 0x50;
    packet[33] = 0x02;
    write_u16_be(packet + 34, 8192);

    /* TCP checksum covers the pseudo-header and the TCP header. */
    memcpy(pseudo_header, &source.s_addr, 4);
    memcpy(pseudo_header + 4, &destination.sin_addr.s_addr, 4);
    pseudo_header[9] = IPPROTO_TCP;
    write_u16_be(pseudo_header + 10, NERO_TCP_HEADER_LEN);
    memcpy(pseudo_header + 12, packet + 20, NERO_TCP_HEADER_LEN);
    write_u16_be(packet + 36, checksum16(pseudo_header, sizeof(pseudo_header)));

    /* Calculate the IPv4 checksum using the wire-format representation. */
    memcpy(ip_wire, packet, sizeof(ip_wire));
    write_u16_be(ip_wire + 2, (uint16_t)NERO_PACKET_LEN);
    write_u16_be(ip_wire + 6, 0);
    write_u16_be(packet + 10, checksum16(ip_wire, sizeof(ip_wire)));

    socket_fd = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (socket_fd < 0) {
        perror("raw socket (administrator privileges may be required)");
        return 1;
    }
    if (setsockopt(socket_fd, IPPROTO_IP, IP_HDRINCL,
                   &include_header, sizeof(include_header)) < 0) {
        perror("IP_HDRINCL");
        close(socket_fd);
        return 1;
    }

    /* Exactly one sendto() call; no retry, no worker, no sending loop. */
    bytes_sent = sendto(socket_fd, packet, sizeof(packet), 0,
                        (const struct sockaddr *)&destination,
                        sizeof(destination));
    if (bytes_sent < 0) {
        perror("sendto SYN");
        close(socket_fd);
        return 1;
    }

    close(socket_fd);
    if ((size_t)bytes_sent != sizeof(packet)) {
        fprintf(stderr, "[SYN] Incomplete send: %zd bytes.\n", bytes_sent);
        return 1;
    }

    printf("[SYN] One %u-byte SYN packet queued for %s:%u. "
           "No response was checked.\n",
           (unsigned)NERO_PACKET_LEN, target_ip, (unsigned)target_port);
    return 0;
}

/* Preserve the existing NERO entry point and its default TCP port. */
int SYN_Flood(const char *target_ip)
{
    return nero_syn_probe_once(target_ip, (uint16_t)NERO_DEFAULT_PORT);
}

#ifdef NERO_SYN_STANDALONE
/* Optional standalone diagnostic executable, separate from the NERO CLI. */
int main(int argc, char **argv)
{
    char *end = NULL;
    unsigned long port = NERO_DEFAULT_PORT;

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: %s <private-IPv4> [tcp-port]\n", argv[0]);
        return 2;
    }
    if (argc == 3) {
        errno = 0;
        port = strtoul(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' ||
            port < 1 || port > 65535) {
            fputs("Invalid TCP port.\n", stderr);
            return 2;
        }
    }
    return nero_syn_probe_once(argv[1], (uint16_t)port);
}
#endif
