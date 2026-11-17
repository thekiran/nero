#ifndef NERO_SUGGESTION_CATALOG_H
#define NERO_SUGGESTION_CATALOG_H

/*
 * Single source of truth for NERO IntelliSense.
 *
 * Every row is one *executable command form*: the exact whitespace-separated
 * token sequence the dispatcher in commands.c / wifi_command.c accepts. Tokens
 * wrapped in <...> or [...] are placeholders and are NEVER offered as ghost
 * text, inserted by Tab completion, or treated as executable values. The engine
 * (suggestion_engine.c) and inline help (inline_help.c) read only this table, so
 * help/completion can never drift from what the shell actually runs.
 *
 * Each X(form, flags, category, description, example) row provides:
 *   form        - literal template, e.g. "wifi analyze <PCAP>"
 *   flags       - NERO_FORM_* bitmask (platform / build-gating)
 *   category    - short group label used by inline help
 *   description - one-line human description
 *   example     - a concrete, copy-pasteable invocation (literal-only forms
 *                 repeat the form; placeholder forms show a sample value)
 *
 * The head token of every form is either a keyword from CLI_KEYWORDS (see
 * command_catalog.h) or an <IP> placeholder for the legacy positional-target
 * aliases. suggestion_engine self-checks this invariant in debug builds.
 */

/* Build/platform gating. A form is "unsupported on this build" when its
 * required engine is compiled out; the engine reports that honestly rather
 * than pretending the command will run. */
enum {
    NERO_FORM_ALL = 0u,       /* always available on every supported build */
    NERO_FORM_NET = 1u << 0,  /* needs the TCP diagnostics engine (-Sc / scan) */
    NERO_FORM_SYN = 1u << 1   /* needs the legacy -Syn packet engine */
};

#define CLI_COMMAND_FORMS(X)                                                                        \
    /* ---- General ---- */                                                                        \
    X("help", NERO_FORM_ALL, "General", "Show the command summary", "help")                         \
    X("help --all", NERO_FORM_ALL, "General", "Show detailed help and legacy aliases", "help --all")\
    X("help -All", NERO_FORM_ALL, "General", "Detailed help (legacy spelling)", "help -All")        \
    X("version", NERO_FORM_ALL, "General", "Show the program version", "version")                   \
    X("clear", NERO_FORM_ALL, "General", "Clear the screen", "clear")                               \
    X("exit", NERO_FORM_ALL, "General", "Quit the shell", "exit")                                   \
    X("exit -All", NERO_FORM_ALL, "General", "Quit and clear the screen", "exit -All")              \
    /* ---- History ---- */                                                                         \
    X("history", NERO_FORM_ALL, "History", "Show command history", "history")                       \
    X("history clear", NERO_FORM_ALL, "History", "Clear command history", "history clear")          \
    X("history -clear", NERO_FORM_ALL, "History", "Clear command history (legacy)", "history -clear")\
    /* ---- Logging ---- */                                                                         \
    X("logs", NERO_FORM_ALL, "Logging", "Show recent logs", "logs")                                 \
    X("logs errors", NERO_FORM_ALL, "Logging", "Show warnings and errors", "logs errors")           \
    X("logs path", NERO_FORM_ALL, "Logging", "Show the log file location", "logs path")             \
    X("logs clear", NERO_FORM_ALL, "Logging", "Clear stored logs", "logs clear")                    \
    X("log", NERO_FORM_ALL, "Logging", "Show recent logs (legacy)", "log")                          \
    X("log -errors", NERO_FORM_ALL, "Logging", "Show warnings and errors (legacy)", "log -errors")  \
    X("log -path", NERO_FORM_ALL, "Logging", "Show the log file location (legacy)", "log -path")     \
    X("log -clear", NERO_FORM_ALL, "Logging", "Clear stored logs (legacy)", "log -clear")           \
    /* ---- Diagnostics ---- */                                                                     \
    X("doctor", NERO_FORM_ALL, "Diagnostics", "Check the local environment (read-only)", "doctor")  \
    /* ---- Network diagnostics ---- */                                                             \
    X("network status", NERO_FORM_ALL, "Network", "Show the network diagnostics mode", "network status")\
    X("network enable", NERO_FORM_ALL, "Network", "Enable network diagnostics", "network enable")   \
    X("network disable", NERO_FORM_ALL, "Network", "Disable network diagnostics", "network disable")\
    X("-mode", NERO_FORM_ALL, "Network", "Show the network diagnostics mode (legacy)", "-mode")     \
    X("-mode on", NERO_FORM_ALL, "Network", "Enable network diagnostics (legacy)", "-mode on")      \
    X("-mode off", NERO_FORM_ALL, "Network", "Disable network diagnostics (legacy)", "-mode off")   \
    X("scan <IP>", NERO_FORM_NET, "Network", "Run read-only TCP port diagnostics", "scan 192.0.2.1")\
    X("<IP> -Sc", NERO_FORM_NET, "Network", "Run read-only TCP port diagnostics (legacy)", "192.0.2.1 -Sc")\
    /* ---- Wireless ---- */                                                                        \
    X("wifi", NERO_FORM_ALL, "Wireless", "Show the Wi-Fi command summary", "wifi")                  \
    X("wifi help", NERO_FORM_ALL, "Wireless", "Show the Wi-Fi command summary", "wifi help")        \
    X("wifi status", NERO_FORM_ALL, "Wireless", "Show adapter and capture capabilities", "wifi status")\
    X("wifi capabilities", NERO_FORM_ALL, "Wireless", "Show detailed capture capabilities", "wifi capabilities")\
    X("wifi interfaces", NERO_FORM_ALL, "Wireless", "List wireless interfaces", "wifi interfaces")  \
    X("wifi scan", NERO_FORM_ALL, "Wireless", "Run an active access-point survey", "wifi scan")     \
    X("wifi aps", NERO_FORM_ALL, "Wireless", "Show passively observed access points", "wifi aps")   \
    X("wifi clients", NERO_FORM_ALL, "Wireless", "Show observed client devices", "wifi clients")    \
    X("wifi signal", NERO_FORM_ALL, "Wireless", "Show current link signal metrics", "wifi signal")  \
    X("wifi inspect", NERO_FORM_ALL, "Wireless", "Inspect a tracked station in detail", "wifi inspect")\
    X("wifi monitor", NERO_FORM_ALL, "Wireless", "Open the passive monitor dashboard", "wifi monitor")\
    X("wifi export", NERO_FORM_ALL, "Wireless", "Export observed data to a file", "wifi export")    \
    X("wifi clear", NERO_FORM_ALL, "Wireless", "Clear captured Wi-Fi data from memory", "wifi clear")\
    X("wifi analyze <PCAP>", NERO_FORM_ALL, "Wireless", "Analyze a capture file offline", "wifi analyze capture.pcap")\
    /* ---- Laboratory ---- */                                                                      \
    X("preflight", NERO_FORM_SYN, "Laboratory", "Show -Syn resource requirements (read-only)", "preflight")\
    X("<IP> -Syn", NERO_FORM_SYN, "Laboratory", "Run SYN/UDP flood (authorized lab target only)", "192.0.2.1 -Syn")

#endif /* NERO_SUGGESTION_CATALOG_H */
