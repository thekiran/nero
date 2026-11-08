/* Exact parser capacities, syntax failures and output ownership; no networking. */
#include "nero/cli/parser.h"
#include <stdio.h>
#include <string.h>

/* Keep checks active in release configurations as well as Debug builds. */
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Parser check failed at line %d: %s\n", __LINE__, #condition); \
    return 1; } } while (0)

int main(void)
{
    char storage[256], input[256];
    char *args[65];
    size_t count = 999;
    CHECK(nero_parse(NULL, 1, storage, sizeof(storage), args, 65, &count) == NERO_PARSE_INVALID);
    CHECK(nero_parse("", 1, storage, sizeof(storage), args, 0, &count) == NERO_PARSE_INVALID);
    CHECK(nero_parse("", 1, storage, sizeof(storage), args, 65, &count) == NERO_PARSE_OK);
    CHECK(count == 0 && args[0] == NULL);
    memset(input, 'x', sizeof(input));
    CHECK(nero_parse(input, sizeof(input), storage, sizeof(storage), args, 65, &count) == NERO_PARSE_LIMIT);
    input[255] = '\0';
    CHECK(nero_parse(input, sizeof(input), storage, sizeof(storage), args, 65, &count) == NERO_PARSE_OK);
    CHECK(args[0] != NULL);
    CHECK(count == 1 && strlen(args[0]) == 255 && args[1] == NULL);
    CHECK(nero_parse(input, sizeof(input), storage, 255, args, 65, &count) == NERO_PARSE_LIMIT);
    for (size_t i = 0; i < 65; ++i) { input[i * 2] = 'x'; input[i * 2 + 1] = ' '; }
    input[127] = '\0';
    CHECK(nero_parse(input, sizeof(input), storage, sizeof(storage), args, 65, &count) == NERO_PARSE_OK);
    CHECK(count == 64 && args[64] == NULL);
    input[127] = ' '; input[130] = '\0';
    CHECK(nero_parse(input, sizeof(input), storage, sizeof(storage), args, 65, &count) == NERO_PARSE_LIMIT);
    CHECK(count == 0);
    const char *bad[] = {"'unterminated", "\"unterminated", "trailing\\"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(nero_parse(bad[i], 256, storage, sizeof(storage), args, 65, &count) == NERO_PARSE_SYNTAX);
        CHECK(count == 0);
    }
    CHECK(nero_parse("'' \"two words\" escaped\\ space", 256, storage, sizeof(storage), args, 65, &count) == NERO_PARSE_OK);
    CHECK(args[0] != NULL && args[1] != NULL && args[2] != NULL);
    CHECK(count == 3 && args[3] == NULL && args[0][0] == '\0');
    CHECK(strcmp(args[1], "two words") == 0 && strcmp(args[2], "escaped space") == 0);
    CHECK(args[0] >= storage && args[2] < storage + sizeof(storage));
    puts("PASS parser capacity, malformed syntax and borrowed storage boundaries");
    return 0;
}
