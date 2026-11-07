/* One bounded TCP exchange on IPv4 loopback, using the production wrappers. */
#include "../../src/platform/windows/nero_win_compat.h"
#include "nero/socket_handle.h"
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "FAIL line %d: %s (WSA=%d)\n", __LINE__, #expression, WSAGetLastError()); return 1; \
} } while (0)
int main(void)
{
    WSADATA wsa;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    nero_socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener != NERO_INVALID_SOCKET);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(listen(listener, 1) == 0);
    int length = sizeof(address);
    CHECK(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    nero_socket_t client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(client != NERO_INVALID_SOCKET);
    CHECK(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    nero_socket_t peer = accept(listener, NULL, NULL);
    CHECK(peer != NERO_INVALID_SOCKET);
    DWORD timeout = 5000;
    CHECK(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == 0);
    struct pollfd ready = {client, POLLOUT, 0};
    CHECK(nero_socket_poll(&ready, 1, 5000) == 1 && (ready.revents & POLLOUT));
    CHECK(send(client, "N", 1, 0) == 1);
    char byte = 0;
    CHECK(recv(peer, &byte, 1, 0) == 1 && byte == 'N');
    int error = -1;
    socklen_t error_length = sizeof(error);
    CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 && error == 0);
    close(peer); close(client); close(listener); WSACleanup();
    puts("PASS: production socket/connect/poll/getsockopt wrappers, one loopback TCP exchange");
    return 0;
}
