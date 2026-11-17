#ifndef NERO_SUGGESTION_ENGINE_H
#define NERO_SUGGESTION_ENGINE_H

/*
 * Pure, deterministic suggestion engine for NERO IntelliSense.
 *
 * Every function here is side-effect free: no terminal I/O, no network, no
 * command execution, no global mutable state. Given the same inputs it always
 * returns the same results, which makes it directly unit-testable in isolation
 * (the engine links against nothing but this translation unit). Rendering,
 * key handling and configuration live elsewhere (render.c / input.c).
 *
 * The registry is suggestion_catalog.h (the X-macro command-form table). The
 * engine never parses human-readable help text and never fabricates argument
 * values for placeholder tokens such as <IP> or <PCAP>.
 */

#include <stddef.h>

/* A full suggested command line plus its catalog description. */
#define NERO_SUGGEST_TEXT_MAX 256
#define NERO_SUGGEST_MAX_ITEMS 24

typedef struct {
    char text[NERO_SUGGEST_TEXT_MAX]; /* insertable completion (literal-only) */
    const char *display;              /* readable form text (static), or NULL */
    const char *description;          /* catalog description, or "" */
    int from_history;                 /* 1 = sourced from session history */
} nero_suggestion;

typedef struct {
    nero_suggestion items[NERO_SUGGEST_MAX_ITEMS];
    size_t count;
    int truncated; /* 1 = more matches existed than were stored */
} nero_suggestion_list;

/* Validation verdict for a complete buffer. No side effects, no dispatch. */
typedef enum {
    NERO_VALIDATE_EMPTY,       /* nothing typed yet */
    NERO_VALIDATE_INCOMPLETE,  /* a strict prefix of a valid form */
    NERO_VALIDATE_VALID,       /* matches a runnable form on this build */
    NERO_VALIDATE_UNSUPPORTED, /* a valid form whose engine is absent here */
    NERO_VALIDATE_INVALID      /* does not match any form */
} nero_validate_result;

/* Outcome of a Tab completion request. */
typedef enum {
    NERO_TAB_NONE,      /* nothing matches; leave the buffer unchanged */
    NERO_TAB_COMPLETED, /* buffer was extended to a unique / common prefix */
    NERO_TAB_AMBIGUOUS  /* multiple matches remain; show a menu */
} nero_tab_result;

/*
 * Ghost text: the inline continuation to display after `buffer` when the
 * cursor is at the end. Writes the suffix (not including `buffer`) into `out`
 * and returns its length. Returns 0 and out[0]=0 when there is no suggestion.
 *
 * Priority: a unique catalog continuation of the current token/form wins; if
 * the catalog is ambiguous or silent, the most recent history entry that has
 * `buffer` as a prefix is used. Placeholder tokens are never ghosted.
 *
 * `history` is an array of `history_count` NUL-terminated lines, oldest first
 * (the in-process session history; nothing is read from disk here).
 */
size_t nero_suggest_ghost(const char *buffer, const char *const *history, size_t history_count,
                          char *out, size_t out_size);

/*
 * Tab completion. On NERO_TAB_COMPLETED, `buffer` (capacity `buffer_size`) is
 * rewritten in place to the extended text. On NERO_TAB_AMBIGUOUS, `buffer` is
 * left unchanged and `list` is filled with the candidate command lines. A
 * literal space is never inserted as a fallback.
 */
nero_tab_result nero_suggest_complete(char *buffer, size_t buffer_size, nero_suggestion_list *list);

/* Fill `list` with catalog forms that have `buffer` as a (token-aware) prefix,
 * ranked best-first. Safe on any input; never executes anything. */
void nero_suggest_matches(const char *buffer, nero_suggestion_list *list);

/* Validate a complete buffer with zero side effects. */
nero_validate_result nero_suggest_validate(const char *buffer);

/*
 * Typo correction, offered as advice only (never auto-applied). If the first
 * token of `buffer` is not a known command but is within a small bounded edit
 * distance of exactly one plausible command keyword, that keyword is written
 * to `out` and 1 is returned; otherwise 0. Honors word length to avoid noisy
 * corrections on very short tokens.
 */
int nero_suggest_typo(const char *buffer, char *out, size_t out_size);

/* Bounded Levenshtein distance between two NUL-terminated strings, capped at
 * `max + 1` (so callers can early-out). Exposed for testing. */
size_t nero_edit_distance(const char *a, const char *b, size_t max);

#endif /* NERO_SUGGESTION_ENGINE_H */
