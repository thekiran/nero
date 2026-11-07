/*
 * Nero CLI command dispatcher.
 *
 * Supported commands:
 *   help
 *   help -All
 *   history
 *   history -clear
 *   clear
 *   version
 *   exit
 *   exit -All
 *
 * Network diagnostics:
 *   -mode
 *   -mode on
 *   -mode off
 *   <IPv4> -mode
 *   <IPv4> -Sc
 *
 * Legacy:
 *   <IPv4> -Syn
 *
 * -Syn validates its target but runs no network engine.
 * Offline test builds never generate network traffic.
 */

#include "cli_internal.h"
#include "command_catalog.h"
#include "nero/cli/parser.h"
#include "nero/ascii.h"
#include "nero/printing.h"
#include "nero/terminal.h"
#include "nero/network_diagnostics.h"
#include "nero/operation_ui.h"
#ifndef NERO_NETWORK_UNAVAILABLE
#include "nero/portscanner.h"
#endif

#ifndef NERO_NETWORK_UNAVAILABLE
#include "nero/syn_flood_powered.h"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stddef.h>     /* size_t */

#include <time.h>


#define CLI_MAX_TOKENS 64
#define TCP_TIMEOUT_SECONDS 1

typedef enum {
    CLI_CMD_CONTINUE = 0,
    CLI_CMD_EXIT = 1,
    CLI_CMD_FAILURE = -1,
    CLI_CMD_INVALID = -2
} cli_cmd_result;

/* Session-level network diagnostics setting. */
static int scan_enabled = 1;

/* ============================================================
 * TCP DIAGNOSTICS
 * ============================================================ */

/* These helpers are only reachable from the live -Sc path.  Offline and
 * network-unavailable builds must not compile them, otherwise -Werror flags
 * the unused network code. */
#if !defined(NERO_SAFE_TEST) && !defined(NERO_NETWORK_UNAVAILABLE)

typedef enum {
    TCP_PORT_OPEN,
    TCP_PORT_CLOSED,
    TCP_PORT_TIMEOUT,
    TCP_PORT_ERROR
} tcp_port_result;

static const char *tcp_result_string(tcp_port_result result)
{
    switch (result) {
        case TCP_PORT_OPEN:
            return "OPEN";
        case TCP_PORT_CLOSED:
            return "CLOSED";
        case TCP_PORT_TIMEOUT:
            return "TIMEOUT";
        default:
            return "ERROR / UNREACHABLE";
    }
}

/*
 * Perform one bounded TCP connection attempt.
 *
 * No raw sockets, crafted packets, spoofing,
 * packet floods or parallel scan workers.
 */
static tcp_port_result tcp_port_check(
    const char *ip,
    uint16_t port
)
{
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1)
        return TCP_PORT_ERROR;

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return TCP_PORT_ERROR;

    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return TCP_PORT_ERROR;
    }

    int status = connect(
        fd,
        (struct sockaddr *)&address,
        sizeof(address)
    );

    if (status == 0) {
        close(fd);
        return TCP_PORT_OPEN;
    }

    if (errno == ECONNREFUSED) {
        close(fd);
        return TCP_PORT_CLOSED;
    }

    if (errno != EINPROGRESS) {
        close(fd);
        return TCP_PORT_ERROR;
    }

    if (fd >= FD_SETSIZE) {
        close(fd);
        return TCP_PORT_ERROR;
    }

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(fd, &writefds);

    struct timeval timeout = {
        .tv_sec = TCP_TIMEOUT_SECONDS,
        .tv_usec = 0
    };

    status = select(
        fd + 1,
        NULL,
        &writefds,
        NULL,
        &timeout
    );

    if (status == 0) {
        close(fd);
        return TCP_PORT_TIMEOUT;
    }

    if (status < 0 || !FD_ISSET(fd, &writefds)) {
        close(fd);
        return TCP_PORT_ERROR;
    }

    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);

    if (getsockopt(
            fd,
            SOL_SOCKET,
            SO_ERROR,
            &socket_error,
            &error_length
        ) != 0) {
        close(fd);
        return TCP_PORT_ERROR;
    }

    close(fd);

    if (socket_error == 0)
        return TCP_PORT_OPEN;

    if (socket_error == ECONNREFUSED)
        return TCP_PORT_CLOSED;

    if (socket_error == ETIMEDOUT)
        return TCP_PORT_TIMEOUT;

    return TCP_PORT_ERROR;
}

#endif /* !NERO_SAFE_TEST && !NERO_NETWORK_UNAVAILABLE */

/* ============================================================
 * MODE COMMAND
 * ============================================================ */

static cli_cmd_result cmd_mode(char **tokens, size_t count)
{
    if (count == 1 && strcmp(tokens[0], "-mode") == 0) {
        printf(
            "\nNetwork diagnostics: %s\n",
            scan_enabled ? "ON" : "OFF"
        );
        return CLI_CMD_CONTINUE;
    }

    if (count == 2 && strcmp(tokens[0], "-mode") == 0) {
        if (strcmp(tokens[1], "on") == 0) {
            scan_enabled = 1;
        } else if (strcmp(tokens[1], "off") == 0) {
            scan_enabled = 0;
        } else {
            return CLI_CMD_INVALID;
        }

        printf(
            "\nNetwork diagnostics: %s\n",
            scan_enabled ? "ON" : "OFF"
        );

        return CLI_CMD_CONTINUE;
    }

    /* Preserve legacy "<IPv4> -mode" syntax. */
    if (count == 2 && strcmp(tokens[1], "-mode") == 0) {
        struct in_addr address;

        if (inet_pton(AF_INET, tokens[0], &address) != 1)
            return CLI_CMD_INVALID;

        printf(
            "\nNetwork diagnostics: %s\n",
            scan_enabled ? "ON" : "OFF"
        );

        return CLI_CMD_CONTINUE;
    }

    return CLI_CMD_INVALID;
}

/* ============================================================
 * PORT CHECK COMMAND
 * ============================================================ */

static cli_cmd_result cmd_scan(char **tokens, size_t count)
{
    if (count != 2 || strcmp(tokens[1], "-Sc") != 0)
        return CLI_CMD_INVALID;

    struct in_addr address;

    if (inet_pton(AF_INET, tokens[0], &address) != 1)
        return CLI_CMD_INVALID;

#ifdef NERO_NETWORK_UNAVAILABLE
    fputs(
        "\n-Sc: unsupported in this build; "
        "no network traffic is generated.\n",
        stderr
    );
#else
    if (!scan_enabled) {
        puts("\nNetwork diagnostics are disabled.");
        return CLI_CMD_CONTINUE;
    }

#ifdef NERO_SAFE_TEST

    /*
     * Offline simulation.
     * No socket is created.
     */
    printf(
        "\n-Sc: simulation for %s; packets sent: 0\n",
        tokens[0]
    );

#else

    int selected_port = portscanner(tokens[0]);

    if (selected_port == PORTSCANNER_CANCELLED) {
        if (cli_termination_signal == SIGINT)
            cli_termination_signal = 0;
        return CLI_CMD_CONTINUE;
    }

    if (selected_port < 0)
        return CLI_CMD_CONTINUE;

    if (selected_port == 0) {
        puts("\nNo open TCP ports found.");
        return CLI_CMD_CONTINUE;
    }

    printf("\nSelected port: %d\n", selected_port);

    tcp_port_result result =
        tcp_port_check(tokens[0], (uint16_t)selected_port);

    printf(
        "Port %d : %s\n",
        selected_port,
        tcp_result_string(result)
    );

#endif

#endif /* NERO_NETWORK_UNAVAILABLE */

    return CLI_CMD_CONTINUE;
}

/* ============================================================
 * SYN LEGACY COMMAND
 * ============================================================ */


static cli_cmd_result cmd_syn(char **tokens, size_t count)
{
    struct in_addr addr;

    /* Validate arguments and command syntax before dispatching. */
    if (tokens == NULL || count != 2 ||
        tokens[0] == NULL || tokens[1] == NULL) {
        return CLI_CMD_INVALID;
    }

    if (strcmp(tokens[1], "-Syn") != 0 ||
        inet_pton(AF_INET, tokens[0], &addr) != 1) {
        return CLI_CMD_INVALID;
    }

#if defined(NERO_SAFE_TEST)

    /* Safe test mode: no real network traffic. */
    cli_show_syn_status();

#elif defined(NERO_NETWORK_UNAVAILABLE)

    /* Network diagnostics are unavailable in this build. */
    fputs(
        "-Syn: unsupported in this build; "
        "no network traffic is generated.\n",
        stderr
    );

#else

    /* SYN flood'u başlat. Port taraması SYN_Flood içinde port_get()
     * üzerinden portscanner() ile yapılır, burada ayrıca çağırmayın. */
    if (SYN_Flood(tokens[0]) != 0) {
        fprintf(stderr, "SYN flood failed for %s\n", tokens[0]);
    }

#endif

    return CLI_CMD_CONTINUE;
}

/* ============================================================
 * HELP
 * ============================================================ */

static cli_cmd_result cmd_help(char **tokens, size_t count)
{
    if (count > 2 ||
        (count == 2 && strcmp(tokens[1], "-All") != 0)) {
        return CLI_CMD_INVALID;
    }

    cli_show_help();

    return CLI_CMD_CONTINUE;
}

/* ============================================================
 * HISTORY
 * ============================================================ */

static cli_cmd_result cmd_history(char **tokens, size_t count)
{
    if (count == 2 && strcmp(tokens[1], "-clear") == 0) {
        memset(history, 0, sizeof(history));
        history_count = 0;

        printf("\nHistory cleared.\n");
    } else if (count == 1) {
        for (int i = 0; i < history_count; ++i)
            printf("\n%s", history[i]);
    } else {
        return CLI_CMD_INVALID;
    }

    return CLI_CMD_CONTINUE;
}

/* ============================================================
 * EXIT
 * ============================================================ */

static cli_cmd_result cmd_exit(char **tokens, size_t count)
{
    if (count > 2 ||
        (count == 2 && strcmp(tokens[1], "-All") != 0)) {
        return CLI_CMD_INVALID;
    }

    if (terminal_color_enabled())
        fputs(EXIT_MESSAGE, stdout);
    else
        puts("\nExiting...");

    if (count == 2)
        clear_screen();

    return CLI_CMD_EXIT;
}

/* ============================================================
 * CLEAR
 * ============================================================ */

static cli_cmd_result cmd_clear(char **tokens, size_t count)
{
    (void)tokens;

    if (count != 1)
        return CLI_CMD_INVALID;

    row_location = 16;
    ascii_function();

    return CLI_CMD_CONTINUE;
}

/* ============================================================
 * VERSION
 * ============================================================ */

static cli_cmd_result cmd_version(char **tokens, size_t count)
{
    (void)tokens;

    if (count != 1)
        return CLI_CMD_INVALID;

    fputs(VERSION_MESSAGE, stdout);

    return CLI_CMD_CONTINUE;
}

/* ============================================================
 * COMMAND REGISTRY
 * ============================================================ */

static const struct {
    const char *name;
    cli_cmd_result (*run)(char **tokens, size_t count);
} CLI_COMMANDS[] = {
#define CLI_COMMAND(name, handler) {#name, handler},
    CLI_KEYWORDS(CLI_COMMAND)
#undef CLI_COMMAND
};

/* ============================================================
 * UNKNOWN COMMAND
 * ============================================================ */

static void cli_report_unknown(void)
{
    if (terminal_color_enabled()) {
        fputs(UNKNOWN_COMMAND_WARNING, stdout);
    } else {
        fputs(
            "\n[WARNING] Unknown command! "
            "Available commands: 'help'",
            stdout
        );
    }
}

/* ============================================================
 * MAIN COMMAND DISPATCHER
 * ============================================================ */

int cli_dispatch_command(const char *command)
{
    if (command == NULL)
        return -1;

    char copy[CLI_LINE_CAPACITY];

    size_t length = strlen(command);

    if (length >= sizeof(copy))
        return -1;

    char *tokens[CLI_MAX_TOKENS];
    size_t count = 0;

    if (nero_parse(
            command,
            sizeof(copy),
            copy,
            sizeof(copy),
            tokens,
            CLI_MAX_TOKENS,
            &count
        ) != NERO_PARSE_OK) {
        goto invalid;
    }

    if (count == 0)
        return 0;

    cli_cmd_result result;

    /* Accept both the documented "<ip> -Syn" order and the common
     * "-Syn <ip>" alias.  The same applies to -Sc. */
    if (count == 2 && strcmp(tokens[0], "-Syn") == 0) {
        char *alias_tokens[2] = {tokens[1], tokens[0]};
        result = cmd_syn(alias_tokens, 2);
    } else if (count == 2 && strcmp(tokens[0], "-Sc") == 0) {
        char *alias_tokens[2] = {tokens[1], tokens[0]};
        result = cmd_scan(alias_tokens, 2);

    /* IPv4 + SYN */
    } else if (count >= 2 &&
        strcmp(tokens[1], "-Syn") == 0) {

        result = cmd_syn(tokens, count);

    /* IPv4 + TCP port check */
    } else if (count >= 2 &&
               strcmp(tokens[1], "-Sc") == 0) {

        result = cmd_scan(tokens, count);

    /* Mode control */
    } else if (strcmp(tokens[0], "-mode") == 0 ||
               (count == 2 &&
                strcmp(tokens[1], "-mode") == 0)) {

        result = cmd_mode(tokens, count);

    /* Standard keyword commands */
    } else {
        const size_t command_count =
            sizeof(CLI_COMMANDS) / sizeof(CLI_COMMANDS[0]);

        size_t i = 0;

        for (; i < command_count; ++i) {
            if (strcmp(
                    tokens[0],
                    CLI_COMMANDS[i].name
                ) == 0) {

                result = CLI_COMMANDS[i].run(
                    tokens,
                    count
                );
                break;
            }
        }

        if (i == command_count) {
            cli_report_unknown();
            return 0;
        }
    }

    if (result == CLI_CMD_INVALID)
        goto invalid;

    return (int)result;

invalid:
    fputs(
        "\nInvalid command parameters. Use 'help'.\n",
        stderr
    );

    return 0;
}
