/**
 * @file test_ai.c
 * @brief Unit tests for hotkv_promptcache_key() determinism, including a
 * cross-language reference vector (the same input must hash identically in
 * every official HotKV SDK). No live server required.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "hotkv.h"
#include "hotkv_private.h"

#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Minimal test framework (matches tests/test_commands.c)
 * ========================================================================= */

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name)                                            \
    static void test_##name(void);                            \
    static void run_test_##name(void) {                       \
        g_tests_run++;                                        \
        printf("  %-50s ", #name);                            \
        test_##name();                                        \
    }                                                         \
    static void test_##name(void)

#define ASSERT(expr)                                          \
    do {                                                      \
        if (!(expr)) {                                        \
            printf("FAIL\n    ASSERT(%s) at %s:%d\n",        \
                   #expr, __FILE__, __LINE__);                \
            g_tests_failed++;                                 \
            return;                                           \
        }                                                     \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                        \
    do {                                                                           \
        const char *_a = (a);                                                      \
        const char *_b = (b);                                                      \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                   \
            printf("FAIL\n    %s == \"%s\", expected \"%s\" at %s:%d\n", #a,      \
                   _a ? _a : "(null)", _b ? _b : "(null)", __FILE__, __LINE__);    \
            g_tests_failed++;                                                      \
            return;                                                                \
        }                                                                          \
    } while (0)

#define PASS() do { printf("OK\n"); g_tests_passed++; } while (0)

/* ========================================================================= */

TEST(golden_vector_matches_every_sdk) {
    const char *roles[] = {"user"};
    const char *contents[] = {"Hello"};
    const char *pkeys[] = {"temperature", "top_p"};
    const char *pvals[] = {"0.7", "1"};

    char *key = hotkv_promptcache_key("openai", "gpt-4", "You are helpful.", roles, contents, 1,
                                      pkeys, pvals, 2);
    ASSERT(key != NULL);
    ASSERT_STR_EQ(key, "14caa0bbab1edd17eb7205f946a358d94a5e47db203e5514f6e1c405d987311b");
    ASSERT(strlen(key) == 64);
    hotkv_free(key);
    PASS();
}

TEST(deterministic_across_repeated_calls) {
    const char *roles[] = {"system", "user"};
    const char *contents[] = {"be terse", "2+2?"};
    const char *pkeys[] = {"max_tokens", "temperature"};
    const char *pvals[] = {"100", "0.0"};
    char *first =
        hotkv_promptcache_key("anthropic", "claude", NULL, roles, contents, 2, pkeys, pvals, 2);
    int i;

    ASSERT(first != NULL);
    for (i = 0; i < 20; i++) {
        char *again =
            hotkv_promptcache_key("anthropic", "claude", NULL, roles, contents, 2, pkeys, pvals, 2);
        ASSERT(again != NULL);
        ASSERT_STR_EQ(again, first);
        hotkv_free(again);
    }
    hotkv_free(first);
    PASS();
}

TEST(param_order_independent) {
    const char *keys_ab[] = {"a", "b", "c"};
    const char *vals_ab[] = {"1", "2", "3"};
    const char *keys_ba[] = {"c", "a", "b"};
    const char *vals_ba[] = {"3", "1", "2"};

    char *k1 = hotkv_promptcache_key("p", "m", NULL, NULL, NULL, 0, keys_ab, vals_ab, 3);
    char *k2 = hotkv_promptcache_key("p", "m", NULL, NULL, NULL, 0, keys_ba, vals_ba, 3);
    ASSERT(k1 != NULL && k2 != NULL);
    ASSERT_STR_EQ(k1, k2);
    hotkv_free(k1);
    hotkv_free(k2);
    PASS();
}

TEST(differs_on_content_change) {
    const char *roles[] = {"user"};
    const char *c1[] = {"hi"};
    const char *c2[] = {"hi!"};
    char *k1 = hotkv_promptcache_key("p", "m", NULL, roles, c1, 1, NULL, NULL, 0);
    char *k2 = hotkv_promptcache_key("p", "m", NULL, roles, c2, 1, NULL, NULL, 0);
    ASSERT(k1 != NULL && k2 != NULL);
    ASSERT(strcmp(k1, k2) != 0);
    hotkv_free(k1);
    hotkv_free(k2);
    PASS();
}

TEST(null_system_matches_empty_string) {
    char *k1 = hotkv_promptcache_key("p", "m", NULL, NULL, NULL, 0, NULL, NULL, 0);
    char *k2 = hotkv_promptcache_key("p", "m", "", NULL, NULL, 0, NULL, NULL, 0);
    ASSERT(k1 != NULL && k2 != NULL);
    ASSERT_STR_EQ(k1, k2);
    hotkv_free(k1);
    hotkv_free(k2);
    PASS();
}

TEST(handles_escaping_without_crashing) {
    char *k = hotkv_promptcache_key("p", "m", "line1\nline2\t\"quoted\"\\backslash", NULL, NULL, 0,
                                    NULL, NULL, 0);
    ASSERT(k != NULL);
    ASSERT(strlen(k) == 64);
    hotkv_free(k);
    PASS();
}

int main(void) {
    printf("=== PromptCache key builder tests ===\n\n");

    run_test_golden_vector_matches_every_sdk();
    run_test_deterministic_across_repeated_calls();
    run_test_param_order_independent();
    run_test_differs_on_content_change();
    run_test_null_system_matches_empty_string();
    run_test_handles_escaping_without_crashing();

    printf("\n=================================\n");
    printf("Results: %d passed, %d failed, %d total\n", g_tests_passed, g_tests_failed,
           g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
