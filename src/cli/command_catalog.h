#ifndef NERO_COMMAND_CATALOG_H
#define NERO_COMMAND_CATALOG_H

/* X-macro catalogs keep command discovery and help/completion consumers on
 * shared definitions. Each consumer supplies its own expansion macro. */
#define CLI_KEYWORDS(X) \
    X(help, cmd_help) \
    X(history, cmd_history) \
    X(exit, cmd_exit) \
    X(clear, cmd_clear) \
    X(doctor, cmd_doctor) \
    X(version, cmd_version) \
    X(log, cmd_log) \
    X(logs, cmd_logs) \
    X(network, cmd_network) \
    X(scan, cmd_scan_alias) \
    X(syn, cmd_syn_alias) \
    X(preflight, cmd_preflight)

/* Grouped help also feeds completion; legacy entries stay available below. */
#define CLI_GROUPED_HELP(X) \
    X("GENERAL", "help", "Show commands") \
    X("GENERAL", "help --all", "Show detailed help") \
    X("GENERAL", "version", "Show version") \
    X("GENERAL", "clear", "Clear terminal") \
    X("GENERAL", "exit", "Exit NERO") \
    X("HISTORY", "history", "Show command history") \
    X("HISTORY", "history clear", "Clear command history") \
    X("LOGGING", "logs", "Show recent logs") \
    X("LOGGING", "logs errors", "Show errors and warnings") \
    X("LOGGING", "logs path", "Show log file location") \
    X("LOGGING", "logs clear", "Clear stored logs") \
    X("DIAGNOSTICS", "doctor", "Check local environment") \
    X("NETWORK", "network status", "Show network mode") \
    X("NETWORK", "network enable", "Enable diagnostics") \
    X("NETWORK", "network disable", "Disable diagnostics") \
    X("NETWORK", "scan <IP>", "Run TCP port diagnostics") \
    X("LABORATORY", "syn preflight", "Show resource requirements") \
    X("LABORATORY", "syn <IP>", "Run authorized laboratory operation")

#define CLI_HELP_ENTRIES(X) \
    X("<network IP> -Syn", ": Run SYN/UDP packet flood (authorized targets)") \
    X("-Syn <network IP>", ": Alias for the SYN/UDP packet flood") \
    X("help", ": Show this help message") \
    X("help -All", ": Show this help message") \
    X("exit", ": Quit the shell") \
    X("clear", ": Clear the screen") \
    X("history", ": Show command history") \
    X("doctor", ": Read-only offline diagnostics (private data redacted)") \
    X("version", ": Show program version") \
    X("preflight", ": Read-only -Syn preflight: expected resource use, no network I/O") \
    X("log", ": Show recent application logs") \
    X("log -errors", ": Show warnings and errors") \
    X("log -path", ": Show log file location") \
    X("log -clear", ": Clear log history") \
    X("history -clear", ": Clear command history") \
    X("exit -All", ": Quit the shell and clear the screen") \
    X("<target> -Sc", ": Check TCP ports (offline builds simulate)") \
    X("-mode", ": Show network diagnostics setting") \
    X("<target> -mode", ": Show network diagnostics setting") \
    X("-mode on", ": Enable network diagnostics") \
    X("-mode off", ": Disable network diagnostics")

#endif
