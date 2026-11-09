/* Include the real private reader, without linking input/dispatch/network. */
#include "../../src/cli/history.c"
#include <stdarg.h>
char history[CLI_HISTORY_CAPACITY][CLI_LINE_CAPACITY];
int history_count;
void nero_log_write(nero_log_level level, const char *component, const char *format, ...)
{ (void)level; (void)component; (void)format; abort(); }
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 131072) return 0;
    FILE *file = tmpfile();
    if (!file) abort();
    if (fwrite(data, 1, size, file) != size) abort();
    char entries[CLI_HISTORY_CAPACITY][CLI_LINE_CAPACITY]; int count;
    if (read_entries(file, entries, &count) != 0) abort();
    if (count < 0 || count > CLI_HISTORY_CAPACITY) abort();
    for (int i = 0; i < count; ++i)
        if (!memchr(entries[i], 0, CLI_LINE_CAPACITY) || !persistable(entries[i])) abort();
    fclose(file);
    return 0;
}
