#ifndef NERO_COMMAND_CATALOG_H
#define NERO_COMMAND_CATALOG_H

/* X-macro catalogs keep command discovery and help/completion consumers on
 * shared definitions. Each consumer supplies its own expansion macro. */
#define CLI_KEYWORDS(X) \
    X(help, cmd_help) \
    X(history, cmd_history) \
    X(exit, cmd_exit) \
    X(clear, cmd_clear) \
    X(version, cmd_version) \
    X(log, cmd_log) \
    X(sim, cmd_sim) \
    X(preflight, cmd_preflight)

#define CLI_HELP_ENTRIES(X) \
    X("<network IP> -Syn", ": Run SYN/UDP packet flood (authorized targets)") \
    X("-Syn <network IP>", ": Alias for the SYN/UDP packet flood") \
    X("help", ": Show this help message") \
    X("help -All", ": Show this help message") \
    X("exit", ": Quit the shell") \
    X("clear", ": Clear the screen") \
    X("history", ": Show command history") \
    X("version", ": Show program version") \
    X("sim", ": Offline workload profile (OFFLINE SIMULATION, no network I/O)") \
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
