/**
 * @file test_resp.c
 * @brief Unit tests for the RESP2/RESP3 parser.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * Minimal test framework: ASSERT macros with pass/fail counts.
 * The parser is tested independently of any connection layer.
 */

#include "hotkv.h"
#include "hotkv_private.h"

#include <math.h>
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

#define ASSERT_EQ_STR(a, b)                                   \
    do {                                                      \
        const char *_a = (a);                                 \
        const char *_b = (b);                                 \
        if (!_a || !_b || strcmp(_a, _b) != 0) {              \
            printf("FAIL\n    \"%s\" != \"%s\" at %s:%d\n",  \
                   _a ? _a : "(null)",                        \
                   _b ? _b : "(null)",                        \
                   __FILE__, __LINE__);                       \
            g_tests_failed++;                                 \
            return;                                           \
        }                                                     \
    } while (0)

#define PASS() do { printf("OK\n"); g_tests_passed++; } while (0)

/* =========================================================================
 * RESP2: Simple string
 * ========================================================================= */

TEST(simple_string) {
    hotkv_reader_t *r = hotkv_reader_new();
    ASSERT(r != NULL);

    const char *data = "+OK\r\n";
    ASSERT_EQ_INT(hotkv_reader_feed(r, data, strlen(data)), HOTKV_OK);

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_STRING);
    ASSERT_EQ_STR(reply->str, "OK");
    ASSERT_EQ_INT(reply->len, 2);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(simple_string_pong) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "+PONG\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_STRING);
    ASSERT_EQ_STR(reply->str, "PONG");

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP2: Error
 * ========================================================================= */

TEST(error) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "-ERR unknown command 'foobar'\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_ERROR);
    ASSERT_EQ_STR(reply->str, "ERR unknown command 'foobar'");

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(wrongtype_error) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_ERROR);
    ASSERT(strstr(reply->str, "WRONGTYPE") != NULL);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP2: Integer
 * ========================================================================= */

TEST(integer_positive) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = ":1000\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_INTEGER);
    ASSERT_EQ_INT(reply->integer, 1000);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(integer_negative) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = ":-42\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_INTEGER);
    ASSERT_EQ_INT(reply->integer, -42);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(integer_zero) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = ":0\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_INTEGER);
    ASSERT_EQ_INT(reply->integer, 0);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP2: Bulk string
 * ========================================================================= */

TEST(bulk_string) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "$5\r\nhello\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_BULK);
    ASSERT_EQ_STR(reply->str, "hello");
    ASSERT_EQ_INT(reply->len, 5);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(bulk_string_empty) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "$0\r\n\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_BULK);
    ASSERT_EQ_STR(reply->str, "");
    ASSERT_EQ_INT(reply->len, 0);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(null_bulk_string) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "$-1\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_NIL);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(bulk_string_binary) {
    /* Binary-safe: contains NUL byte */
    hotkv_reader_t *r = hotkv_reader_new();
    const char data[] = "$6\r\nhe\x00llo\r\n";
    hotkv_reader_feed(r, data, sizeof(data) - 1); /* exclude NUL terminator */

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_BULK);
    ASSERT_EQ_INT(reply->len, 6);
    ASSERT(memcmp(reply->str, "he\x00llo", 6) == 0);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP2: Array
 * ========================================================================= */

TEST(array_basic) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_ARRAY);
    ASSERT_EQ_INT(reply->nelem, 2);
    ASSERT_EQ_INT(reply->elements[0]->type, HOTKV_REPLY_BULK);
    ASSERT_EQ_STR(reply->elements[0]->str, "foo");
    ASSERT_EQ_INT(reply->elements[1]->type, HOTKV_REPLY_BULK);
    ASSERT_EQ_STR(reply->elements[1]->str, "bar");

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(array_empty) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "*0\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_ARRAY);
    ASSERT_EQ_INT(reply->nelem, 0);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(null_array) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "*-1\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_NIL);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(array_mixed_types) {
    hotkv_reader_t *r = hotkv_reader_new();
    /* Array of [integer(1), bulk("two"), nil, simple("OK")] */
    const char *data = "*4\r\n:1\r\n$3\r\ntwo\r\n$-1\r\n+OK\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_ARRAY);
    ASSERT_EQ_INT(reply->nelem, 4);
    ASSERT_EQ_INT(reply->elements[0]->type, HOTKV_REPLY_INTEGER);
    ASSERT_EQ_INT(reply->elements[0]->integer, 1);
    ASSERT_EQ_INT(reply->elements[1]->type, HOTKV_REPLY_BULK);
    ASSERT_EQ_STR(reply->elements[1]->str, "two");
    ASSERT_EQ_INT(reply->elements[2]->type, HOTKV_REPLY_NIL);
    ASSERT_EQ_INT(reply->elements[3]->type, HOTKV_REPLY_STRING);
    ASSERT_EQ_STR(reply->elements[3]->str, "OK");

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(nested_array) {
    hotkv_reader_t *r = hotkv_reader_new();
    /* Array of [array[1, 2], array[3]] */
    const char *data = "*2\r\n*2\r\n:1\r\n:2\r\n*1\r\n:3\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_ARRAY);
    ASSERT_EQ_INT(reply->nelem, 2);
    ASSERT_EQ_INT(reply->elements[0]->type, HOTKV_REPLY_ARRAY);
    ASSERT_EQ_INT(reply->elements[0]->nelem, 2);
    ASSERT_EQ_INT(reply->elements[0]->elements[0]->integer, 1);
    ASSERT_EQ_INT(reply->elements[0]->elements[1]->integer, 2);
    ASSERT_EQ_INT(reply->elements[1]->type, HOTKV_REPLY_ARRAY);
    ASSERT_EQ_INT(reply->elements[1]->nelem, 1);
    ASSERT_EQ_INT(reply->elements[1]->elements[0]->integer, 3);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * Incremental (partial) feeds
 * ========================================================================= */

TEST(incremental_feed) {
    hotkv_reader_t *r = hotkv_reader_new();

    /* Feed byte by byte: +OK\r\n */
    const char *data = "+OK\r\n";
    for (size_t i = 0; i < strlen(data); i++) {
        hotkv_reader_feed(r, &data[i], 1);

        hotkv_reply_t *reply = NULL;
        int rc = hotkv_reader_next(r, &reply);
        ASSERT_EQ_INT(rc, HOTKV_OK);

        if (i < strlen(data) - 1) {
            ASSERT(reply == NULL); /* incomplete */
        } else {
            ASSERT(reply != NULL);
            ASSERT_EQ_INT(reply->type, HOTKV_REPLY_STRING);
            ASSERT_EQ_STR(reply->str, "OK");
            hotkv_free_reply(reply);
        }
    }

    hotkv_reader_free(r);
    PASS();
}

TEST(incremental_bulk_string) {
    hotkv_reader_t *r = hotkv_reader_new();

    /* Feed in pieces: "$5\r\n" then "hel" then "lo\r\n" */
    hotkv_reader_feed(r, "$5\r\n", 4);
    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply == NULL);

    hotkv_reader_feed(r, "hel", 3);
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply == NULL);

    hotkv_reader_feed(r, "lo\r\n", 4);
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_BULK);
    ASSERT_EQ_STR(reply->str, "hello");

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(incremental_array) {
    hotkv_reader_t *r = hotkv_reader_new();

    /* Feed partial array header. */
    hotkv_reader_feed(r, "*2\r\n$3\r\n", 8);

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply == NULL); /* incomplete - missing data */

    hotkv_reader_feed(r, "foo\r\n", 5);
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply == NULL); /* still incomplete - need second element */

    hotkv_reader_feed(r, "$3\r\nbar\r\n", 9);
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_ARRAY);
    ASSERT_EQ_INT(reply->nelem, 2);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * Multiple replies in one feed
 * ========================================================================= */

TEST(multiple_replies) {
    hotkv_reader_t *r = hotkv_reader_new();

    /* Two complete replies in one feed. */
    const char *data = "+OK\r\n:42\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_STRING);
    ASSERT_EQ_STR(reply->str, "OK");
    hotkv_free_reply(reply);

    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_INTEGER);
    ASSERT_EQ_INT(reply->integer, 42);
    hotkv_free_reply(reply);

    /* No more replies. */
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply == NULL);

    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP3: Null
 * ========================================================================= */

TEST(resp3_null) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "_\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_NIL);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP3: Boolean
 * ========================================================================= */

TEST(resp3_bool_true) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "#t\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_BOOL);
    ASSERT_EQ_INT(reply->integer, 1);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(resp3_bool_false) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "#f\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_BOOL);
    ASSERT_EQ_INT(reply->integer, 0);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP3: Double
 * ========================================================================= */

TEST(resp3_double) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = ",3.14\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_DOUBLE);
    ASSERT(fabs(reply->dval - 3.14) < 0.001);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(resp3_double_inf) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = ",inf\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_DOUBLE);
    ASSERT(!isfinite(reply->dval) && reply->dval > 0);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(resp3_double_neg_inf) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = ",-inf\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_DOUBLE);
    ASSERT(!isfinite(reply->dval) && reply->dval < 0);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP3: Big number
 * ========================================================================= */

TEST(resp3_bignum) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "(3492890328409238509324850943850943825024385\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_BIGNUM);
    ASSERT_EQ_STR(reply->str, "3492890328409238509324850943850943825024385");

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP3: Verbatim string
 * ========================================================================= */

TEST(resp3_verbatim) {
    hotkv_reader_t *r = hotkv_reader_new();
    /* =15\r\ntxt:Some string\r\n */
    const char *data = "=15\r\ntxt:Some string\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_VERB);
    ASSERT_EQ_STR(reply->vtype, "txt");
    ASSERT_EQ_STR(reply->str, "Some string");
    ASSERT_EQ_INT(reply->len, 11);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP3: Map
 * ========================================================================= */

TEST(resp3_map) {
    hotkv_reader_t *r = hotkv_reader_new();
    /* Map with 2 entries: "first" -> 1, "second" -> 2 */
    const char *data = "%2\r\n$5\r\nfirst\r\n:1\r\n$6\r\nsecond\r\n:2\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_MAP);
    ASSERT_EQ_INT(reply->nelem, 4); /* 2 pairs = 4 elements */
    ASSERT_EQ_STR(reply->elements[0]->str, "first");
    ASSERT_EQ_INT(reply->elements[1]->integer, 1);
    ASSERT_EQ_STR(reply->elements[2]->str, "second");
    ASSERT_EQ_INT(reply->elements[3]->integer, 2);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP3: Set
 * ========================================================================= */

TEST(resp3_set) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "~3\r\n$5\r\nhello\r\n$5\r\nworld\r\n:42\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_SET);
    ASSERT_EQ_INT(reply->nelem, 3);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * Paired replies (ZRANGE ... WITHSCORES, HRANDFIELD ... WITHVALUES, ...):
 * RESP3's array of 2-element pairs vs RESP2's flat alternating array.
 * ========================================================================= */

TEST(reply_pairs_resp3_nested) {
    hotkv_reader_t *r = hotkv_reader_new();
    /* RESP3 WITHSCORES shape: *2\r\n *2\r\n a 1 *2\r\n b 2 */
    const char *data = "*2\r\n*2\r\n$1\r\na\r\n,1\r\n*2\r\n$1\r\nb\r\n,2\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(hotkv_reply_pair_count(reply), 2);

    hotkv_reply_t *k = NULL, *v = NULL;
    ASSERT_EQ_INT(hotkv_reply_pair_at(reply, 0, &k, &v), 1);
    ASSERT_EQ_STR(k->str, "a");
    ASSERT(v->dval == 1.0);
    ASSERT_EQ_INT(hotkv_reply_pair_at(reply, 1, &k, &v), 1);
    ASSERT_EQ_STR(k->str, "b");
    ASSERT(v->dval == 2.0);
    ASSERT_EQ_INT(hotkv_reply_pair_at(reply, 2, &k, &v), 0);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(reply_pairs_resp2_flat) {
    hotkv_reader_t *r = hotkv_reader_new();
    /* RESP2 WITHSCORES shape: flat *4 [a, 1, b, 2] */
    const char *data = "*4\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$1\r\n2\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(hotkv_reply_pair_count(reply), 2);

    hotkv_reply_t *k = NULL, *v = NULL;
    ASSERT_EQ_INT(hotkv_reply_pair_at(reply, 0, &k, &v), 1);
    ASSERT_EQ_STR(k->str, "a");
    ASSERT_EQ_STR(v->str, "1");
    ASSERT_EQ_INT(hotkv_reply_pair_at(reply, 1, &k, &v), 1);
    ASSERT_EQ_STR(k->str, "b");
    ASSERT_EQ_STR(v->str, "2");

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP3: Push
 * ========================================================================= */

TEST(resp3_push) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = ">2\r\n$7\r\nmessage\r\n$5\r\nhello\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_PUSH);
    ASSERT_EQ_INT(reply->nelem, 2);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * RESP3: Attribute
 * ========================================================================= */

TEST(resp3_attribute) {
    hotkv_reader_t *r = hotkv_reader_new();
    /* Attribute with 1 pair: "key" -> "value" */
    const char *data = "|1\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_ATTR);
    ASSERT_EQ_INT(reply->nelem, 2); /* 1 pair = 2 elements */

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * Error conditions
 * ========================================================================= */

TEST(protocol_error_unknown_type) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "?invalid\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_ERR_PROTOCOL);
    ASSERT(reply == NULL);

    hotkv_reader_free(r);
    PASS();
}

TEST(protocol_error_invalid_integer) {
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = ":notanumber\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_ERR_PROTOCOL);
    ASSERT(reply == NULL);

    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * Reader lifecycle
 * ========================================================================= */

TEST(reader_reset) {
    hotkv_reader_t *r = hotkv_reader_new();

    /* Feed partial data, then reset. */
    hotkv_reader_feed(r, "+PA", 3);
    ASSERT(hotkv_reader_buffered(r) == 3);

    hotkv_reader_reset(r);
    ASSERT(hotkv_reader_buffered(r) == 0);

    /* Should work fine after reset. */
    hotkv_reader_feed(r, "+OK\r\n", 5);
    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_STR(reply->str, "OK");

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(reader_null_safety) {
    /* These should not crash. */
    hotkv_reader_free(NULL);
    hotkv_free_reply(NULL);
    ASSERT(hotkv_reader_buffered(NULL) == 0);
    PASS();
}

/* =========================================================================
 * Command formatter
 * ========================================================================= */

TEST(format_command_simple) {
    /*
     * "SET key value" should produce:
     * *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n
     */
    const char *argv[] = {"SET", "key", "value"};
    size_t lens[] = {3, 3, 5};

    hotkv_buf_t buf;
    ASSERT_EQ_INT(hotkv_buf_init(&buf, 256), HOTKV_OK);
    ASSERT_EQ_INT(hotkv_format_command_argv(&buf, 3, argv, lens), HOTKV_OK);

    const char *expected = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
    ASSERT_EQ_INT(buf.len, strlen(expected));
    ASSERT(memcmp(buf.data, expected, buf.len) == 0);

    hotkv_buf_destroy(&buf);
    PASS();
}

TEST(format_command_binary) {
    /* Binary-safe key with NUL byte */
    const char key[] = "ke\x00y";
    const char *argv[] = {"GET", key};
    size_t lens[] = {3, 4};

    hotkv_buf_t buf;
    ASSERT_EQ_INT(hotkv_buf_init(&buf, 256), HOTKV_OK);
    ASSERT_EQ_INT(hotkv_format_command_argv(&buf, 2, argv, lens), HOTKV_OK);

    /* *2\r\n$3\r\nGET\r\n$4\r\nke\0y\r\n */
    const char expected[] = "*2\r\n$3\r\nGET\r\n$4\r\nke\x00y\r\n";
    ASSERT_EQ_INT(buf.len, sizeof(expected) - 1);
    ASSERT(memcmp(buf.data, expected, buf.len) == 0);

    hotkv_buf_destroy(&buf);
    PASS();
}

/* =========================================================================
 * Realistic HotKV scenarios
 * ========================================================================= */

TEST(scan_reply) {
    /* SCAN returns an array of [cursor_string, array_of_keys] */
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "*2\r\n$1\r\n0\r\n*3\r\n$4\r\nkey1\r\n$4\r\nkey2\r\n$4\r\nkey3\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_ARRAY);
    ASSERT_EQ_INT(reply->nelem, 2);
    /* Cursor */
    ASSERT_EQ_INT(reply->elements[0]->type, HOTKV_REPLY_BULK);
    ASSERT_EQ_STR(reply->elements[0]->str, "0");
    /* Keys array */
    ASSERT_EQ_INT(reply->elements[1]->type, HOTKV_REPLY_ARRAY);
    ASSERT_EQ_INT(reply->elements[1]->nelem, 3);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

TEST(info_reply) {
    /* INFO returns a large bulk string */
    hotkv_reader_t *r = hotkv_reader_new();
    const char *data = "$27\r\n# Server\r\nhotkv_version:0.1\r\n";
    hotkv_reader_feed(r, data, strlen(data));

    hotkv_reply_t *reply = NULL;
    ASSERT_EQ_INT(hotkv_reader_next(r, &reply), HOTKV_OK);
    ASSERT(reply != NULL);
    ASSERT_EQ_INT(reply->type, HOTKV_REPLY_BULK);
    ASSERT_EQ_INT(reply->len, 27);
    ASSERT(strstr(reply->str, "hotkv_version") != NULL);

    hotkv_free_reply(reply);
    hotkv_reader_free(r);
    PASS();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
    printf("libhotkv RESP parser tests\n");
    printf("==========================\n\n");

    printf("RESP2 types:\n");
    run_test_simple_string();
    run_test_simple_string_pong();
    run_test_error();
    run_test_wrongtype_error();
    run_test_integer_positive();
    run_test_integer_negative();
    run_test_integer_zero();
    run_test_bulk_string();
    run_test_bulk_string_empty();
    run_test_null_bulk_string();
    run_test_bulk_string_binary();
    run_test_array_basic();
    run_test_array_empty();
    run_test_null_array();
    run_test_array_mixed_types();
    run_test_nested_array();

    printf("\nIncremental parsing:\n");
    run_test_incremental_feed();
    run_test_incremental_bulk_string();
    run_test_incremental_array();

    printf("\nMultiple replies:\n");
    run_test_multiple_replies();

    printf("\nRESP3 types:\n");
    run_test_resp3_null();
    run_test_resp3_bool_true();
    run_test_resp3_bool_false();
    run_test_resp3_double();
    run_test_resp3_double_inf();
    run_test_resp3_double_neg_inf();
    run_test_resp3_bignum();
    run_test_resp3_verbatim();
    run_test_resp3_map();
    run_test_resp3_set();
    run_test_reply_pairs_resp3_nested();
    run_test_reply_pairs_resp2_flat();
    run_test_resp3_push();
    run_test_resp3_attribute();

    printf("\nError conditions:\n");
    run_test_protocol_error_unknown_type();
    run_test_protocol_error_invalid_integer();

    printf("\nReader lifecycle:\n");
    run_test_reader_reset();
    run_test_reader_null_safety();

    printf("\nCommand formatter:\n");
    run_test_format_command_simple();
    run_test_format_command_binary();

    printf("\nRealistic scenarios:\n");
    run_test_scan_reply();
    run_test_info_reply();

    printf("\n==========================\n");
    printf("Results: %d passed, %d failed, %d total\n",
           g_tests_passed, g_tests_failed, g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
