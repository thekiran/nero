#ifndef NERO_CLI_PARSER_H
#define NERO_CLI_PARSER_H
#include <stddef.h>

typedef enum {
    NERO_PARSE_OK,
    NERO_PARSE_LIMIT,
    NERO_PARSE_SYNTAX,
    NERO_PARSE_INVALID
} nero_parse_status;

/* input_limit bounds the search for the terminating NUL. storage receives
 * unquoted token text; argv borrows pointers into it and reserves its final
 * slot for NULL. Only NERO_PARSE_OK exposes a complete argument list. */
nero_parse_status nero_parse(const char *input, size_t input_limit,
                             char *storage, size_t storage_capacity,
                             char **argv, size_t argv_capacity, size_t *argc);
#endif
