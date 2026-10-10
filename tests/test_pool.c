/**
 * @file test_pool.c
 * @brief Unit tests for connection pool lifecycle.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * These tests verify pool creation, configuration validation, and
 * NULL-safety without requiring a live server. Connection-dependent
 * behaviour (acquire/release) would require integration tests.
 */

#include "hotkv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Minimal test framework
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

#define ASSERT_EQ_INT(a, b)                                   \
    do {                                                      \
        long long _a = (long long)(a);                        \
        long long _b = (long long)(b);                        \
        if (_a != _b) {                                       \
            printf("FAIL\n    %s == %lld, expected %lld"      \
                   " at %s:%d\n",                             \
                   #a, _a, _b, __FILE__, __LINE__);           \
            g_tests_failed++;                                 \
            return;                                           \
        }                                                     \
    } while (0)

#define PASS() do { printf("OK\n"); g_tests_passed++; } while (0)

/* =========================================================================
 * Pool lifecycle
 * ========================================================================= */

TEST(pool_null_opts) {
    /* NULL opts should return NULL, not crash. */
    hotkv_pool_t *pool = hotkv_pool_new(NULL);
    ASSERT(pool == NULL);
    PASS();
}

TEST(pool_free_null) {
    /* Should not crash. */
    hotkv_pool_free(NULL);
    PASS();
}

TEST(pool_active_null) {
    /* Should return 0, not crash. */
    ASSERT_EQ_INT(hotkv_pool_active(NULL), 0);
    PASS();
}

TEST(pool_release_null) {
    /* Should not crash. */
    hotkv_pool_release(NULL, NULL);
    PASS();
}

/* =========================================================================
 * Context lifecycle
 * ========================================================================= */

TEST(disconnect_null) {
    /* Should not crash. */
    hotkv_disconnect(NULL);
    PASS();
}

TEST(free_reply_null) {
    /* Should not crash. */
    hotkv_free_reply(NULL);
    PASS();
}

TEST(reader_free_null) {
    /* Should not crash. */
    hotkv_reader_free(NULL);
    PASS();
}

TEST(reader_new_and_free) {
    hotkv_reader_t *r = hotkv_reader_new();
    ASSERT(r != NULL);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * Custom allocator
 * ========================================================================= */

static int s_alloc_count = 0;
static int s_free_count = 0;

static void *test_malloc(size_t sz) {
    s_alloc_count++;
    return malloc(sz);
}

static void *test_calloc(size_t c, size_t sz) {
    s_alloc_count++;
    return calloc(c, sz);
}

static void *test_realloc(void *p, size_t sz) {
    return realloc(p, sz);
}

static void test_free(void *p) {
    if (p) s_free_count++;
    free(p);
}

TEST(custom_allocator) {
    s_alloc_count = 0;
    s_free_count = 0;

    hotkv_set_allocator(test_malloc, test_calloc, test_realloc, test_free);

    hotkv_reader_t *r = hotkv_reader_new();
    ASSERT(r != NULL);
    ASSERT(s_alloc_count > 0);

    hotkv_reader_free(r);
    ASSERT(s_free_count > 0);

    /* Restore default allocator */
    hotkv_set_allocator(NULL, NULL, NULL, NULL);
    PASS();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
    printf("libhotkv pool & lifecycle tests\n");
    printf("================================\n\n");

    printf("Pool lifecycle:\n");
    run_test_pool_null_opts();
    run_test_pool_free_null();
    run_test_pool_active_null();
    run_test_pool_release_null();

    printf("\nContext lifecycle:\n");
    run_test_disconnect_null();
    run_test_free_reply_null();
    run_test_reader_free_null();
    run_test_reader_new_and_free();

    printf("\nCustom allocator:\n");
    run_test_custom_allocator();

    printf("\n================================\n");
    printf("Results: %d passed, %d failed, %d total\n",
           g_tests_passed, g_tests_failed, g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
