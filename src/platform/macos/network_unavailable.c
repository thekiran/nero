#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "nero/syn_flood_powered.h"

int SYN_Flood(const char *target_ip)
{
    if (target_ip == NULL) {
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in target = {0};

    target.sin_family = AF_INET;
    target.sin_port = htons(80);

    if (inet_pton(AF_INET, target_ip,
                  &target.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address\n");
        close(sock);
        return 1;
    }

    printf("Testing TCP connection to %s:80\n", target_ip);

    int result = connect(
        sock,
        (struct sockaddr *)&target,
        sizeof(target)
    );

    if (result == 0) {
        printf("TCP connection successful\n");
    } else {
        fprintf(stderr, "TCP connection failed: %s\n",
                strerror(errno));
    }

    close(sock);

    return result == 0 ? 0 : 1;
}