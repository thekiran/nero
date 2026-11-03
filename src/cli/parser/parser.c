/* Bounded command tokenizer. Token text lives in caller-provided storage;
 * returned argv pointers borrow that buffer and require no individual frees. */
#include "nero/cli/parser.h"
#include <ctype.h>

nero_parse_status nero_parse(const char *input, size_t input_limit,
                             char *storage, size_t storage_capacity,
                             char **argv, size_t argv_capacity, size_t *argc)
{
    /* Separate source and destination offsets allow quotes and escapes to be
     * consumed without allocating temporary strings for each argument. */
    size_t length = 0, source = 0, target = 0, count = 0;
    if (!input || !storage || !argv || !argc || !argv_capacity)
        return NERO_PARSE_INVALID;
    *argc = 0;
    argv[0] = NULL;
    while (length < input_limit && input[length])
        ++length;
    if (length == input_limit || length >= storage_capacity)
        return NERO_PARSE_LIMIT;
    while (source < length) {
        char quote = 0;
        while (source < length && isspace((unsigned char)input[source]))
            ++source;
        if (source == length)
            break;
        if (count == argv_capacity - 1)
            return NERO_PARSE_LIMIT;
        argv[count++] = storage + target;
        while (source < length) {
            char current = input[source++];
            if (!quote && isspace((unsigned char)current))
                break;
            if (current == '\\' && quote != '\'') {
                if (source == length)
                    return NERO_PARSE_SYNTAX;
                storage[target++] = input[source++];
            } else if (current == quote) {
                quote = 0;
            } else if (!quote && (current == '\'' || current == '"')) {
                quote = current;
            } else {
                storage[target++] = current;
            }
        }
        if (quote)
            return NERO_PARSE_SYNTAX;
        storage[target++] = '\0';
    }
    argv[count] = NULL;
    *argc = count;
    return NERO_PARSE_OK;
}
