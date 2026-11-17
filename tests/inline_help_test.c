/*
 * Deterministic unit tests for contextual inline help. Links only
 * inline_help.c + suggestion_engine.c; no terminal or command execution.
 */
#include "inline_help.h"
#include "suggestion_engine.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static void test_lookup(void)
{
    nero_help_entry entry;

    CHECK(nero_inline_help_lookup("wifi", &entry) == 1);
    CHECK(strcmp(entry.head, "wifi") == 0);
    CHECK(entry.syntax != NULL && entry.description != NULL && entry.example != NULL);
    CHECK(strcmp(entry.category, "Wireless") == 0);

    /* Longest-prefix: the specific subcommand, not just the base. */
    CHECK(nero_inline_help_lookup("wifi analyze", &entry) == 1);
    CHECK(strcmp(entry.syntax, "wifi analyze <PCAP>") == 0);
    CHECK(strcmp(entry.head, "wifi") == 0);

    CHECK(nero_inline_help_lookup("network status", &entry) == 1);
    CHECK(entry.status == NERO_VALIDATE_VALID);
    CHECK(strcmp(entry.category, "Network") == 0);

    /* Positional alias head. */
    CHECK(nero_inline_help_lookup("192.0.2.1 -Syn", &entry) == 1);
    CHECK(strcmp(entry.category, "Laboratory") == 0);

    /* Unknown input yields no help. */
    CHECK(nero_inline_help_lookup("zzz", &entry) == 0);
    CHECK(nero_inline_help_lookup("", &entry) == 0);
}

static void test_options(void)
{
    nero_suggestion_list options;
    nero_inline_help_options("wifi", &options);
    /* All wifi forms are enumerated as valid options. */
    CHECK(options.count > 5);
    int saw_analyze = 0, saw_status = 0;
    for (size_t i = 0; i < options.count; ++i) {
        CHECK(options.items[i].display != NULL);
        CHECK(options.items[i].description != NULL);
        if (strcmp(options.items[i].display, "wifi analyze <PCAP>") == 0)
            saw_analyze = 1;
        if (strcmp(options.items[i].display, "wifi status") == 0)
            saw_status = 1;
    }
    CHECK(saw_analyze && saw_status);
}

static void test_render_smoke(void)
{
    /* Standard C update stream: supported by POSIX and native MinGW alike.
     * Failure to capture output must fail the test, never silently skip it. */
    char buf[4096];
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream == NULL)
        return;

    nero_inline_help_render(stream, "wifi analyze", 1, 0, 0);
    CHECK(fflush(stream) == 0);
    CHECK(fseek(stream, 0, SEEK_SET) == 0);
    size_t n = fread(buf, 1, sizeof(buf) - 1, stream);
    CHECK(!ferror(stream));
    CHECK(n > 0);
    /* Detect truncation rather than accepting only a matching output prefix. */
    CHECK(fgetc(stream) == EOF);
    CHECK(!ferror(stream));
    buf[n] = '\0';
    CHECK(strstr(buf, "wifi analyze <PCAP>") != NULL);
    CHECK(strstr(buf, "example:") != NULL);
    CHECK(fclose(stream) == 0);
}

int main(void)
{
    test_lookup();
    test_options();
    test_render_smoke();
    if (failures == 0)
        puts("inline-help: all checks passed");
    return failures == 0 ? 0 : 1;
}
