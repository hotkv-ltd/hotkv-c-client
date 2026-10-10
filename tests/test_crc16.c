/**
 * @file test_crc16.c
 * @brief Unit tests for hotkv_crc16() and hotkv_key_slot() against the
 * known CLUSTER KEYSLOT vectors. No live server required.
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

static uint16_t slot_of(const char *key) {
    return hotkv_key_slot(key, strlen(key));
}

/* ========================================================================= */

TEST(raw_crc16_xmodem_check_value) {
    /* The well-known CRC-16/XMODEM check value. */
    ASSERT_EQ_INT(hotkv_crc16("123456789", 9), 0x31C3);
    PASS();
}

TEST(slot_foo) {
    ASSERT_EQ_INT(slot_of("foo"), 12182);
    PASS();
}

TEST(slot_bar) {
    ASSERT_EQ_INT(slot_of("bar"), 5061);
    PASS();
}

TEST(slot_hello) {
    ASSERT_EQ_INT(slot_of("hello"), 866);
    PASS();
}

TEST(hash_tag_user1000_following) {
    ASSERT_EQ_INT(slot_of("{user1000}.following"), 3443);
    PASS();
}

TEST(hash_tag_user1000_followers_same_slot) {
    /* Same {user1000} hash tag as the previous key must land in the same slot. */
    ASSERT_EQ_INT(slot_of("{user1000}.followers"), 3443);
    PASS();
}

TEST(empty_hash_tag_hashes_whole_key) {
    /* "{}" is an empty tag: the whole key "{}foo" is hashed instead. */
    ASSERT_EQ_INT(slot_of("{}foo"), 9500);
    PASS();
}

TEST(empty_first_tag_falls_back_to_whole_key) {
    ASSERT_EQ_INT(slot_of("foo{}{bar}"), 8363);
    PASS();
}

TEST(first_closing_brace_wins) {
    /* Tag is "{bar" (from the first '{' to the first '}' after it). */
    ASSERT_EQ_INT(slot_of("foo{{bar}}zap"), 4015);
    PASS();
}

TEST(hash_tag_matches_plain_bar) {
    /* Tag is "bar", same slot as hashing "bar" directly. */
    ASSERT_EQ_INT(slot_of("foo{bar}{zap}"), 5061);
    ASSERT_EQ_INT(slot_of("foo{bar}{zap}"), slot_of("bar"));
    PASS();
}

TEST(slot_in_range) {
    ASSERT_EQ_INT(slot_of("foo") < 16384, 1);
    ASSERT_EQ_INT(slot_of("") < 16384, 1);
    PASS();
}

int main(void) {
    printf("=== CRC16 / cluster key-slot tests ===\n\n");

    run_test_raw_crc16_xmodem_check_value();
    run_test_slot_foo();
    run_test_slot_bar();
    run_test_slot_hello();
    run_test_hash_tag_user1000_following();
    run_test_hash_tag_user1000_followers_same_slot();
    run_test_empty_hash_tag_hashes_whole_key();
    run_test_empty_first_tag_falls_back_to_whole_key();
    run_test_first_closing_brace_wins();
    run_test_hash_tag_matches_plain_bar();
    run_test_slot_in_range();

    printf("\n=================================\n");
    printf("Results: %d passed, %d failed, %d total\n", g_tests_passed, g_tests_failed,
           g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
