/**
 * @file hotkv_cmd.h
 * @brief Internal argv builder shared by the typed command wrappers.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * Not part of the public API. Header-only (static inline) so each wrapper
 * file gets its own copy without exporting symbols.
 */

#ifndef HOTKV_CMD_H
#define HOTKV_CMD_H

#include "hotkv.h"
#include "hotkv_private.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Small growable argv/argvlen builder used by every wrapper below. Literal
 * keyword strings are safe to measure with strlen() (they are always our
 * own NUL-terminated C string literals, never user-supplied binary data).
 * ========================================================================= */

typedef struct {
    const char **argv;
    size_t *lens;
    size_t count;
    size_t cap;
    char **owned; /* heap strings freed at destroy time */
    size_t nowned;
    size_t owned_cap;
    int oom;
    int bad; /* an argument could not be represented (NaN, negative USD, ...) */
} hkv_cmd_t;

static inline void hkv_cmd_init(hkv_cmd_t *c) {
    c->argv = NULL;
    c->lens = NULL;
    c->count = 0;
    c->cap = 0;
    c->owned = NULL;
    c->nowned = 0;
    c->owned_cap = 0;
    c->oom = 0;
    c->bad = 0;
}

static inline void hkv_cmd_destroy(hkv_cmd_t *c) {
    size_t i;
    for (i = 0; i < c->nowned; i++) {
        hotkv_free(c->owned[i]);
    }
    hotkv_free(c->owned);
    hotkv_free((void *)c->argv);
    hotkv_free(c->lens);
    c->argv = NULL;
    c->lens = NULL;
    c->owned = NULL;
}

static inline int hkv_cmd_reserve(hkv_cmd_t *c) {
    if (c->count < c->cap) {
        return 1;
    }
    {
        size_t ncap = c->cap ? c->cap * 2 : 8;
        const char **a = (const char **)hotkv_realloc((void *)c->argv, ncap * sizeof(*a));
        size_t *l = (size_t *)hotkv_realloc(c->lens, ncap * sizeof(*l));
        if (!a || !l) {
            c->oom = 1;
            return 0;
        }
        c->argv = a;
        c->lens = l;
        c->cap = ncap;
    }
    return 1;
}

/* Append an argument that is not owned by the builder (literal or
 * caller-provided buffer that outlives the call). */
static inline void hkv_cmd_add(hkv_cmd_t *c, const char *s, size_t len) {
    if (c->oom || !hkv_cmd_reserve(c)) {
        return;
    }
    c->argv[c->count] = s;
    c->lens[c->count] = len;
    c->count++;
}

static inline void hkv_cmd_adds(hkv_cmd_t *c, const char *s) {
    hkv_cmd_add(c, s, strlen(s));
}

/* Track a heap string so it gets freed by hkv_cmd_destroy, then add it. */
static inline void hkv_cmd_add_owned(hkv_cmd_t *c, char *s, size_t len) {
    if (c->oom || !s) {
        hotkv_free(s);
        c->oom = c->oom || !s;
        return;
    }
    if (c->nowned >= c->owned_cap) {
        size_t ncap = c->owned_cap ? c->owned_cap * 2 : 8;
        char **o = (char **)hotkv_realloc(c->owned, ncap * sizeof(*o));
        if (!o) {
            hotkv_free(s);
            c->oom = 1;
            return;
        }
        c->owned = o;
        c->owned_cap = ncap;
    }
    c->owned[c->nowned++] = s;
    hkv_cmd_add(c, s, len);
}

/* printf-style formatted argument (numbers, mostly). */
static inline void hkv_cmd_addf(hkv_cmd_t *c, const char *fmt, ...) {
    char stackbuf[48];
    va_list ap;
    int n;

    if (c->oom) {
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        c->oom = 1;
        return;
    }
    if ((size_t)n < sizeof(stackbuf)) {
        hkv_cmd_add_owned(c, hotkv_strdup(stackbuf), (size_t)n);
        return;
    }
    {
        char *s = (char *)hotkv_malloc((size_t)n + 1);
        if (!s) {
            c->oom = 1;
            return;
        }
        va_start(ap, fmt);
        vsnprintf(s, (size_t)n + 1, fmt, ap);
        va_end(ap);
        hkv_cmd_add_owned(c, s, (size_t)n);
    }
}

/* Add a comma-separated float vector, e.g. for RAG VECTORS/VECTOR. */
static inline void hkv_cmd_add_csv_floats(hkv_cmd_t *c, const float *vec, size_t n) {
    size_t cap = n * (HOTKV_FMT_NUM_MAX < 24 ? HOTKV_FMT_NUM_MAX : 24) + 8;
    size_t off = 0;
    char *buf;

    if (c->oom) {
        return;
    }
    buf = (char *)hotkv_malloc(cap);
    if (!buf) {
        c->oom = 1;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        char num[HOTKV_FMT_NUM_MAX];
        size_t w = hotkv_fmt_float(num, sizeof(num), vec[i]);
        if (w == 0 || !isfinite((double)vec[i])) {
            hotkv_free(buf);
            c->bad = 1;
            return;
        }
        if (off + w + 2 > cap) {
            char *nb;
            cap = (off + w + 2) * 2;
            nb = (char *)hotkv_realloc(buf, cap);
            if (!nb) {
                hotkv_free(buf);
                c->oom = 1;
                return;
            }
            buf = nb;
        }
        if (i) buf[off++] = ',';
        memcpy(buf + off, num, w);
        off += w;
    }
    hkv_cmd_add_owned(c, buf, off);
}

/* Plain-decimal, locale-independent number arguments (see hotkv_num.c).
 * finite_only: the enterprise parsers use from_chars and refuse inf/nan, so
 * fail the command locally rather than send something the server rejects. */
static inline void hkv_cmd_add_double(hkv_cmd_t *c, double v, int finite_only) {
    char num[HOTKV_FMT_NUM_MAX];
    size_t n;
    if (c->oom || c->bad) return;
    n = hotkv_fmt_double(num, sizeof(num), v);
    if (n == 0 || (finite_only && !isfinite(v))) {
        c->bad = 1;
        return;
    }
    hkv_cmd_add_owned(c, hotkv_strdup(num), n);
}

static inline void hkv_cmd_add_float(hkv_cmd_t *c, float v) {
    char num[HOTKV_FMT_NUM_MAX];
    size_t n;
    if (c->oom || c->bad) return;
    n = hotkv_fmt_float(num, sizeof(num), v);
    if (n == 0 || !isfinite((double)v)) {
        c->bad = 1;
        return;
    }
    hkv_cmd_add_owned(c, hotkv_strdup(num), n);
}

/* A USD amount in the exact fixed-point form HotKV parses: plain decimal,
 * at most 6 fractional digits, never an exponent, never negative. */
static inline void hkv_cmd_add_usd(hkv_cmd_t *c, double usd) {
    char num[HOTKV_FMT_NUM_MAX];
    size_t n;
    if (c->oom || c->bad) return;
    n = hotkv_fmt_usd(num, sizeof(num), usd);
    if (n == 0) {
        c->bad = 1;
        return;
    }
    hkv_cmd_add_owned(c, hotkv_strdup(num), n);
}

static inline hotkv_reply_t *hkv_cmd_exec(hotkv_context_t *ctx, hkv_cmd_t *c) {
    hotkv_reply_t *r;
    if (c->oom) {
        hkv_cmd_destroy(c);
        hotkv_set_error(ctx, HOTKV_ERR_OOM, "out of memory building command");
        return NULL;
    }
    if (c->bad) {
        hkv_cmd_destroy(c);
        return hotkv_local_error_reply("ERR invalid numeric argument, nothing was sent");
    }
    r = hotkv_command_argv(ctx, (int)c->count, c->argv, c->lens);
    hkv_cmd_destroy(c);
    return r;
}


#endif /* HOTKV_CMD_H */
