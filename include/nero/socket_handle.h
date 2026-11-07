#ifndef NERO_SOCKET_HANDLE_H
#define NERO_SOCKET_HANDLE_H
/* Socket handles are pointer-sized on Windows, integer descriptors on POSIX. */
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET nero_socket_t;
#define NERO_INVALID_SOCKET INVALID_SOCKET
#else
typedef int nero_socket_t;
#define NERO_INVALID_SOCKET (-1)
#define nero_socket_poll poll
#endif
#endif
