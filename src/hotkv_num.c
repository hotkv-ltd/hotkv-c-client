/**
 * @file hotkv_num.c
 * @brief Locale-independent number formatting for the wire.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * printf("%g") and friends honour the C locale of the host program: after
 * setlocale(LC_ALL, "de_DE.UTF-8") they print "0,5", which no server parses.
 * They also switch to exponent form ("1e-05", "1e+09"), which several HotKV
 * parsers (LLMBUDGET.SET cost limits, LLMSTATS.PRICE amounts) reject. The
 * functions here build the text from digits and an exponent, so the output
 * is always plain ASCII decimal: an optional minus sign, digits, and at most
 * one '.'.
 */

#include "hotkv_private.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NaN / infinity tests written without <math.h> macros: some C libraries
 * (MinGW) expand isnan()/isinf() into code that warns about double to float
 * conversions. */
static int hkv_isnan(double v) {
    return v != v;
}

static int hkv_isinf(double v) {
    return !hkv_isnan(v) && (v - v) != 0.0;
}

/*
 * Renders the shortest decimal string that reads back as the same double
 * (or float when is_float), without exponent. Returns the length, or 0 when
 * v is NaN or buf is too small.
 */
static size_t fmt_shortest(char *buf, size_t bufsz, double v, int is_float) {
    char tmp[64];
    char digits[32];
    size_t nd = 0;
    int exp10 = 0;
    int neg;
    int prec;
    size_t pos = 0;

    if (!buf || bufsz == 0 || hkv_isnan(v)) return 0;

    if (hkv_isinf(v)) {
        const char *s = v < 0 ? "-inf" : "inf";
        size_t n = strlen(s);
        if (n + 1 > bufsz) return 0;
        memcpy(buf, s, n + 1);
        return n;
    }

    if (v == 0.0) {
        if (bufsz < 2) return 0;
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }

    neg = v < 0;

    /* Smallest precision (significant digits) that round-trips. The check
     * uses strtod/strtof, which read the same locale snprintf wrote. */
    for (prec = 1; prec <= 17; prec++) {
        double back;
        snprintf(tmp, sizeof(tmp), "%.*e", prec - 1, v);
        if (is_float) {
            back = (double)strtof(tmp, NULL);
            if (back == (double)(float)v) break;
        } else {
            back = strtod(tmp, NULL);
            if (back == v) break;
        }
        if (is_float && prec >= 9) break;
    }
    if (prec > 17) snprintf(tmp, sizeof(tmp), "%.16e", v);

    /* tmp is [-]d[<decimal point>ddd]e[+-]XX. Collect the digits and the
     * exponent without assuming what the decimal point looks like. */
    {
        const char *p = tmp;
        const char *e;
        if (*p == '-') p++;
        for (e = p; *e && *e != 'e' && *e != 'E'; e++) {
            if (*e >= '0' && *e <= '9' && nd < sizeof(digits) - 1) digits[nd++] = *e;
        }
        if (!*e) return 0;
        exp10 = atoi(e + 1);
    }
    while (nd > 1 && digits[nd - 1] == '0') nd--;

    /* value = 0.d1d2d3... * 10^(exp10 + 1) */
    {
        long point = (long)exp10 + 1; /* digits before the decimal point */
        size_t need = (neg ? 1u : 0u) + 1u;
        if (point <= 0) {
            need += 2 + (size_t)(-point) + nd;      /* 0.000ddd */
        } else if ((size_t)point >= nd) {
            need += (size_t)point;                  /* ddd000 */
        } else {
            need += nd + 1;                         /* dd.ddd */
        }
        if (need > bufsz) return 0;

        if (neg) buf[pos++] = '-';
        if (point <= 0) {
            buf[pos++] = '0';
            buf[pos++] = '.';
            for (long i = 0; i < -point; i++) buf[pos++] = '0';
            memcpy(buf + pos, digits, nd);
            pos += nd;
        } else if ((size_t)point >= nd) {
            memcpy(buf + pos, digits, nd);
            pos += nd;
            for (size_t i = nd; i < (size_t)point; i++) buf[pos++] = '0';
        } else {
            memcpy(buf + pos, digits, (size_t)point);
            pos += (size_t)point;
            buf[pos++] = '.';
            memcpy(buf + pos, digits + point, nd - (size_t)point);
            pos += nd - (size_t)point;
        }
        buf[pos] = '\0';
    }
    return pos;
}

size_t hotkv_fmt_double(char *buf, size_t bufsz, double v) {
    return fmt_shortest(buf, bufsz, v, 0);
}

size_t hotkv_fmt_float(char *buf, size_t bufsz, float v) {
    return fmt_shortest(buf, bufsz, (double)v, 1);
}

/*
 * USD amounts: HotKV parses them as exact fixed point, [0-9]+(.[0-9]{1,6})?,
 * to the micro-dollar. The amount is rounded to the nearest micro-dollar and
 * printed with trailing zeros trimmed, using integer arithmetic only.
 */
size_t hotkv_fmt_usd(char *buf, size_t bufsz, double usd) {
    uint64_t micros, whole, frac;
    int n;
    size_t len;

    if (!buf || bufsz == 0) return 0;
    if (!(usd >= 0.0) || hkv_isinf(usd)) return 0; /* negative, NaN or infinite */
    if (usd > 18446744073709.0) return 0;      /* micro-dollars must fit in 64 bits */

    micros = (uint64_t)floor(usd * 1e6 + 0.5);
    whole = micros / 1000000u;
    frac = micros % 1000000u;

    if (frac == 0) {
        n = snprintf(buf, bufsz, "%llu", (unsigned long long)whole);
    } else {
        n = snprintf(buf, bufsz, "%llu.%06llu", (unsigned long long)whole,
                     (unsigned long long)frac);
    }
    if (n < 0 || (size_t)n >= bufsz) return 0;
    len = (size_t)n;
    if (frac != 0) {
        while (len > 0 && buf[len - 1] == '0') len--;
        buf[len] = '\0';
    }
    return len;
}
