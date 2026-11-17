/*
 * Pure, deterministic suggestion engine for NERO IntelliSense.
 *
 * No terminal I/O, no network, no command execution, no mutable globals. The
 * command registry is suggestion_catalog.h. Placeholder tokens (<IP>, <PCAP>,
 * [OPT]) are never offered as values. See suggestion_engine.h for the contract.
 */
#include "suggestion_engine.h"
#include "suggestion_catalog.h"

#include <stdio.h>
#include <string.h>

/* A command form, tokenized. Longest real form has three tokens
 * ("wifi analyze <PCAP>"); the bound carries slack for future growth. */
#define MAX_FORM_TOKENS 6
#define MAX_TOKEN_LEN 32
/* Input is bounded by the line editor (CLI_LINE_CAPACITY, 256). */
#define MAX_INPUT_TOKENS 32

typedef struct {
    const char *form;
    unsigned flags;
    const char *category;
    const char *description;
    const char *example;
} catalog_row;

static const catalog_row ROWS[] = {
#define NERO_FORM_ROW(form, flags, category, description, example)                                  \
    {form, (unsigned)(flags), category, description, example},
    CLI_COMMAND_FORMS(NERO_FORM_ROW)
#undef NERO_FORM_ROW
};

#define ROW_COUNT (sizeof(ROWS) / sizeof(ROWS[0]))

typedef struct {
    char tokens[MAX_FORM_TOKENS][MAX_TOKEN_LEN];
    size_t count;
} token_vec;

/* Split a NUL-terminated string on runs of spaces/tabs into bounded tokens.
 * Over-long tokens are truncated safely; excess tokens are dropped. Returns
 * whether the string ends with trailing whitespace after at least one token. */
static int split_tokens(const char *s, token_vec *out, size_t max_tokens)
{
    out->count = 0;
    int trailing_space = 0;
    size_t i = 0;
    while (s[i] != '\0') {
        while (s[i] == ' ' || s[i] == '\t')
            ++i;
        if (s[i] == '\0') {
            if (out->count > 0)
                trailing_space = 1;
            break;
        }
        size_t start = i;
        while (s[i] != '\0' && s[i] != ' ' && s[i] != '\t')
            ++i;
        size_t len = i - start;
        if (out->count < max_tokens && out->count < MAX_FORM_TOKENS) {
            if (len >= MAX_TOKEN_LEN)
                len = MAX_TOKEN_LEN - 1;
            memcpy(out->tokens[out->count], s + start, len);
            out->tokens[out->count][len] = '\0';
            ++out->count;
        }
    }
    return trailing_space;
}

static int is_placeholder(const char *tok)
{
    return tok[0] == '<' || tok[0] == '[';
}

/* <IP> accepts only address-shaped tokens so the positional aliases
 * (<IP> -Sc / -Syn) are not proposed after ordinary keywords. Other
 * placeholders (<PCAP>, file names) accept any non-empty token. */
static int placeholder_accepts(const char *placeholder, const char *value)
{
    if (value[0] == '\0')
        return 0;
    if (strstr(placeholder, "IP") != NULL) {
        int dot = 0;
        for (const char *p = value; *p != '\0'; ++p) {
            if (*p == '.')
                dot = 1;
            else if (*p < '0' || *p > '9')
                return 0;
        }
        return dot;
    }
    return 1;
}

static int token_matches(const char *form_token, const char *input_token)
{
    if (is_placeholder(form_token))
        return placeholder_accepts(form_token, input_token);
    return strcmp(form_token, input_token) == 0;
}

static int starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* Index in `buffer` where the partial (in-progress) token begins. When the
 * buffer ends in whitespace there is no partial token and the index is the end
 * of the string, so the completion prefix keeps the user's exact spacing. */
static size_t partial_offset(const char *buffer, int trailing_space, const char *partial)
{
    size_t buffer_len = strlen(buffer);
    if (trailing_space || partial[0] == '\0')
        return buffer_len;
    return buffer_len - strlen(partial);
}

static int form_available(unsigned flags)
{
#if defined(NERO_NETWORK_UNAVAILABLE)
    if (flags & NERO_FORM_NET)
        return 0;
#endif
#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SYN_UNAVAILABLE)
    if (flags & NERO_FORM_SYN)
        return 0;
#endif
    (void)flags;
    return 1;
}

static void list_push(nero_suggestion_list *list, const char *text, const char *display,
                      const char *description, int from_history)
{
    /* De-duplicate on the insertable text. */
    for (size_t i = 0; i < list->count; ++i)
        if (strcmp(list->items[i].text, text) == 0)
            return;
    if (list->count >= NERO_SUGGEST_MAX_ITEMS) {
        list->truncated = 1;
        return;
    }
    nero_suggestion *item = &list->items[list->count++];
    size_t len = strlen(text);
    if (len >= NERO_SUGGEST_TEXT_MAX)
        len = NERO_SUGGEST_TEXT_MAX - 1;
    memcpy(item->text, text, len);
    item->text[len] = '\0';
    item->display = display;
    item->description = description != NULL ? description : "";
    item->from_history = from_history;
}

/* Stable best-first ordering: shorter completions first, then lexicographic. */
static void list_sort(nero_suggestion_list *list)
{
    for (size_t i = 1; i < list->count; ++i) {
        nero_suggestion key = list->items[i];
        size_t key_len = strlen(key.text);
        size_t j = i;
        while (j > 0) {
            size_t prev_len = strlen(list->items[j - 1].text);
            if (prev_len < key_len ||
                (prev_len == key_len && strcmp(list->items[j - 1].text, key.text) <= 0))
                break;
            list->items[j] = list->items[j - 1];
            --j;
        }
        list->items[j] = key;
    }
}

void nero_suggest_matches(const char *buffer, nero_suggestion_list *list)
{
    list->count = 0;
    list->truncated = 0;
    if (buffer == NULL)
        return;

    token_vec in;
    int trailing_space = split_tokens(buffer, &in, MAX_INPUT_TOKENS);
    size_t completed = trailing_space ? in.count : (in.count > 0 ? in.count - 1 : 0);
    const char *partial = (!trailing_space && in.count > 0) ? in.tokens[in.count - 1] : "";
    size_t next_index = completed;

    size_t prefix_len = partial_offset(buffer, trailing_space, partial);
    char prefix[NERO_SUGGEST_TEXT_MAX];
    if (prefix_len >= sizeof(prefix))
        return; /* buffer longer than any completion we would store */
    memcpy(prefix, buffer, prefix_len);
    prefix[prefix_len] = '\0';

    for (size_t r = 0; r < ROW_COUNT; ++r) {
        token_vec form;
        split_tokens(ROWS[r].form, &form, MAX_FORM_TOKENS);
        if (form.count <= next_index)
            continue; /* nothing left in this form to offer */

        int ok = 1;
        for (size_t i = 0; i < completed; ++i) {
            if (!token_matches(form.tokens[i], in.tokens[i])) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;

        const char *next = form.tokens[next_index];
        if (is_placeholder(next))
            continue; /* never complete a value */
        if (!starts_with(next, partial))
            continue;

        char text[NERO_SUGGEST_TEXT_MAX];
        int written = snprintf(text, sizeof(text), "%s%s", prefix, next);
        if (written < 0 || (size_t)written >= sizeof(text))
            continue;
        list_push(list, text, ROWS[r].form, ROWS[r].description, 0);
    }
    list_sort(list);
}

/* Longest common prefix length across all candidate texts. */
static size_t common_prefix_len(const nero_suggestion_list *list)
{
    if (list->count == 0)
        return 0;
    size_t limit = strlen(list->items[0].text);
    for (size_t i = 1; i < list->count; ++i) {
        size_t j = 0;
        const char *a = list->items[0].text;
        const char *b = list->items[i].text;
        while (j < limit && a[j] != '\0' && a[j] == b[j])
            ++j;
        limit = j;
    }
    return limit;
}

size_t nero_suggest_ghost(const char *buffer, const char *const *history, size_t history_count,
                          char *out, size_t out_size)
{
    if (out_size == 0)
        return 0;
    out[0] = '\0';
    if (buffer == NULL || buffer[0] == '\0')
        return 0;

    nero_suggestion_list list;
    nero_suggest_matches(buffer, &list);
    size_t buffer_len = strlen(buffer);
    if (list.count > 0) {
        size_t lcp = common_prefix_len(&list);
        if (lcp > buffer_len) {
            size_t suffix = lcp - buffer_len;
            if (suffix >= out_size)
                suffix = out_size - 1;
            memcpy(out, list.items[0].text + buffer_len, suffix);
            out[suffix] = '\0';
            return suffix;
        }
    }

    /* History fallback: the most recent entry extending the exact buffer. */
    if (history != NULL) {
        for (size_t i = history_count; i > 0; --i) {
            const char *line = history[i - 1];
            if (line == NULL)
                continue;
            size_t line_len = strlen(line);
            if (line_len > buffer_len && strncmp(line, buffer, buffer_len) == 0) {
                size_t suffix = line_len - buffer_len;
                if (suffix >= out_size)
                    suffix = out_size - 1;
                memcpy(out, line + buffer_len, suffix);
                out[suffix] = '\0';
                return suffix;
            }
        }
    }
    return 0;
}

nero_tab_result nero_suggest_complete(char *buffer, size_t buffer_size, nero_suggestion_list *list)
{
    nero_suggest_matches(buffer, list);
    if (list->count == 0)
        return NERO_TAB_NONE;

    size_t buffer_len = strlen(buffer);
    size_t lcp = common_prefix_len(list);
    if (lcp > buffer_len) {
        if (lcp >= buffer_size)
            lcp = buffer_size - 1;
        /* Every candidate shares list->items[0] as a prefix up to lcp. */
        memcpy(buffer, list->items[0].text, lcp);
        buffer[lcp] = '\0';
        return NERO_TAB_COMPLETED;
    }
    if (list->count == 1)
        return NERO_TAB_NONE; /* already complete; never insert a bare space */
    return NERO_TAB_AMBIGUOUS;
}

nero_validate_result nero_suggest_validate(const char *buffer)
{
    if (buffer == NULL)
        return NERO_VALIDATE_INVALID;
    token_vec in;
    (void)split_tokens(buffer, &in, MAX_INPUT_TOKENS);
    if (in.count == 0)
        return NERO_VALIDATE_EMPTY;

    int saw_prefix = 0;
    for (size_t r = 0; r < ROW_COUNT; ++r) {
        token_vec form;
        split_tokens(ROWS[r].form, &form, MAX_FORM_TOKENS);
        if (form.count < in.count)
            continue;
        int ok = 1;
        for (size_t i = 0; i < in.count; ++i) {
            if (!token_matches(form.tokens[i], in.tokens[i])) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;
        if (form.count == in.count)
            return form_available(ROWS[r].flags) ? NERO_VALIDATE_VALID
                                                  : NERO_VALIDATE_UNSUPPORTED;
        saw_prefix = 1;
    }
    return saw_prefix ? NERO_VALIDATE_INCOMPLETE : NERO_VALIDATE_INVALID;
}

size_t nero_edit_distance(const char *a, const char *b, size_t max)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la > lb) {
        const char *t = a;
        a = b;
        b = t;
        size_t tl = la;
        la = lb;
        lb = tl;
    }
    if (lb - la > max)
        return max + 1;
    /* Two-row DP, bounded by the shorter string's length. la <= lb. */
    size_t prev[MAX_TOKEN_LEN + 1];
    size_t curr[MAX_TOKEN_LEN + 1];
    if (la > MAX_TOKEN_LEN || lb > 4 * MAX_TOKEN_LEN)
        return max + 1; /* out of scope for command-name typo correction */
    for (size_t j = 0; j <= la; ++j)
        prev[j] = j;
    for (size_t i = 1; i <= lb; ++i) {
        curr[0] = i;
        size_t row_min = curr[0];
        for (size_t j = 1; j <= la; ++j) {
            size_t cost = (a[j - 1] == b[i - 1]) ? 0 : 1;
            size_t del = prev[j] + 1;
            size_t ins = curr[j - 1] + 1;
            size_t sub = prev[j - 1] + cost;
            size_t best = del < ins ? del : ins;
            if (sub < best)
                best = sub;
            curr[j] = best;
            if (best < row_min)
                row_min = best;
        }
        if (row_min > max)
            return max + 1;
        for (size_t j = 0; j <= la; ++j)
            prev[j] = curr[j];
    }
    return prev[la];
}

int nero_suggest_typo(const char *buffer, char *out, size_t out_size)
{
    if (out_size == 0)
        return 0;
    out[0] = '\0';
    if (buffer == NULL)
        return 0;

    /* Only correct when the input matches no form at all. */
    if (nero_suggest_validate(buffer) != NERO_VALIDATE_INVALID)
        return 0;

    token_vec in;
    (void)split_tokens(buffer, &in, MAX_INPUT_TOKENS);
    if (in.count == 0)
        return 0;
    const char *word = in.tokens[0];
    size_t word_len = strlen(word);
    if (word_len < 2)
        return 0; /* too short to correct without guessing */

    /* Permit one edit for short words, two for longer ones. */
    size_t budget = word_len <= 4 ? 1 : 2;

    char best[MAX_TOKEN_LEN] = {0};
    size_t best_distance = budget + 1;
    int have_best = 0;
    int ambiguous = 0;

    for (size_t r = 0; r < ROW_COUNT; ++r) {
        token_vec form;
        split_tokens(ROWS[r].form, &form, MAX_FORM_TOKENS);
        const char *head = form.tokens[0];
        if (is_placeholder(head))
            continue;
        if (strcmp(head, word) == 0)
            return 0; /* exact head; not a typo */
        size_t d = nero_edit_distance(word, head, budget);
        if (d > budget)
            continue;
        if (have_best && strcmp(best, head) == 0)
            continue; /* same keyword reached via another form */
        if (!have_best || d < best_distance) {
            size_t len = strlen(head);
            if (len >= sizeof(best))
                len = sizeof(best) - 1;
            memcpy(best, head, len);
            best[len] = '\0';
            best_distance = d;
            have_best = 1;
            ambiguous = 0;
        } else if (d == best_distance) {
            ambiguous = 1;
        }
    }
    if (!have_best || ambiguous)
        return 0;
    size_t len = strlen(best);
    if (len >= out_size)
        return 0;
    memcpy(out, best, len + 1);
    return 1;
}
