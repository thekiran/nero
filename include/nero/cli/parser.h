#ifndef NERO_CLI_PARSER_H
#define NERO_CLI_PARSER_H
#include <stddef.h>

typedef enum {
    NERO_PARSE_OK,
    NERO_PARSE_LIMIT,
    NERO_PARSE_SYNTAX,
    NERO_PARSE_INVALID
} nero_parse_status;

nero_parse_status nero_parse(const char *input, size_t input_limit,
                             char *storage, size_t storage_capacity,
                             char **argv, size_t argv_capacity, size_t *argc);
#endif
