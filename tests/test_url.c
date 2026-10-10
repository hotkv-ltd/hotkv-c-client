/**
 * @file test_url.c
 * @brief Unit tests for hotkv_url_parse(). No live server is required.
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

/* ========================================================================= */

TEST(basic_hotkv_scheme) {
    hotkv_url_t u;
    ASSERT_EQ_INT(hotkv_url_parse("hotkv://localhost:6399", &u), HOTKV_OK);
    ASSERT_STR_EQ(u.host, "localhost");
    ASSERT_EQ_INT(u.port, 6399);
    ASSERT_EQ_INT(u.db, 0);
    ASSERT_EQ_INT(u.use_tls, 0);
    ASSERT(u.username == NULL);
    ASSERT(u.password == NULL);
    hotkv_url_free(&u);
    PASS();
}

TEST(tls_scheme) {
    hotkv_url_t u;
    ASSERT_EQ_INT(hotkv_url_parse("hotkvs://secure.example.com:6400", &u), HOTKV_OK);
    ASSERT_STR_EQ(u.host, "secure.example.com");
    ASSERT_EQ_INT(u.port, 6400);
    ASSERT_EQ_INT(u.use_tls, 1);
    hotkv_url_free(&u);
    PASS();
}

TEST(redis_scheme_accepted) {
    hotkv_url_t u;
    ASSERT_EQ_INT(hotkv_url_parse("redis://127.0.0.1:6379/3", &u), HOTKV_OK);
    ASSERT_STR_EQ(u.host, "127.0.0.1");
    ASSERT_EQ_INT(u.port, 6379);
    ASSERT_EQ_INT(u.db, 3);
    ASSERT_EQ_INT(u.use_tls, 0);
    hotkv_url_free(&u);
    PASS();
}

TEST(rediss_scheme_with_auth_and_db) {
    hotkv_url_t u;
    ASSERT_EQ_INT(hotkv_url_parse("rediss://user:p%40ss@host.example:1234/5", &u), HOTKV_OK);
    ASSERT_STR_EQ(u.host, "host.example");
    ASSERT_EQ_INT(u.port, 1234);
    ASSERT_EQ_INT(u.db, 5);
    ASSERT_EQ_INT(u.use_tls, 1);
    ASSERT_STR_EQ(u.username, "user");
    ASSERT_STR_EQ(u.password, "p@ss");
    hotkv_url_free(&u);
    PASS();
}

TEST(password_only) {
    hotkv_url_t u;
    ASSERT_EQ_INT(hotkv_url_parse("hotkv://:onlypass@host", &u), HOTKV_OK);
    ASSERT_STR_EQ(u.host, "host");
    ASSERT_EQ_INT(u.port, 6379);
    ASSERT(u.username == NULL);
    ASSERT_STR_EQ(u.password, "onlypass");
    hotkv_url_free(&u);
    PASS();
}

TEST(default_host_when_empty) {
    hotkv_url_t u;
    ASSERT_EQ_INT(hotkv_url_parse("hotkv://:6399", &u), HOTKV_OK);
    ASSERT_STR_EQ(u.host, "127.0.0.1");
    ASSERT_EQ_INT(u.port, 6399);
    hotkv_url_free(&u);
    PASS();
}

TEST(no_scheme_separator_is_error) {
    hotkv_url_t u;
    ASSERT_EQ_INT(hotkv_url_parse("localhost:6399", &u), HOTKV_ERR_PROTOCOL);
    PASS();
}

TEST(unknown_scheme_is_error) {
    hotkv_url_t u;
    ASSERT_EQ_INT(hotkv_url_parse("ftp://localhost:6399", &u), HOTKV_ERR_PROTOCOL);
    PASS();
}

TEST(null_args) {
    hotkv_url_t u;
    ASSERT_EQ_INT(hotkv_url_parse(NULL, &u), HOTKV_ERR_OTHER);
    ASSERT_EQ_INT(hotkv_url_parse("hotkv://host", NULL), HOTKV_ERR_OTHER);
    PASS();
}

TEST(free_is_null_safe) {
    hotkv_url_free(NULL);
    PASS();
}

int main(void) {
    printf("=== URL parser tests ===\n\n");

    run_test_basic_hotkv_scheme();
    run_test_tls_scheme();
    run_test_redis_scheme_accepted();
    run_test_rediss_scheme_with_auth_and_db();
    run_test_password_only();
    run_test_default_host_when_empty();
    run_test_no_scheme_separator_is_error();
    run_test_unknown_scheme_is_error();
    run_test_null_args();
    run_test_free_is_null_safe();

    printf("\n=================================\n");
    printf("Results: %d passed, %d failed, %d total\n", g_tests_passed, g_tests_failed,
           g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
