/* Real record importer, severity, filtering and JSON escaping. No initialize(),
 * persistent writer, shell, scanner or raw engine can be called by this target. */
#include "../../src/core/log.c"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 131072) return 0;
    FILE *file = tmpfile();
    if (!file) abort();
    if (fwrite(data, 1, size, file) != size) abort();
    head = used = 0; memset(&counters, 0, sizeof(counters));
    import_recent(file);
    fclose(file);
    if (used > LOG_CAPACITY) abort();
    FILE *output = tmpfile();
    if (!output) abort();
    for (size_t i = 0; i < used; ++i) {
        const log_entry *entry = &entries[(head + i) % LOG_CAPACITY];
        const char *ts, *comp, *msg; size_t tsn, compn;
        split_record(entry->text, &ts, &tsn, &comp, &compn, &msg);
        if (entry->level > NERO_LOG_FATAL) abort();
        json_escape(output, ts, tsn); json_escape(output, comp, compn);
        json_escape(output, msg, strlen(msg));
        nero_log_query q = {0}; q.component = "parser";
        (void)record_matches(entry, &q);
    }
    fclose(output);
    return 0;
}
