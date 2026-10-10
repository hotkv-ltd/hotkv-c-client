/**
 * @file test_commands.c
 * @brief Unit tests for command formatting.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * Tests verify the RESP wire format produced by hotkv_format_command_argv()
 * using the exact argument patterns that each typed command function uses.
 * No live server is required.
 */

#include "hotkv.h"
#include "hotkv_private.h"

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

#define ASSERT_BUF_EQ(buf, expected, expected_len)            \
    do {                                                      \
        if ((buf).len != (size_t)(expected_len) ||            \
            memcmp((buf).data, (expected), (expected_len))) { \
            printf("FAIL\n    buffer mismatch at %s:%d\n",   \
                   __FILE__, __LINE__);                       \
            printf("    got %zu bytes, expected %zu\n",       \
                   (buf).len, (size_t)(expected_len));        \
            g_tests_failed++;                                 \
            return;                                           \
        }                                                     \
    } while (0)

#define PASS() do { printf("OK\n"); g_tests_passed++; } while (0)

/* =========================================================================
 * Helper: format and compare
 * ========================================================================= */

static int format_and_check(int argc, const char **argv,
                             const size_t *lens, const char *expected) {
    hotkv_buf_t buf;
    if (hotkv_buf_init(&buf, 256) != HOTKV_OK) return 0;
    if (hotkv_format_command_argv(&buf, argc, argv, lens) != HOTKV_OK) {
        hotkv_buf_destroy(&buf);
        return 0;
    }
    size_t elen = strlen(expected);
    int ok = (buf.len == elen && memcmp(buf.data, expected, elen) == 0);
    hotkv_buf_destroy(&buf);
    return ok;
}

/* =========================================================================
 * SET / GET / DEL
 * ========================================================================= */

TEST(cmd_set) {
    const char *argv[] = {"SET", "mykey", "myvalue"};
    size_t lens[] = {3, 5, 7};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n"));
    PASS();
}

TEST(cmd_get) {
    const char *argv[] = {"GET", "mykey"};
    size_t lens[] = {3, 5};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$3\r\nGET\r\n$5\r\nmykey\r\n"));
    PASS();
}

TEST(cmd_del_single) {
    const char *argv[] = {"DEL", "mykey"};
    size_t lens[] = {3, 5};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$3\r\nDEL\r\n$5\r\nmykey\r\n"));
    PASS();
}

TEST(cmd_del_multi) {
    const char *argv[] = {"DEL", "k1", "k2", "k3"};
    size_t lens[] = {3, 2, 2, 2};
    ASSERT(format_and_check(4, argv, lens,
        "*4\r\n$3\r\nDEL\r\n$2\r\nk1\r\n$2\r\nk2\r\n$2\r\nk3\r\n"));
    PASS();
}

/* =========================================================================
 * MSET / MGET
 * ========================================================================= */

TEST(cmd_mset) {
    const char *argv[] = {"MSET", "k1", "v1", "k2", "v2"};
    size_t lens[] = {4, 2, 2, 2, 2};
    ASSERT(format_and_check(5, argv, lens,
        "*5\r\n$4\r\nMSET\r\n$2\r\nk1\r\n$2\r\nv1\r\n"
        "$2\r\nk2\r\n$2\r\nv2\r\n"));
    PASS();
}

TEST(cmd_mget) {
    const char *argv[] = {"MGET", "k1", "k2"};
    size_t lens[] = {4, 2, 2};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$4\r\nMGET\r\n$2\r\nk1\r\n$2\r\nk2\r\n"));
    PASS();
}

/* =========================================================================
 * INCR / DECR / INCRBY / DECRBY / INCRBYFLOAT
 * ========================================================================= */

TEST(cmd_incr) {
    const char *argv[] = {"INCR", "counter"};
    size_t lens[] = {4, 7};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$4\r\nINCR\r\n$7\r\ncounter\r\n"));
    PASS();
}

TEST(cmd_incrby) {
    const char *argv[] = {"INCRBY", "counter", "10"};
    size_t lens[] = {6, 7, 2};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$6\r\nINCRBY\r\n$7\r\ncounter\r\n$2\r\n10\r\n"));
    PASS();
}

TEST(cmd_incrbyfloat) {
    const char *argv[] = {"INCRBYFLOAT", "counter", "3.14"};
    size_t lens[] = {11, 7, 4};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$11\r\nINCRBYFLOAT\r\n$7\r\ncounter\r\n$4\r\n3.14\r\n"));
    PASS();
}

/* =========================================================================
 * APPEND / STRLEN / GETRANGE / SETRANGE
 * ========================================================================= */

TEST(cmd_append) {
    const char *argv[] = {"APPEND", "mykey", "world"};
    size_t lens[] = {6, 5, 5};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$6\r\nAPPEND\r\n$5\r\nmykey\r\n$5\r\nworld\r\n"));
    PASS();
}

TEST(cmd_strlen) {
    const char *argv[] = {"STRLEN", "mykey"};
    size_t lens[] = {6, 5};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$6\r\nSTRLEN\r\n$5\r\nmykey\r\n"));
    PASS();
}

/* =========================================================================
 * SETEX / SETNX / PSETEX
 * ========================================================================= */

TEST(cmd_setex) {
    const char *argv[] = {"SETEX", "mykey", "60", "myvalue"};
    size_t lens[] = {5, 5, 2, 7};
    ASSERT(format_and_check(4, argv, lens,
        "*4\r\n$5\r\nSETEX\r\n$5\r\nmykey\r\n$2\r\n60\r\n$7\r\nmyvalue\r\n"));
    PASS();
}

TEST(cmd_setnx) {
    const char *argv[] = {"SETNX", "mykey", "myvalue"};
    size_t lens[] = {5, 5, 7};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$5\r\nSETNX\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n"));
    PASS();
}

/* =========================================================================
 * List commands: LPUSH / RPUSH / LPOP / RPOP / LLEN / LRANGE
 * ========================================================================= */

TEST(cmd_lpush) {
    const char *argv[] = {"LPUSH", "mylist", "a", "b", "c"};
    size_t lens[] = {5, 6, 1, 1, 1};
    ASSERT(format_and_check(5, argv, lens,
        "*5\r\n$5\r\nLPUSH\r\n$6\r\nmylist\r\n"
        "$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    PASS();
}

TEST(cmd_rpush) {
    const char *argv[] = {"RPUSH", "mylist", "x"};
    size_t lens[] = {5, 6, 1};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$5\r\nRPUSH\r\n$6\r\nmylist\r\n$1\r\nx\r\n"));
    PASS();
}

TEST(cmd_lpop) {
    const char *argv[] = {"LPOP", "mylist"};
    size_t lens[] = {4, 6};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$4\r\nLPOP\r\n$6\r\nmylist\r\n"));
    PASS();
}

TEST(cmd_lrange) {
    const char *argv[] = {"LRANGE", "mylist", "0", "-1"};
    size_t lens[] = {6, 6, 1, 2};
    ASSERT(format_and_check(4, argv, lens,
        "*4\r\n$6\r\nLRANGE\r\n$6\r\nmylist\r\n$1\r\n0\r\n$2\r\n-1\r\n"));
    PASS();
}

/* =========================================================================
 * Set commands: SADD / SREM / SMEMBERS / SISMEMBER / SCARD
 * ========================================================================= */

TEST(cmd_sadd) {
    const char *argv[] = {"SADD", "myset", "a", "b"};
    size_t lens[] = {4, 5, 1, 1};
    ASSERT(format_and_check(4, argv, lens,
        "*4\r\n$4\r\nSADD\r\n$5\r\nmyset\r\n$1\r\na\r\n$1\r\nb\r\n"));
    PASS();
}

TEST(cmd_smembers) {
    const char *argv[] = {"SMEMBERS", "myset"};
    size_t lens[] = {8, 5};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$8\r\nSMEMBERS\r\n$5\r\nmyset\r\n"));
    PASS();
}

/* =========================================================================
 * Hash commands: HSET / HGET / HDEL / HGETALL
 * ========================================================================= */

TEST(cmd_hset) {
    const char *argv[] = {"HSET", "myhash", "field1", "val1"};
    size_t lens[] = {4, 6, 6, 4};
    ASSERT(format_and_check(4, argv, lens,
        "*4\r\n$4\r\nHSET\r\n$6\r\nmyhash\r\n$6\r\nfield1\r\n$4\r\nval1\r\n"));
    PASS();
}

TEST(cmd_hget) {
    const char *argv[] = {"HGET", "myhash", "field1"};
    size_t lens[] = {4, 6, 6};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$4\r\nHGET\r\n$6\r\nmyhash\r\n$6\r\nfield1\r\n"));
    PASS();
}

TEST(cmd_hgetall) {
    const char *argv[] = {"HGETALL", "myhash"};
    size_t lens[] = {7, 6};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$7\r\nHGETALL\r\n$6\r\nmyhash\r\n"));
    PASS();
}

/* =========================================================================
 * Sorted set commands: ZADD / ZSCORE / ZRANGE
 * ========================================================================= */

TEST(cmd_zadd) {
    /* ZADD myzset 1.5 member1 */
    const char *argv[] = {"ZADD", "myzset", "1.5", "member1"};
    size_t lens[] = {4, 6, 3, 7};
    ASSERT(format_and_check(4, argv, lens,
        "*4\r\n$4\r\nZADD\r\n$6\r\nmyzset\r\n$3\r\n1.5\r\n$7\r\nmember1\r\n"));
    PASS();
}

TEST(cmd_zrange) {
    /* ZRANGE myzset 0 -1 WITHSCORES */
    const char *argv[] = {"ZRANGE", "myzset", "0", "-1", "WITHSCORES"};
    size_t lens[] = {6, 6, 1, 2, 10};
    ASSERT(format_and_check(5, argv, lens,
        "*5\r\n$6\r\nZRANGE\r\n$6\r\nmyzset\r\n$1\r\n0\r\n$2\r\n-1\r\n"
        "$10\r\nWITHSCORES\r\n"));
    PASS();
}

/* =========================================================================
 * Key commands: EXISTS / EXPIRE / TTL / TYPE / RENAME / KEYS / SCAN
 * ========================================================================= */

TEST(cmd_exists) {
    const char *argv[] = {"EXISTS", "mykey"};
    size_t lens[] = {6, 5};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$6\r\nEXISTS\r\n$5\r\nmykey\r\n"));
    PASS();
}

TEST(cmd_expire) {
    const char *argv[] = {"EXPIRE", "mykey", "100"};
    size_t lens[] = {6, 5, 3};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$6\r\nEXPIRE\r\n$5\r\nmykey\r\n$3\r\n100\r\n"));
    PASS();
}

TEST(cmd_ttl) {
    const char *argv[] = {"TTL", "mykey"};
    size_t lens[] = {3, 5};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$3\r\nTTL\r\n$5\r\nmykey\r\n"));
    PASS();
}

TEST(cmd_type) {
    const char *argv[] = {"TYPE", "mykey"};
    size_t lens[] = {4, 5};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$4\r\nTYPE\r\n$5\r\nmykey\r\n"));
    PASS();
}

TEST(cmd_scan) {
    /* SCAN 0 MATCH * COUNT 100 */
    const char *argv[] = {"SCAN", "0", "MATCH", "*", "COUNT", "100"};
    size_t lens[] = {4, 1, 5, 1, 5, 3};
    ASSERT(format_and_check(6, argv, lens,
        "*6\r\n$4\r\nSCAN\r\n$1\r\n0\r\n$5\r\nMATCH\r\n"
        "$1\r\n*\r\n$5\r\nCOUNT\r\n$3\r\n100\r\n"));
    PASS();
}

/* =========================================================================
 * Server commands: PING / ECHO / SELECT / DBSIZE / FLUSHDB / INFO
 * ========================================================================= */

TEST(cmd_ping) {
    const char *argv[] = {"PING"};
    size_t lens[] = {4};
    ASSERT(format_and_check(1, argv, lens,
        "*1\r\n$4\r\nPING\r\n"));
    PASS();
}

TEST(cmd_echo) {
    const char *argv[] = {"ECHO", "Hello World"};
    size_t lens[] = {4, 11};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$4\r\nECHO\r\n$11\r\nHello World\r\n"));
    PASS();
}

TEST(cmd_select) {
    const char *argv[] = {"SELECT", "5"};
    size_t lens[] = {6, 1};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$6\r\nSELECT\r\n$1\r\n5\r\n"));
    PASS();
}

TEST(cmd_info) {
    const char *argv[] = {"INFO", "server"};
    size_t lens[] = {4, 6};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$4\r\nINFO\r\n$6\r\nserver\r\n"));
    PASS();
}

/* =========================================================================
 * Transaction commands: MULTI / EXEC / DISCARD
 * ========================================================================= */

TEST(cmd_multi) {
    const char *argv[] = {"MULTI"};
    size_t lens[] = {5};
    ASSERT(format_and_check(1, argv, lens,
        "*1\r\n$5\r\nMULTI\r\n"));
    PASS();
}

TEST(cmd_exec) {
    const char *argv[] = {"EXEC"};
    size_t lens[] = {4};
    ASSERT(format_and_check(1, argv, lens,
        "*1\r\n$4\r\nEXEC\r\n"));
    PASS();
}

TEST(cmd_discard) {
    const char *argv[] = {"DISCARD"};
    size_t lens[] = {7};
    ASSERT(format_and_check(1, argv, lens,
        "*1\r\n$7\r\nDISCARD\r\n"));
    PASS();
}

/* =========================================================================
 * Pub/Sub: PUBLISH
 * ========================================================================= */

TEST(cmd_publish) {
    const char *argv[] = {"PUBLISH", "mychannel", "hello"};
    size_t lens[] = {7, 9, 5};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$7\r\nPUBLISH\r\n$9\r\nmychannel\r\n$5\r\nhello\r\n"));
    PASS();
}

/* =========================================================================
 * HotKV-exclusive: NEXPIRE / NTTL / TAG.SET / TAG.GET / HOTKEYS
 * ========================================================================= */

TEST(cmd_nexpire) {
    const char *argv[] = {"NEXPIRE", "mykey", "500000000"};
    size_t lens[] = {7, 5, 9};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$7\r\nNEXPIRE\r\n$5\r\nmykey\r\n$9\r\n500000000\r\n"));
    PASS();
}

TEST(cmd_nttl) {
    const char *argv[] = {"NTTL", "mykey"};
    size_t lens[] = {4, 5};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$4\r\nNTTL\r\n$5\r\nmykey\r\n"));
    PASS();
}

TEST(cmd_tag_set) {
    const char *argv[] = {"TAG.SET", "mykey", "hot", "fast"};
    size_t lens[] = {7, 5, 3, 4};
    ASSERT(format_and_check(4, argv, lens,
        "*4\r\n$7\r\nTAG.SET\r\n$5\r\nmykey\r\n$3\r\nhot\r\n$4\r\nfast\r\n"));
    PASS();
}

TEST(cmd_tag_get) {
    const char *argv[] = {"TAG.GET", "mykey"};
    size_t lens[] = {7, 5};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$7\r\nTAG.GET\r\n$5\r\nmykey\r\n"));
    PASS();
}

TEST(cmd_hotkeys_top) {
    const char *argv[] = {"HOTKEYS", "TOP", "10"};
    size_t lens[] = {7, 3, 2};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$7\r\nHOTKEYS\r\n$3\r\nTOP\r\n$2\r\n10\r\n"));
    PASS();
}

TEST(cmd_latency_latest) {
    const char *argv[] = {"LATENCY", "LATEST"};
    size_t lens[] = {7, 6};
    ASSERT(format_and_check(2, argv, lens,
        "*2\r\n$7\r\nLATENCY\r\n$6\r\nLATEST\r\n"));
    PASS();
}

/* =========================================================================
 * Edge cases
 * ========================================================================= */

TEST(cmd_empty_value) {
    const char *argv[] = {"SET", "mykey", ""};
    size_t lens[] = {3, 5, 0};
    ASSERT(format_and_check(3, argv, lens,
        "*3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$0\r\n\r\n"));
    PASS();
}

TEST(cmd_binary_key) {
    const char key[] = "k\x00y";
    const char *argv[] = {"GET", key};
    size_t lens[] = {3, 3};

    /* Cannot use format_and_check because expected contains NUL.
       Compare manually with explicit sizes. */
    hotkv_buf_t buf;
    ASSERT_EQ_INT(hotkv_buf_init(&buf, 256), HOTKV_OK);
    ASSERT_EQ_INT(hotkv_format_command_argv(&buf, 2, argv, lens), HOTKV_OK);

    const char expected[] = "*2\r\n$3\r\nGET\r\n$3\r\nk\x00y\r\n";
    size_t expected_len = sizeof(expected) - 1;
    ASSERT_EQ_INT(buf.len, (long long)expected_len);
    ASSERT(memcmp(buf.data, expected, expected_len) == 0);

    hotkv_buf_destroy(&buf);
    PASS();
}

TEST(cmd_null_lens_uses_strlen) {
    /* When argvlen is NULL, formatter should use strlen */
    const char *argv[] = {"SET", "mykey", "myvalue"};
    hotkv_buf_t buf;
    ASSERT_EQ_INT(hotkv_buf_init(&buf, 256), HOTKV_OK);
    ASSERT_EQ_INT(hotkv_format_command_argv(&buf, 3, argv, NULL), HOTKV_OK);

    const char *expected = "*3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n";
    ASSERT_BUF_EQ(buf, expected, strlen(expected));

    hotkv_buf_destroy(&buf);
    PASS();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
    printf("libhotkv command formatter tests\n");
    printf("=================================\n\n");

    printf("String commands:\n");
    run_test_cmd_set();
    run_test_cmd_get();
    run_test_cmd_del_single();
    run_test_cmd_del_multi();
    run_test_cmd_mset();
    run_test_cmd_mget();
    run_test_cmd_incr();
    run_test_cmd_incrby();
    run_test_cmd_incrbyfloat();
    run_test_cmd_append();
    run_test_cmd_strlen();
    run_test_cmd_setex();
    run_test_cmd_setnx();

    printf("\nList commands:\n");
    run_test_cmd_lpush();
    run_test_cmd_rpush();
    run_test_cmd_lpop();
    run_test_cmd_lrange();

    printf("\nSet commands:\n");
    run_test_cmd_sadd();
    run_test_cmd_smembers();

    printf("\nHash commands:\n");
    run_test_cmd_hset();
    run_test_cmd_hget();
    run_test_cmd_hgetall();

    printf("\nSorted set commands:\n");
    run_test_cmd_zadd();
    run_test_cmd_zrange();

    printf("\nKey commands:\n");
    run_test_cmd_exists();
    run_test_cmd_expire();
    run_test_cmd_ttl();
    run_test_cmd_type();
    run_test_cmd_scan();

    printf("\nServer commands:\n");
    run_test_cmd_ping();
    run_test_cmd_echo();
    run_test_cmd_select();
    run_test_cmd_info();

    printf("\nTransaction commands:\n");
    run_test_cmd_multi();
    run_test_cmd_exec();
    run_test_cmd_discard();

    printf("\nPub/Sub commands:\n");
    run_test_cmd_publish();

    printf("\nHotKV-exclusive commands:\n");
    run_test_cmd_nexpire();
    run_test_cmd_nttl();
    run_test_cmd_tag_set();
    run_test_cmd_tag_get();
    run_test_cmd_hotkeys_top();
    run_test_cmd_latency_latest();

    printf("\nEdge cases:\n");
    run_test_cmd_empty_value();
    run_test_cmd_binary_key();
    run_test_cmd_null_lens_uses_strlen();

    printf("\n=================================\n");
    printf("Results: %d passed, %d failed, %d total\n",
           g_tests_passed, g_tests_failed, g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
