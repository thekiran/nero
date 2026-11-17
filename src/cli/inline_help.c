/*
 * Live contextual help for NERO IntelliSense. Pure lookup over the command
 * catalog plus a capability-gated renderer. No command execution, no network.
 */
#include "inline_help.h"
#include "suggestion_catalog.h"

#include <string.h>

#define HELP_MAX_TOKENS 8
#define HELP_TOKEN_LEN 32

typedef struct {
    char tokens[HELP_MAX_TOKENS][HELP_TOKEN_LEN];
    size_t count;
} help_tokens;

static void split(const char *s, help_tokens *out)
{
    out->count = 0;
    size_t i = 0;
    while (s[i] != '\0' && out->count < HELP_MAX_TOKENS) {
        while (s[i] == ' ' || s[i] == '\t')
            ++i;
        if (s[i] == '\0')
            break;
        size_t start = i;
        while (s[i] != '\0' && s[i] != ' ' && s[i] != '\t')
            ++i;
        size_t len = i - start;
        if (len >= HELP_TOKEN_LEN)
            len = HELP_TOKEN_LEN - 1;
        memcpy(out->tokens[out->count], s + start, len);
        out->tokens[out->count][len] = '\0';
        ++out->count;
    }
}

static int is_placeholder(const char *tok)
{
    return tok[0] == '<' || tok[0] == '[';
}

static int head_matches(const char *form_head, const char *input_head)
{
    if (is_placeholder(form_head)) {
        /* <IP> head: address-shaped tokens only. */
        if (strstr(form_head, "IP") == NULL)
            return input_head[0] != '\0';
        int dot = 0;
        for (const char *p = input_head; *p != '\0'; ++p) {
            if (*p == '.')
                dot = 1;
            else if (*p < '0' || *p > '9')
                return 0;
        }
        return dot;
    }
    return strcmp(form_head, input_head) == 0;
}

/* Number of leading input tokens matched by a form, as a prefix. */
static size_t prefix_match_len(const char *form, const help_tokens *in)
{
    help_tokens f;
    split(form, &f);
    size_t n = in->count < f.count ? in->count : f.count;
    size_t i = 0;
    for (; i < n; ++i) {
        const char *ft = f.tokens[i];
        if (is_placeholder(ft)) {
            if (!head_matches(ft, in->tokens[i]))
                break;
        } else if (strcmp(ft, in->tokens[i]) != 0) {
            break;
        }
    }
    return i;
}

int nero_inline_help_lookup(const char *buffer, nero_help_entry *entry)
{
    memset(entry, 0, sizeof(*entry));
    if (buffer == NULL)
        return 0;
    help_tokens in;
    split(buffer, &in);
    if (in.count == 0)
        return 0;

    size_t best_match = 0;
    size_t best_form_tokens = 0;
    const char *best = NULL;

    /* Scan the catalog for the longest-prefix form sharing the input's head. */
    static const struct {
        const char *form;
        unsigned flags;
        const char *category;
        const char *description;
        const char *example;
    } rows[] = {
#define NERO_FORM_ROW(form, flags, category, description, example)                                  \
    {form, (unsigned)(flags), category, description, example},
        CLI_COMMAND_FORMS(NERO_FORM_ROW)
#undef NERO_FORM_ROW
    };
    const size_t row_count = sizeof(rows) / sizeof(rows[0]);

    size_t best_index = row_count;
    for (size_t r = 0; r < row_count; ++r) {
        help_tokens f;
        split(rows[r].form, &f);
        if (f.count == 0 || !head_matches(f.tokens[0], in.tokens[0]))
            continue;
        size_t m = prefix_match_len(rows[r].form, &in);
        /* Prefer the longest prefix match; tie-break toward the base form
         * (fewest tokens) so "wifi" shows the wifi overview, not a subcommand. */
        if (m > best_match || (m == best_match && (best == NULL || f.count < best_form_tokens))) {
            best_match = m;
            best = rows[r].form;
            best_form_tokens = f.count;
            best_index = r;
        }
    }
    if (best_index == row_count)
        return 0;

    entry->head = in.tokens[0][0] != '\0' ? rows[best_index].form : NULL;
    entry->category = rows[best_index].category;
    entry->syntax = rows[best_index].form;
    entry->description = rows[best_index].description;
    entry->example = rows[best_index].example;
    entry->status = nero_suggest_validate(buffer);
    /* head should be the keyword, not the whole form. */
    static char head_buf[HELP_TOKEN_LEN];
    {
        help_tokens f;
        split(rows[best_index].form, &f);
        size_t len = strlen(f.tokens[0]);
        if (len >= sizeof(head_buf))
            len = sizeof(head_buf) - 1;
        memcpy(head_buf, f.tokens[0], len);
        head_buf[len] = '\0';
        entry->head = head_buf;
    }
    return 1;
}

void nero_inline_help_options(const char *buffer, nero_suggestion_list *out)
{
    out->count = 0;
    out->truncated = 0;
    if (buffer == NULL)
        return;
    help_tokens in;
    split(buffer, &in);
    if (in.count == 0)
        return;

    static const struct {
        const char *form;
        unsigned flags;
        const char *category;
        const char *description;
        const char *example;
    } rows[] = {
#define NERO_FORM_ROW(form, flags, category, description, example)                                  \
    {form, (unsigned)(flags), category, description, example},
        CLI_COMMAND_FORMS(NERO_FORM_ROW)
#undef NERO_FORM_ROW
    };
    const size_t row_count = sizeof(rows) / sizeof(rows[0]);

    for (size_t r = 0; r < row_count && out->count < NERO_SUGGEST_MAX_ITEMS; ++r) {
        help_tokens f;
        split(rows[r].form, &f);
        if (f.count == 0 || !head_matches(f.tokens[0], in.tokens[0]))
            continue;
        nero_suggestion *item = &out->items[out->count++];
        size_t len = strlen(rows[r].form);
        if (len >= NERO_SUGGEST_TEXT_MAX)
            len = NERO_SUGGEST_TEXT_MAX - 1;
        memcpy(item->text, rows[r].form, len);
        item->text[len] = '\0';
        item->display = rows[r].form;
        item->description = rows[r].description;
        item->from_history = 0;
    }
}

static const char *status_label(nero_validate_result status)
{
    switch (status) {
        case NERO_VALIDATE_EMPTY:
            return "";
        case NERO_VALIDATE_INCOMPLETE:
            return "incomplete";
        case NERO_VALIDATE_VALID:
            return "ready";
        case NERO_VALIDATE_UNSUPPORTED:
            return "unsupported on this build";
        case NERO_VALIDATE_INVALID:
        default:
            return "unknown";
    }
}

void nero_inline_help_render(FILE *out, const char *buffer, int full, int color, int unicode)
{
    nero_help_entry entry;
    if (!nero_inline_help_lookup(buffer, &entry))
        return;

    const char *dim_on = color ? "\033[2m" : "";
    const char *dim_off = color ? "\033[0m" : "";
    const char *bullet = unicode ? "\xe2\x80\xa2" : "-";

    if (!full) {
        fprintf(out, "%s%s  %s  [%s]%s\n", dim_on, entry.syntax, entry.description,
                status_label(entry.status), dim_off);
        return;
    }

    fprintf(out, "%s%s%s\n", color ? "\033[1;36m" : "", entry.syntax, color ? "\033[0m" : "");
    fprintf(out, "  %s %s\n", bullet, entry.description);
    fprintf(out, "  %s status: %s\n", bullet, status_label(entry.status));
    fprintf(out, "  %s example: %s\n", bullet, entry.example);

    nero_suggestion_list options;
    nero_inline_help_options(buffer, &options);
    if (options.count > 1) {
        fprintf(out, "  %s options:\n", bullet);
        for (size_t i = 0; i < options.count; ++i)
            fprintf(out, "      %-24s %s\n", options.items[i].display, options.items[i].description);
    }
}
