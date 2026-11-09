/* Links only the production tokenizer; command dispatch is never linked. */
#include "nero/cli/parser.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;
    char *input = malloc(size + 1);
    if (!input) abort();
    memcpy(input, data, size); input[size] = 0;
    char storage[256]; char *argv[65]; size_t argc = 0;
    size_t cap = size ? 1 + data[0] % 65 : 65;
    nero_parse_status status = nero_parse(input, size + 1, storage,
                                          sizeof(storage), argv, cap, &argc);
    if (status == NERO_PARSE_OK) {
        if (argc >= cap || argv[argc] != NULL) abort();
        for (size_t i = 0; i < argc; ++i) {
            if (argv[i] < storage || argv[i] >= storage + sizeof(storage) ||
                !memchr(argv[i], 0, sizeof(storage) - (size_t)(argv[i] - storage))) abort();
        }
    }
    /* Also exercise bounded, non-NUL input and short output storage. */
    (void)nero_parse(input, size, storage, size % sizeof(storage), argv, 65, &argc);
    free(input);
    return 0;
}
