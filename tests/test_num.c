/**
 * @file test_num.c
 * @brief Unit tests for number formatting on the wire and for the printf-style
 * command formatter. No live server is required.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "hotkv.h"
#include "hotkv_private.h"
#include "hkv_test.h"

#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

static const char *fd(double v) {
    static char buf[HOTKV_FMT_NUM_MAX];
    return hotkv_fmt_double(buf, sizeof(buf), v) ? buf : NULL;
}
static const char *ff(float v) {
    static char buf[HOTKV_FMT_NUM_MAX];
    return hotkv_fmt_float(buf, sizeof(buf), v) ? buf : NULL;
}
static const char *fu(double v) {
    static char buf[HOTKV_FMT_NUM_MAX];
    return hotkv_fmt_usd(buf, sizeof(buf), v) ? buf : NULL;
}

TEST(double_plain_values) {
    ASSERT_STR_EQ(fd(0.0), "0");
    ASSERT_STR_EQ(fd(-0.0), "0");
    ASSERT_STR_EQ(fd(1.0), "1");
    ASSERT_STR_EQ(fd(-1.0), "-1");
    ASSERT_STR_EQ(fd(0.5), "0.5");
    ASSERT_STR_EQ(fd(-2.25), "-2.25");
    ASSERT_STR_EQ(fd(123.456), "123.456");
    ASSERT_STR_EQ(fd(100.0), "100");
    ASSERT_STR_EQ(fd(1234567890123.0), "1234567890123");
    PASS();
}

TEST(double_never_uses_exponent) {
    ASSERT_STR_EQ(fd(1e-5), "0.00001");
    ASSERT_STR_EQ(fd(1.5e-7), "0.00000015");
    ASSERT_STR_EQ(fd(-1e-5), "-0.00001");
    ASSERT_STR_EQ(fd(1e9), "1000000000");
    ASSERT_STR_EQ(fd(1e21), "1000000000000000000000");
    ASSERT_STR_EQ(fd(-1.5e10), "-15000000000");
    PASS();
}

TEST(double_tiny_and_huge_round_trip) {
    double samples[] = {1e-300, 5e-324, DBL_MIN, DBL_MAX, -DBL_MAX, 1e300, 0.1, 0.2 + 0.1,
                        1.0 / 3.0, 123456789.123456789, 2.2250738585072014e-308};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        const char *s = fd(samples[i]);
        ASSERT(s != NULL);
        ASSERT(strpbrk(s, "eE+") == NULL);
        ASSERT(strtod(s, NULL) == samples[i]);
    }
    PASS();
}

TEST(double_special_values) {
    char tiny[3];
    ASSERT_STR_EQ(fd(INFINITY), "inf");
    ASSERT_STR_EQ(fd(-INFINITY), "-inf");
    ASSERT(fd(NAN) == NULL);
    ASSERT_EQ_INT(hotkv_fmt_double(tiny, sizeof(tiny), 12345.0), 0);
    PASS();
}

TEST(float_shortest) {
    ASSERT_STR_EQ(ff(0.1f), "0.1");
    ASSERT_STR_EQ(ff(1.0f), "1");
    ASSERT_STR_EQ(ff(-0.25f), "-0.25");
    ASSERT_STR_EQ(ff(1e-5f), "0.00001");
    ASSERT_STR_EQ(ff(16777216.0f), "16777216");
    ASSERT(strtof(ff(3.14159274f), NULL) == 3.14159274f);
    ASSERT(strtof(ff(FLT_MIN), NULL) == FLT_MIN);
    ASSERT(strtof(ff(FLT_MAX), NULL) == FLT_MAX);
    PASS();
}

TEST(usd_fixed_point) {
    ASSERT_STR_EQ(fu(0.0), "0");
    ASSERT_STR_EQ(fu(1.0), "1");
    ASSERT_STR_EQ(fu(10.0), "10");
    ASSERT_STR_EQ(fu(0.5), "0.5");
    ASSERT_STR_EQ(fu(12.34), "12.34");
    ASSERT_STR_EQ(fu(1e-5), "0.00001");
    ASSERT_STR_EQ(fu(1e-6), "0.000001");
    ASSERT_STR_EQ(fu(0.1 + 0.2), "0.3");
    ASSERT_STR_EQ(fu(1e9), "1000000000");
    ASSERT_STR_EQ(fu(123456.789012), "123456.789012");
    /* Sub micro-dollar amounts round to the nearest micro-dollar. */
    ASSERT_STR_EQ(fu(4e-7), "0");
    ASSERT_STR_EQ(fu(6e-7), "0.000001");
    PASS();
}

TEST(usd_rejects_what_the_server_rejects) {
    ASSERT(fu(-0.01) == NULL);
    ASSERT(fu(-1e-9) == NULL);
    ASSERT(fu(NAN) == NULL);
    ASSERT(fu(INFINITY) == NULL);
    ASSERT(fu(1e20) == NULL);
    PASS();
}

TEST(formats_survive_comma_locale) {
    const char *names[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8",
                           "de_DE", "fr_FR", NULL};
    const char *used = NULL;
    char probe[32];
    const char *a;
    for (int i = 0; names[i]; i++) {
        if (setlocale(LC_ALL, names[i])) { used = names[i]; break; }
    }
    if (!used) {
        printf("SKIP (no comma-decimal locale installed)\n");
        g_tests_passed++;
        return;
    }
    snprintf(probe, sizeof(probe), "%.1f", 0.5);
    if (strchr(probe, ',') == NULL) {
        setlocale(LC_ALL, "C");
        printf("SKIP (locale %s does not use a decimal comma)\n", used);
        g_tests_passed++;
        return;
    }
    a = fd(0.5);
    if (!a || strchr(a, ',') != NULL || strcmp(a, "0.5") != 0) {
        setlocale(LC_ALL, "C");
        printf("FAIL\n    fd(0.5) = %s under %s\n", a ? a : "(null)", used);
        g_tests_failed++;
        return;
    }
    ASSERT_STR_EQ(fd(-1234.5678), "-1234.5678");
    ASSERT_STR_EQ(fd(1e-5), "0.00001");
    ASSERT_STR_EQ(ff(0.25f), "0.25");
    ASSERT_STR_EQ(fu(12.5), "12.5");
    ASSERT_STR_EQ(fu(0.000123), "0.000123");
    setlocale(LC_ALL, "C");
    printf("OK (%s)\n", used);
    g_tests_passed++;
}

/* ------------------------------------------------------------------ */
/* printf-style command formatter                                      */

static int fmtcmd(hotkv_buf_t *buf, const char *format, ...) {
    va_list ap;
    int rc;
    va_start(ap, format);
    rc = hotkv_format_command(buf, format, ap);
    va_end(ap);
    return rc;
}

static int wire_is(const hotkv_buf_t *b, const char *expected, size_t n) {
    return b->len == n && memcmp(b->data, expected, n) == 0;
}
#define WIRE_IS(b, lit) wire_is((b), (lit), sizeof(lit) - 1)

TEST(command_basic_conversions) {
    hotkv_buf_t b;
    ASSERT_EQ_INT(hotkv_buf_init(&b, 0), HOTKV_OK);
    ASSERT_EQ_INT(fmtcmd(&b, "SET %s %s", "k", "v"), HOTKV_OK);
    ASSERT(WIRE_IS(&b, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"));
    hotkv_buf_destroy(&b);
    PASS();
}

TEST(command_binary_and_numbers) {
    hotkv_buf_t b;
    const char bin[] = {'a', '\0', 'b'};
    ASSERT_EQ_INT(hotkv_buf_init(&b, 0), HOTKV_OK);
    ASSERT_EQ_INT(fmtcmd(&b, "SET %b %d %lld", bin, (size_t)3, -7, -1234567890LL), HOTKV_OK);
    ASSERT(WIRE_IS(&b, "*4\r\n$3\r\nSET\r\n$3\r\na\0b\r\n$2\r\n-7\r\n$11\r\n-1234567890\r\n"));
    hotkv_buf_destroy(&b);
    PASS();
}

TEST(command_concatenates_inside_a_word) {
    hotkv_buf_t b;
    ASSERT_EQ_INT(hotkv_buf_init(&b, 0), HOTKV_OK);
    ASSERT_EQ_INT(fmtcmd(&b, "GET user:%s:%d 100%%", "bob", 7), HOTKV_OK);
    ASSERT(WIRE_IS(&b, "*3\r\n$3\r\nGET\r\n$10\r\nuser:bob:7\r\n$4\r\n100%\r\n"));
    hotkv_buf_destroy(&b);
    PASS();
}

TEST(command_empty_string_is_an_argument) {
    hotkv_buf_t b;
    ASSERT_EQ_INT(hotkv_buf_init(&b, 0), HOTKV_OK);
    ASSERT_EQ_INT(fmtcmd(&b, "SET k %s", ""), HOTKV_OK);
    ASSERT(WIRE_IS(&b, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$0\r\n\r\n"));
    hotkv_buf_destroy(&b);
    PASS();
}

TEST(command_more_than_sixteen_arguments) {
    hotkv_buf_t b;
    char want[512];
    size_t n = 0;
    int i;
    ASSERT_EQ_INT(hotkv_buf_init(&b, 0), HOTKV_OK);
    ASSERT_EQ_INT(fmtcmd(&b, "RPUSH l %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d "
                         "%s %s %s",
                         1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                         "x", "y", "z"), HOTKV_OK);
    n += (size_t)sprintf(want + n, "*25\r\n$5\r\nRPUSH\r\n$1\r\nl\r\n");
    for (i = 1; i <= 20; i++) {
        n += (size_t)sprintf(want + n, "$%d\r\n%d\r\n", i < 10 ? 1 : 2, i);
    }
    n += (size_t)sprintf(want + n, "$1\r\nx\r\n$1\r\ny\r\n$1\r\nz\r\n");
    ASSERT(wire_is(&b, want, n));
    hotkv_buf_destroy(&b);
    PASS();
}

TEST(command_rejects_bad_formats_without_touching_the_buffer) {
    hotkv_buf_t b;
    ASSERT_EQ_INT(hotkv_buf_init(&b, 0), HOTKV_OK);
    ASSERT(fmtcmd(&b, "SET k %") != HOTKV_OK);         /* trailing percent */
    ASSERT(fmtcmd(&b, "SET k %x", 1) != HOTKV_OK);     /* unknown conversion */
    ASSERT(fmtcmd(&b, "SET k %s", (const char *)NULL) != HOTKV_OK);
    ASSERT(fmtcmd(&b, "   ") != HOTKV_OK);             /* no words at all */
    ASSERT_EQ_INT(b.len, 0);
    hotkv_buf_destroy(&b);
    PASS();
}

TEST(command_does_not_leak_number_temporaries) {
    /* Run under ASan/valgrind: %d and %lld used to allocate a temporary per
     * conversion that was never freed. */
    hotkv_buf_t b;
    int i;
    ASSERT_EQ_INT(hotkv_buf_init(&b, 0), HOTKV_OK);
    for (i = 0; i < 1000; i++) {
        b.len = 0;
        ASSERT_EQ_INT(fmtcmd(&b, "INCRBY k %d %lld", i, (long long)i * 3), HOTKV_OK);
    }
    hotkv_buf_destroy(&b);
    PASS();
}

int main(void) {
    printf("Number formatting and command formatter tests\n");
    run_test_double_plain_values();
    run_test_double_never_uses_exponent();
    run_test_double_tiny_and_huge_round_trip();
    run_test_double_special_values();
    run_test_float_shortest();
    run_test_usd_fixed_point();
    run_test_usd_rejects_what_the_server_rejects();
    run_test_formats_survive_comma_locale();
    run_test_command_basic_conversions();
    run_test_command_binary_and_numbers();
    run_test_command_concatenates_inside_a_word();
    run_test_command_empty_string_is_an_argument();
    run_test_command_more_than_sixteen_arguments();
    run_test_command_rejects_bad_formats_without_touching_the_buffer();
    run_test_command_does_not_leak_number_temporaries();
    HKV_TEST_SUMMARY();
    return g_tests_failed > 0 ? 1 : 0;
}
