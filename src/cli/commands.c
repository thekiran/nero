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

#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include "cli_internal.h"
#include "command_catalog.h"
#include "nero/cli/parser.h"
#include "nero/ascii.h"
#include "nero/printing.h"
#include "nero/terminal.h"
#include "nero/network_diagnostics.h"
#include "nero/operation_ui.h"
#include "nero/log.h"
#include "nero/doctor.h"
#include "nero/socket_handle.h"
#ifndef NERO_NETWORK_UNAVAILABLE
#include "nero/portscanner.h"
#endif

#ifndef NERO_NETWORK_UNAVAILABLE
#ifndef NERO_SYN_UNAVAILABLE
#include "nero/syn_flood_powered.h"
#endif
#endif

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>     /* size_t */

#include <time.h>


#define CLI_MAX_TOKENS 64
#define TCP_TIMEOUT_SECONDS 1

/* cli_cmd_result is declared in cli_internal.h so the offline simulation
 * handler (workload.c) shares the dispatcher's return contract. */

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

    nero_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd == NERO_INVALID_SOCKET)
        return TCP_PORT_ERROR;

#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &nonblocking) != 0) {
#else
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
#endif
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

#ifndef _WIN32
    /* Windows selects on SOCKET handles whose values ignore FD_SETSIZE. */
    if (fd >= FD_SETSIZE) {
        close(fd);
        return TCP_PORT_ERROR;
    }
#endif

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(fd, &writefds);

    struct timeval timeout = {
        .tv_sec = TCP_TIMEOUT_SECONDS,
        .tv_usec = 0
    };

    status = select(
#ifdef _WIN32
        0, /* Winsock ignores nfds; never narrow a SOCKET into this parameter. */
#else
        fd + 1,
#endif
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
    nero_log_write(NERO_LOG_WARN, "scanner", "TCP scanner unavailable in this build");
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

    nero_log_write(result == TCP_PORT_ERROR ? NERO_LOG_ERROR :
                   (result == TCP_PORT_TIMEOUT ? NERO_LOG_WARN : NERO_LOG_INFO),
                   "tcp", "target=%s port=%d result=%s", tokens[0], selected_port, tcp_result_string(result));
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

    /* -mode off blocks every network operation at dispatch, consistently with
     * -Sc. This gates the command handler only; the legacy transmission engine
     * is never entered and is not modified. */
    if (!scan_enabled) {
        puts("\nNetwork diagnostics are disabled.");
        return CLI_CMD_CONTINUE;
    }

#if defined(NERO_SAFE_TEST)

    /* Safe test mode: no real network traffic. */
    cli_show_syn_status();

#elif defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SYN_UNAVAILABLE)

    /* Network diagnostics (raw packet engine) are unavailable in this build.
     * Windows: the OS blocks raw TCP packet injection for desktop apps. */
    fputs(
        "-Syn: unsupported in this build; "
        "no network traffic is generated.\n",
        stderr
    );

#else

    /* SYN flood'u başlat. Port taraması SYN_Flood içinde port_get()
     * üzerinden portscanner() ile yapılır, burada ayrıca çağırmayın. */
    if (SYN_Flood(tokens[0]) != 0) {
        nero_log_write(NERO_LOG_ERROR, "cli", "Legacy network operation failed: target=%s", tokens[0]);
        fprintf(stderr, "SYN flood failed for %s\n", tokens[0]);
    }

#endif

    /* Ctrl+C during the -Syn settings prompt is a Cancel, not a session
     * abort; consume the interrupt flag like the scanner and preflight
     * commands so the shell keeps running. */
    if (cli_termination_signal == SIGINT)
        cli_termination_signal = 0;

    return CLI_CMD_CONTINUE;
}

/* ============================================================
 * HELP
 * ============================================================ */

static cli_cmd_result cmd_help(char **tokens, size_t count)
{
    if (count > 2 ||
        (count == 2 && strcmp(tokens[1], "-All") != 0 && strcmp(tokens[1], "--all") != 0)) {
        return CLI_CMD_INVALID;
    }

    cli_show_help();
    if (count == 2) cli_show_help_details();

    return CLI_CMD_CONTINUE;
}

/* ============================================================
 * HISTORY
 * ============================================================ */

static cli_cmd_result cmd_history(char **tokens, size_t count)
{
    if (count == 2 && (strcmp(tokens[1], "-clear") == 0 || strcmp(tokens[1], "clear") == 0)) {
        if (cli_history_clear() == 0)
            printf("\nHistory cleared.\n");
        else
            fputs("\nSession history cleared; persistent history could not be cleared. See log -errors.\n", stderr);
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

static cli_cmd_result cmd_doctor(char **tokens, size_t count)
{
    (void)tokens;
    if (count != 1) return CLI_CMD_INVALID;
    return nero_doctor(stdout) ? CLI_CMD_FAILURE : CLI_CMD_CONTINUE;
}

static cli_cmd_result cmd_version(char **tokens, size_t count)
{
    (void)tokens;

    if (count != 1)
        return CLI_CMD_INVALID;

    fputs(VERSION_MESSAGE, stdout);

    return CLI_CMD_CONTINUE;
}

/* Logs are independent of shell history and survive session restart when
 * the per-user log file can be written. Never log log-reads themselves. */
static cli_cmd_result cmd_log(char **tokens, size_t count)
{
    if (count > 2 || (count == 2 && strcmp(tokens[1], "-errors") != 0 &&
        strcmp(tokens[1], "-path") != 0 && strcmp(tokens[1], "-clear") != 0))
        return CLI_CMD_INVALID;
    putchar('\n');
    if (count == 1) nero_log_dump(stdout, 0);
    else if (count == 2 && strcmp(tokens[1], "-errors") == 0) nero_log_dump(stdout, 1);
    else if (count == 2 && strcmp(tokens[1], "-path") == 0) nero_log_show_path(stdout);
    else if (count == 2 && strcmp(tokens[1], "-clear") == 0) {
        if (nero_log_clear() == 0) puts("Logs cleared.");
        else fprintf(stderr, "Could not clear persisted logs: %s\n", strerror(errno));
    } else return CLI_CMD_INVALID;
    return CLI_CMD_CONTINUE;
}

/* Readable aliases reuse the existing handlers and their validation/gates.
 * No scanner, selection, settings or transmission path is duplicated here. */
static cli_cmd_result cmd_logs(char **tokens, size_t count)
{
    if (count == 1) return cmd_log(tokens, count);
    if (count != 2) return CLI_CMD_INVALID;
    char *option;
    if (strcmp(tokens[1], "errors") == 0) option = "-errors";
    else if (strcmp(tokens[1], "path") == 0) option = "-path";
    else if (strcmp(tokens[1], "clear") == 0) option = "-clear";
    else return CLI_CMD_INVALID;
    char *legacy[] = {"log", option};
    return cmd_log(legacy, 2);
}

static cli_cmd_result cmd_network(char **tokens, size_t count)
{
    if (count != 2) return CLI_CMD_INVALID;
    char *legacy[] = {"-mode", NULL};
    if (strcmp(tokens[1], "status") == 0) return cmd_mode(legacy, 1);
    if (strcmp(tokens[1], "enable") == 0) legacy[1] = "on";
    else if (strcmp(tokens[1], "disable") == 0) legacy[1] = "off";
    else return CLI_CMD_INVALID;
    return cmd_mode(legacy, 2);
}

static cli_cmd_result cmd_scan_alias(char **tokens, size_t count)
{
    if (count != 2) return CLI_CMD_INVALID;
    char *legacy[] = {tokens[1], "-Sc"};
    return cmd_scan(legacy, 2);
}

static cli_cmd_result cmd_syn_alias(char **tokens, size_t count)
{
    if (count != 2) return CLI_CMD_INVALID;
    if (strcmp(tokens[1], "preflight") == 0) return cmd_preflight(tokens + 1, 1);
    char *legacy[] = {tokens[1], "-Syn"};
    return cmd_syn(legacy, 2);
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
    nero_log_write(NERO_LOG_WARN, "cli", "Unknown command rejected (input omitted)");
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
    if (command == NULL) {
        nero_log_write(NERO_LOG_ERROR, "cli", "Null command input");
        return -1;
    }

    char copy[CLI_LINE_CAPACITY];

    size_t length = strlen(command);

    if (length >= sizeof(copy))
        return -1;

    char *tokens[CLI_MAX_TOKENS + 1];
    size_t count = 0;

    if (nero_parse(
            command,
            sizeof(copy),
            copy,
            sizeof(copy),
            tokens,
            CLI_MAX_TOKENS + 1,
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
    nero_log_write(NERO_LOG_WARN, "cli", "Invalid command parameters (input omitted)");
    fputs(
        "\nInvalid command parameters. Use 'help'.\n",
        stderr
    );

    return 0;
}
