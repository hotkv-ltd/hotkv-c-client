/**
 * @file hkv_test.h
 * @brief Tiny assertion framework shared by the newer test programs
 * (same output style as the older per-file copies in test_url.c etc.).
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#ifndef HKV_TEST_H
#define HKV_TEST_H

#include <stdio.h>
#include <string.h>

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
        int _both_null = (_a == NULL && _b == NULL);                               \
        if (!_both_null && (!_a || !_b || strcmp(_a, _b) != 0)) {                  \
            printf("FAIL\n    %s == \"%s\", expected \"%s\" at %s:%d\n", #a,      \
                   _a ? _a : "(null)", _b ? _b : "(null)", __FILE__, __LINE__);    \
            g_tests_failed++;                                                      \
            return;                                                                \
        }                                                                          \
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

#define HKV_TEST_SUMMARY()                                                          \
    do {                                                                            \
        printf("\n=================================\n");                           \
        printf("Results: %d passed, %d failed, %d total\n", g_tests_passed,         \
               g_tests_failed, g_tests_run);                                        \
    } while (0)

#endif /* HKV_TEST_H */
