/* Deterministic fallback runner, not a substitute for coverage-guided fuzzing. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    for (int i = 1; i < argc; ++i) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) return 2;
        uint8_t *data = malloc(131073);
        if (!data) abort();
        size_t n = fread(data, 1, 131073, f);
        if (ferror(f) || n > 131072) return 2;
        fclose(f);
        LLVMFuzzerTestOneInput(data, n);
        free(data);
    }
    return 0;
}
