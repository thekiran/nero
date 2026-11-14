/* Deterministic tests for the shared terminal table engine (nero/table.h).
 *
 * These exercise display-width measurement, UTF-8-aware fitting, the signal
 * level thresholds and the responsive grid (growth, shrink, column drop and the
 * honest narrow-view note) by rendering into an in-memory stream. No terminal
 * or capture device is touched. */

#include "nero/table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The engine reads these capability gates only through nero_table_opts_default;
 * every test passes explicit opts, but the symbols must still resolve at link
 * time, so the test provides its own standalone definitions. */
int terminal_color_enabled(void) { return 0; }
int terminal_unicode_enabled(void) { return 0; }

static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static void test_display_width(void)
{
    CHECK(nero_display_width("hello") == 5);
    CHECK(nero_display_width("") == 0);
    /* CJK ideograph occupies two cells. */
    CHECK(nero_display_width("\xE4\xBD\xA0") == 2);
    /* "a" + combining acute accent (U+0301) stays one cell. */
    CHECK(nero_display_width("a\xCC\x81") == 1);
    /* Emoji is wide. */
    CHECK(nero_display_width("\xF0\x9F\x93\xB6") == 2);
}

static void test_fit(void)
{
    char buf[64];
    int used = nero_fit("abcdefgh", 5, 1, buf, sizeof(buf));
    CHECK(used == 5);
    /* ASCII mode clips with '.' and keeps four content cells + ellipsis. */
    CHECK(strcmp(buf, "abcd.") == 0);

    used = nero_fit("abc", 10, 1, buf, sizeof(buf));
    CHECK(used == 3);
    CHECK(strcmp(buf, "abc") == 0);

    /* Unicode mode uses the U+2026 ellipsis. */
    used = nero_fit("abcdefgh", 5, 0, buf, sizeof(buf));
    CHECK(used == 5);
    CHECK(strstr(buf, "\xE2\x80\xA6") != NULL);
}

static void test_signal_level(void)
{
    const char *color = NULL;
    CHECK(nero_signal_level(0, -40, &color) == 0);
    CHECK(nero_signal_level(1, -40, &color) == 4);
    CHECK(nero_signal_level(1, -60, &color) == 3);
    CHECK(nero_signal_level(1, -70, &color) == 2);
    CHECK(nero_signal_level(1, -90, &color) == 1);
}

typedef struct {
    const char *a;
    const char *b;
} row2;

static void row2_build(int index, void *ctx, nero_table_cell *out)
{
    const row2 *rows = ctx;
    snprintf(out[0].text, sizeof(out[0].text), "%s", rows[index].a);
    snprintf(out[1].text, sizeof(out[1].text), "%s", rows[index].b);
}

/* Render helper: returns a heap string the caller frees. */
static char *render(const nero_table_col *cols, int ncols, int nrows, nero_table_row_fn fn,
                    void *ctx, int width)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *ms = open_memstream(&buf, &len);
    if (ms == NULL)
        return NULL;
    nero_table_opts opts = {0};
    opts.color = 0;
    opts.unicode = 0;
    opts.width = width;
    opts.max_width = width;
    nero_table_draw(ms, &opts, cols, ncols, nrows, fn, ctx);
    fclose(ms);
    return buf;
}

static int max_line_width(const char *text)
{
    int max = 0, cur = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') {
            if (cur > max)
                max = cur;
            cur = 0;
        } else {
            ++cur;
        }
    }
    if (cur > max)
        max = cur;
    return max;
}

static void test_table_render_ascii(void)
{
    static const nero_table_col cols[] = {
        {"NAME", 6, 10, 1, 0, 0, 8, NERO_TABLE_TEXT},
        {"VALUE", 4, 8, 0, 0, 0, 0, NERO_TABLE_TEXT},
    };
    row2 rows[] = {{"alpha", "1"}, {"beta", "22"}};
    char *out = render(cols, 2, 2, row2_build, rows, 40);
    CHECK(out != NULL);
    if (out != NULL) {
        CHECK(strstr(out, "NAME") != NULL);
        CHECK(strstr(out, "VALUE") != NULL);
        CHECK(strstr(out, "alpha") != NULL);
        CHECK(strstr(out, "beta") != NULL);
        /* ASCII fallback uses +,-,| and no box-drawing bytes. */
        CHECK(strstr(out, "+") != NULL);
        CHECK(strstr(out, "\xE2\x94") == NULL);
        /* No line exceeds the requested width. */
        CHECK(max_line_width(out) <= 40);
        free(out);
    }
}

static void test_table_drops_low_priority_column(void)
{
    /* NAME never drops (prio 0); DROPME is droppable (prio 1). At a narrow width
     * the engine hides DROPME and prints an honest narrow-view note. */
    static const nero_table_col cols[] = {
        {"NAME", 8, 20, 1, 0, 0, 20, NERO_TABLE_TEXT},
        {"DROPME", 8, 10, 0, 0, 1, 0, NERO_TABLE_TEXT},
    };
    row2 rows[] = {{"keepthis", "secondary"}};
    char *out = render(cols, 2, 1, row2_build, rows, 18);
    CHECK(out != NULL);
    if (out != NULL) {
        CHECK(strstr(out, "NAME") != NULL);
        /* The dropped column survives only in the honest narrow-view note, not
         * as a rendered header/data cell. */
        CHECK(strstr(out, "narrow view") != NULL);
        CHECK(strstr(out, "DROPME hidden") != NULL);
        CHECK(strstr(out, "secondary") == NULL);
        CHECK(max_line_width(out) <= 18);
        free(out);
    }
}

static void test_table_deterministic(void)
{
    static const nero_table_col cols[] = {
        {"A", 3, 6, 1, 0, 0, 6, NERO_TABLE_TEXT},
        {"B", 3, 6, 0, 1, 0, 0, NERO_TABLE_TEXT},
    };
    row2 rows[] = {{"x", "y"}};
    char *a = render(cols, 2, 1, row2_build, rows, 30);
    char *b = render(cols, 2, 1, row2_build, rows, 30);
    CHECK(a != NULL && b != NULL);
    if (a != NULL && b != NULL)
        CHECK(strcmp(a, b) == 0); /* identical input renders identical frames */
    free(a);
    free(b);
}

int main(void)
{
    test_display_width();
    test_fit();
    test_signal_level();
    test_table_render_ascii();
    test_table_drops_low_priority_column();
    test_table_deterministic();

    if (failures != 0) {
        fprintf(stderr, "%d table-engine test(s) failed\n", failures);
        return 1;
    }
    printf("table-engine: all tests passed\n");
    return 0;
}
