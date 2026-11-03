#ifndef NERO_COMMAND_CATALOG_H
#define NERO_COMMAND_CATALOG_H

#define CLI_KEYWORDS(X) \
    X(help, cmd_help) \
    X(history, cmd_history) \
    X(exit, cmd_exit) \
    X(clear, cmd_clear) \
    X(version, cmd_version)

#define CLI_HELP_ENTRIES(X) \
    X("<network IP> -Syn", ": Network operation (unavailable in macOS/offline builds)") \
    X("help", ": Show this help message") \
    X("help -All", ": Show this help message") \
    X("exit", ": Quit the shell") \
    X("clear", ": Clear the screen") \
    X("history", ": Show command history") \
    X("version", ": Show program version") \
    X("history -clear", ": Clear command history") \
    X("exit -All", ": Quit the shell and clear the screen") \
    X("<target> -Sc", ": Check TCP ports (offline builds simulate)") \
    X("-mode", ": Show network diagnostics setting") \
    X("<target> -mode", ": Show network diagnostics setting") \
    X("-mode on", ": Enable network diagnostics") \
    X("-mode off", ": Disable network diagnostics")

#endif
