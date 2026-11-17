#ifndef NERO_INLINE_HELP_H
#define NERO_INLINE_HELP_H

/*
 * Live contextual help for the command the user is currently typing.
 *
 * Reads only the IntelliSense catalog (suggestion_catalog.h) via the pure
 * lookup below; rendering is capability-gated and performed by the caller's
 * chosen stream. No command is ever executed to produce help.
 */
#include "suggestion_engine.h"
#include <stddef.h>
#include <stdio.h>

typedef struct {
    const char *head;        /* matched command keyword, or NULL */
    const char *category;    /* catalog group label */
    const char *syntax;      /* canonical form text, e.g. "wifi analyze <PCAP>" */
    const char *description; /* one-line description */
    const char *example;     /* copy-pasteable example */
    nero_validate_result status;
} nero_help_entry;

/* Fill `entry` with the catalog row that best describes `buffer`'s command
 * context. Returns 1 on a match, 0 otherwise (entry zeroed). Pure. */
int nero_inline_help_lookup(const char *buffer, nero_help_entry *entry);

/* List related forms sharing the current command's head keyword (its valid
 * options/subcommands), best-first. Pure. */
void nero_inline_help_options(const char *buffer, nero_suggestion_list *out);

/*
 * Render a help block for `buffer` to `out`. `full` selects the detailed view
 * (syntax, options, example); otherwise a single compact line is written.
 * `color` and `unicode` gate ANSI styling and box characters respectively.
 * Writes nothing when there is no matching command context.
 */
void nero_inline_help_render(FILE *out, const char *buffer, int full, int color, int unicode);

#endif /* NERO_INLINE_HELP_H */
