/*
 * Deterministic unit tests for the pure IntelliSense suggestion engine.
 * Links only suggestion_engine.c: no terminal, no network, no dispatch.
 */
#include "suggestion_engine.h"
#include "suggestion_catalog.h"
#include "command_catalog.h"

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

static int ghost_eq(const char *buffer, const char *const *hist, size_t hn, const char *expect)
{
    char out[256];
    nero_suggest_ghost(buffer, hist, hn, out, sizeof(out));
    return strcmp(out, expect) == 0;
}

static nero_tab_result tab(const char *in, char *out, size_t out_size, nero_suggestion_list *list)
{
    strncpy(out, in, out_size - 1);
    out[out_size - 1] = '\0';
    return nero_suggest_complete(out, out_size, list);
}

static void test_edit_distance(void)
{
    CHECK(nero_edit_distance("kitten", "sitting", 8) == 3);
    CHECK(nero_edit_distance("", "", 4) == 0);
    CHECK(nero_edit_distance("abc", "abc", 4) == 0);
    CHECK(nero_edit_distance("abc", "abd", 4) == 1);
    CHECK(nero_edit_distance("version", "versin", 4) == 1);
    /* Capping: distance beyond max returns max + 1. */
    CHECK(nero_edit_distance("abcdef", "uvwxyz", 2) == 3);
}

static void test_ghost(void)
{
    /* Catalog continuation of a partial command / token. */
    CHECK(ghost_eq("ver", NULL, 0, "sion"));
    CHECK(ghost_eq("he", NULL, 0, "lp"));
    CHECK(ghost_eq("wifi an", NULL, 0, "alyze"));
    CHECK(ghost_eq("history -c", NULL, 0, "lear"));
    CHECK(ghost_eq("network en", NULL, 0, "able"));
    /* No ghost for empty input or ambiguous continuations. */
    CHECK(ghost_eq("", NULL, 0, ""));
    CHECK(ghost_eq("wifi a", NULL, 0, "")); /* aps vs analyze */
    /* Placeholders are never fabricated as values. */
    CHECK(ghost_eq("scan ", NULL, 0, ""));
    CHECK(ghost_eq("wifi analyze ", NULL, 0, ""));
}

static void test_history_ghost(void)
{
    const char *hist[] = {"wifi monitor", "scan 10.0.0.1", "scan 10.0.0.2"};
    /* The most recent matching history entry wins as a fallback. */
    CHECK(ghost_eq("scan ", hist, 3, "10.0.0.2"));
    /* Catalog continuation takes priority over history. */
    CHECK(ghost_eq("sc", hist, 3, "an"));
    /* History only; unknown prefix yields nothing. */
    CHECK(ghost_eq("zzz", hist, 3, ""));
}

static void test_completion(void)
{
    char buf[256];
    nero_suggestion_list list;

    CHECK(tab("vers", buf, sizeof(buf), &list) == NERO_TAB_COMPLETED);
    CHECK(strcmp(buf, "version") == 0);

    CHECK(tab("history -c", buf, sizeof(buf), &list) == NERO_TAB_COMPLETED);
    CHECK(strcmp(buf, "history -clear") == 0);

    CHECK(tab("network en", buf, sizeof(buf), &list) == NERO_TAB_COMPLETED);
    CHECK(strcmp(buf, "network enable") == 0);

    /* Common-prefix extension across several matches ("log" <- "logs"). */
    CHECK(tab("l", buf, sizeof(buf), &list) == NERO_TAB_COMPLETED);
    CHECK(strcmp(buf, "log") == 0);

    /* Ambiguous: a menu is offered; the buffer is unchanged. */
    CHECK(tab("wifi ", buf, sizeof(buf), &list) == NERO_TAB_AMBIGUOUS);
    CHECK(strcmp(buf, "wifi ") == 0);
    CHECK(list.count > 1);

    /* No match: nothing is completed. */
    CHECK(tab("zzz", buf, sizeof(buf), &list) == NERO_TAB_NONE);
    CHECK(strcmp(buf, "zzz") == 0);

    /* A placeholder-only next token is never auto-completed. */
    CHECK(tab("scan ", buf, sizeof(buf), &list) == NERO_TAB_NONE);
    CHECK(strcmp(buf, "scan ") == 0);
}

static void test_match_quality(void)
{
    nero_suggestion_list list;
    nero_suggest_matches("wifi ", &list);
    /* Menu items are de-duplicated and carry a readable display + description. */
    CHECK(list.count > 1);
    for (size_t i = 0; i < list.count; ++i) {
        CHECK(list.items[i].display != NULL);
        CHECK(list.items[i].description != NULL);
        CHECK(list.items[i].from_history == 0);
        for (size_t j = i + 1; j < list.count; ++j)
            CHECK(strcmp(list.items[i].text, list.items[j].text) != 0); /* no duplicates */
    }
    /* Shorter completions sort first. */
    for (size_t i = 1; i < list.count; ++i)
        CHECK(strlen(list.items[i - 1].text) <= strlen(list.items[i].text));
}

static void test_validation(void)
{
    CHECK(nero_suggest_validate("") == NERO_VALIDATE_EMPTY);
    CHECK(nero_suggest_validate("   ") == NERO_VALIDATE_EMPTY);
    CHECK(nero_suggest_validate("network") == NERO_VALIDATE_INCOMPLETE);
    CHECK(nero_suggest_validate("network status") == NERO_VALIDATE_VALID);
    CHECK(nero_suggest_validate("help") == NERO_VALIDATE_VALID);
    CHECK(nero_suggest_validate("help ") == NERO_VALIDATE_VALID);
    CHECK(nero_suggest_validate("wifi") == NERO_VALIDATE_VALID);
    CHECK(nero_suggest_validate("wifi analyze capture.pcap") == NERO_VALIDATE_VALID);
    CHECK(nero_suggest_validate("scan foo") == NERO_VALIDATE_INVALID);
    CHECK(nero_suggest_validate("bogus") == NERO_VALIDATE_INVALID);
    CHECK(nero_suggest_validate("help extra junk") == NERO_VALIDATE_INVALID);

    /* Network-dependent forms reflect the build: available unless compiled out. */
    nero_validate_result scan = nero_suggest_validate("scan 192.0.2.1");
    nero_validate_result syn = nero_suggest_validate("192.0.2.1 -Syn");
#if defined(NERO_NETWORK_UNAVAILABLE)
    CHECK(scan == NERO_VALIDATE_UNSUPPORTED);
#else
    CHECK(scan == NERO_VALIDATE_VALID);
#endif
#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SYN_UNAVAILABLE)
    CHECK(syn == NERO_VALIDATE_UNSUPPORTED);
#else
    CHECK(syn == NERO_VALIDATE_VALID);
#endif
}

static void test_typo(void)
{
    char out[64];
    CHECK(nero_suggest_typo("netwrk", out, sizeof(out)) == 1 && strcmp(out, "network") == 0);
    CHECK(nero_suggest_typo("versin", out, sizeof(out)) == 1 && strcmp(out, "version") == 0);
    CHECK(nero_suggest_typo("helpp", out, sizeof(out)) == 1 && strcmp(out, "help") == 0);
    /* Exact command: not a typo. */
    CHECK(nero_suggest_typo("wifi", out, sizeof(out)) == 0);
    /* Valid prefix: not a typo. */
    CHECK(nero_suggest_typo("hist", out, sizeof(out)) == 0);
    /* Too short / too far: no guess. */
    CHECK(nero_suggest_typo("q", out, sizeof(out)) == 0);
    CHECK(nero_suggest_typo("zzzzzz", out, sizeof(out)) == 0);
}

static void test_robustness(void)
{
    /* NULL and tiny buffers must be safe. */
    char out[8];
    CHECK(nero_suggest_ghost(NULL, NULL, 0, out, sizeof(out)) == 0);
    CHECK(nero_suggest_validate(NULL) == NERO_VALIDATE_INVALID);
    CHECK(nero_suggest_typo(NULL, out, sizeof(out)) == 0);
    nero_suggestion_list list;
    nero_suggest_matches(NULL, &list);
    CHECK(list.count == 0);
    /* Oversized token input does not overflow. */
    char big[512];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    nero_suggest_matches(big, &list); /* must not crash */
    CHECK(nero_suggest_validate(big) == NERO_VALIDATE_INVALID);
}

/* Every executable form's head keyword must be one the dispatcher actually
 * recognizes: either a CLI_KEYWORDS entry, the "-mode" network flag handled
 * specially in the dispatcher, or an <IP>/placeholder positional head. This
 * keeps the IntelliSense catalog from drifting away from the real command set. */
static int known_head(const char *head)
{
    if (head[0] == '<' || head[0] == '[')
        return 1; /* placeholder positional alias */
    if (strcmp(head, "-mode") == 0)
        return 1; /* dispatcher-handled flag, not a keyword */
    static const char *const keywords[] = {
#define KEYWORD(name, handler) #name,
        CLI_KEYWORDS(KEYWORD)
#undef KEYWORD
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i)
        if (strcmp(keywords[i], head) == 0)
            return 1;
    return 0;
}

static void test_catalog_consistency(void)
{
#define FORM_HEAD(form, flags, category, description, example)                                      \
    do {                                                                                           \
        const char *f = form;                                                                      \
        char head[32];                                                                             \
        size_t n = 0;                                                                              \
        while (f[n] != '\0' && f[n] != ' ' && n < sizeof(head) - 1) {                               \
            head[n] = f[n];                                                                         \
            ++n;                                                                                   \
        }                                                                                          \
        head[n] = '\0';                                                                            \
        if (!known_head(head))                                                                     \
            fprintf(stderr, "FAIL catalog head '%s' not a dispatcher command\n", head);           \
        CHECK(known_head(head));                                                                   \
    } while (0);
    CLI_COMMAND_FORMS(FORM_HEAD)
#undef FORM_HEAD
}

int main(void)
{
    test_catalog_consistency();
    test_edit_distance();
    test_ghost();
    test_history_ghost();
    test_completion();
    test_match_quality();
    test_validation();
    test_typo();
    test_robustness();
    if (failures == 0)
        puts("suggestion-engine: all checks passed");
    return failures == 0 ? 0 : 1;
}
