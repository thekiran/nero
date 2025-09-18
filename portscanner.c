#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>

int portscanner(const char *target_ip) {
    struct sockaddr_in addr;
    int sockfd;
    int port;
    int port_select;
    int port_counter = 0;
    
    // IP adresinin geçerli olup olmadığını kontrol et
    if (inet_pton(AF_INET, target_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[-] Geçersiz IP adresi: %s\n", target_ip);
        return 1;
    }

    addr.sin_family = AF_INET;

    printf("\n[*] Taranıyor: %s\n", target_ip);

    for (port = 1; port <= 1000; port++) {//65535
        sockfd = socket(AF_INET, SOCK_STREAM, 0);

        if (sockfd < 0) {
            perror("Socket hatası");
            return 1;
        }

        addr.sin_port = htons(port);

        // Timeout ayarı (0.5 saniye)
        struct timeval tv;
        
        tv.tv_sec = 0;
        tv.tv_usec = 500000;

        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {

            printf("[+] Port %d açık\n", port);
            port_counter++;//test
        }

        close(sockfd);
    }
        
    
    while (1) 
    {
        printf("Please select a port (1-65535): ");
        fflush(stdout);

        int result = scanf("%d", &port_select);

        if (result != 1) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) { } // stdin temizle
            printf("Invalid input! Please enter a valid number.\n");
            continue;
        }

        if (port_select < 1 || port_select > 65535) {
            printf("Port number must be between 1 and 65535.\n");
            continue;
        }

        break;
    }

    return port_select;
}
