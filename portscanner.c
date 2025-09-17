#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>

#define PORT_SCAN_START 1
#define PORT_SCAN_END 1000 // İsterseniz 65535 yapabilirsiniz
#define TIMEOUT_USEC 500000 // 0.5 saniye

int portscanner(const char *target_ip) {
    struct sockaddr_in addr;
    int port;
    int open_ports = 0;

    // IP adresinin geçerli olup olmadığını kontrol et
    memset(&addr, 0, sizeof(addr));
    if (inet_pton(AF_INET, target_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[-] Geçersiz IP adresi: %s\n", target_ip);
        return 1;
    }
    addr.sin_family = AF_INET;

    printf("\n[*] Taranıyor: %s\n", target_ip);

    for (port = PORT_SCAN_START; port <= PORT_SCAN_END; port++) {
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            perror("Socket hatası");
            continue;
        }

        addr.sin_port = htons(port);

        // Timeout ayarı (0.5 saniye)
        struct timeval tv = {0};
        tv.tv_sec = 0;
        tv.tv_usec = TIMEOUT_USEC;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            printf("[+] Port %d açık\n", port);
            open_ports++;
        }

        close(sockfd);
    }

    printf("[*] Toplam açık port: %d\n", open_ports);
    int port_select;
    while (1) {
        printf("Please select a port to check its service (1-%d): ", open_ports);
        fflush(stdout);
        if (scanf("%d", &port_select) != 1) {
            fprintf(stderr, "[-] Geçersiz giriş\n");
            while(getchar() != '\n'); // buffer'ı temizle
            continue;
        }

        if (port_select < 1 || port_select > open_ports) {
            fprintf(stderr, "[-] Geçersiz port numarası, lütfen 1 ile %d arasında bir değer girin\n", open_ports);
            continue;
        }

        // Kullanıcının seçtiği porta karşılık gelen servisi bul
        // Bu bölüm eksik, çünkü servis bilgisi tarama sırasında elde edilmedi
        printf("[*] Seçilen port: %d\n", port_select);
        // Burada, port_select'e karşılık gelen servisi bulmak için ek kod gerekecek

        break;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Kullanım: %s <hedef_ip>\n", argv[0]);
        return 1;
    }

    portscanner(argv[1]);

    return 0;
}