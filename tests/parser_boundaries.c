/* Exact parser capacities, syntax failures and output ownership; no networking. */
#include "nero/cli/parser.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char storage[256], input[256];
    char *args[65];
    size_t count = 999;
    assert(nero_parse(NULL, 1, storage, sizeof(storage), args, 65, &count) == NERO_PARSE_INVALID);
    assert(nero_parse("", 1, storage, sizeof(storage), args, 0, &count) == NERO_PARSE_INVALID);
    assert(nero_parse("", 1, storage, sizeof(storage), args, 65, &count) == NERO_PARSE_OK);
    assert(count == 0 && args[0] == NULL);
    memset(input, 'x', sizeof(input));
    assert(nero_parse(input, sizeof(input), storage, sizeof(storage), args, 65, &count) == NERO_PARSE_LIMIT);
    input[255] = '\0';
    assert(nero_parse(input, sizeof(input), storage, sizeof(storage), args, 65, &count) == NERO_PARSE_OK);
    assert(count == 1 && strlen(args[0]) == 255 && args[1] == NULL);
    assert(nero_parse(input, sizeof(input), storage, 255, args, 65, &count) == NERO_PARSE_LIMIT);
    for (size_t i = 0; i < 65; ++i) { input[i * 2] = 'x'; input[i * 2 + 1] = ' '; }
    input[127] = '\0';
    assert(nero_parse(input, sizeof(input), storage, sizeof(storage), args, 65, &count) == NERO_PARSE_OK);
    assert(count == 64 && args[64] == NULL);
    input[127] = ' '; input[130] = '\0';
    assert(nero_parse(input, sizeof(input), storage, sizeof(storage), args, 65, &count) == NERO_PARSE_LIMIT);
    assert(count == 0);
    const char *bad[] = {"'unterminated", "\"unterminated", "trailing\\"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(nero_parse(bad[i], 256, storage, sizeof(storage), args, 65, &count) == NERO_PARSE_SYNTAX);
        assert(count == 0);
    }
    assert(nero_parse("'' \"two words\" escaped\\ space", 256, storage, sizeof(storage), args, 65, &count) == NERO_PARSE_OK);
    assert(count == 3 && args[3] == NULL && args[0][0] == '\0');
    assert(strcmp(args[1], "two words") == 0 && strcmp(args[2], "escaped space") == 0);
    assert(args[0] >= storage && args[2] < storage + sizeof(storage));
    puts("PASS parser capacity, malformed syntax and borrowed storage boundaries");
    return 0;
}
