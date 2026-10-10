/**
 * @file live_util.h
 * @brief Shared helpers for the tests that talk to a real HotKV server
 * (test_live_core.c, test_live_enterprise.c, test_remote.c).
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * Each helper takes ownership of the reply it is given, frees it, and records
 * a pass or a failure with the description, so a test line is one call:
 *
 *     expect_int(hotkv_llen(ctx, k, kl), 3, "LLEN");
 */

#ifndef HKV_LIVE_UTIL_H
#define HKV_LIVE_UTIL_H

#include "hotkv.h"
#include "hotkv_private.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#define hkv_getpid _getpid
#else
#include <unistd.h>
#define hkv_getpid getpid
#endif

#if defined(__GNUC__) || defined(__clang__)
#define HKV_MAYBE_UNUSED __attribute__((unused))
#else
#define HKV_MAYBE_UNUSED
#endif

static int g_ok = 0;
static int g_failed = 0;
static int g_skipped = 0;
static char g_run_id[96];

/* Connection state shared by the suites. */
static hotkv_context_t *ctx HKV_MAYBE_UNUSED;
static int g_proto HKV_MAYBE_UNUSED;
static const hotkv_url_t *g_u HKV_MAYBE_UNUSED;

/* Safe mode (HOTKV_LIVE_SAFE=1): skip the few operations that change server
 * wide state (FLUSHDB, SCRIPT FLUSH, resetting HOTKEYS / LATENCY statistics,
 * FLUSHALL), for shared servers. Everything else stays inside
 * names that carry a unique run id and is removed again. */
static int g_safe = 0;

/* CA file for hotkvs:// URLs; falls back to HOTKV_TEST_TLS_CA. */
static const char *g_tls_ca HKV_MAYBE_UNUSED;


/* Optional prefix put in front of every run id, so a runner can find (and remove)
 * everything it created on a shared server: HOTKV_LIVE_PREFIX or g_run_prefix. */
static char g_run_prefix[48] = "";

static inline void live_init_run_id(const char *tag) {
    const char *safe = getenv("HOTKV_LIVE_SAFE");
    const char *pre = getenv("HOTKV_LIVE_PREFIX");
    if (safe && *safe == '1') g_safe = 1;
    if (pre && *pre && !g_run_prefix[0]) snprintf(g_run_prefix, sizeof(g_run_prefix), "%s", pre);
    snprintf(g_run_id, sizeof(g_run_id), "%s%s-%ld-%d", g_run_prefix, tag, (long)time(NULL), (int)hkv_getpid());
}

static inline void pass(void) {
    g_ok++;
}

static inline void fail(const char *what, const char *fmt, ...) {
    va_list ap;
    g_failed++;
    printf("  FAIL %s: ", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static inline void skip(const char *what, const char *why) {
    g_skipped++;
    printf("  SKIP %s (%s)\n", what, why);
}

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (cond) {                                                        \
            pass();                                                        \
        } else {                                                           \
            fail(what, "condition `%s` is false (line %d)", #cond, __LINE__); \
        }                                                                  \
    } while (0)

/* ---- key bookkeeping: every key is released at the end of the run ---- */
static char **g_keys = NULL;
static size_t g_nkeys = 0;
static size_t g_keycap = 0;

static inline char *key(const char *suffix) {
    char buf[300];
    char *k;
    snprintf(buf, sizeof(buf), "sdktest:%s:%s", g_run_id, suffix);
    k = hotkv_strdup(buf);
    if (g_nkeys == g_keycap) {
        size_t ncap = g_keycap ? g_keycap * 2 : 256;
        char **nk = (char **)realloc(g_keys, ncap * sizeof(char *));
        if (!nk) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        g_keys = nk;
        g_keycap = ncap;
    }
    g_keys[g_nkeys++] = k;
    return k;
}

static inline void free_keys(void) {
    size_t i;
    for (i = 0; i < g_nkeys; i++) hotkv_free(g_keys[i]);
    free(g_keys);
    g_keys = NULL;
    g_nkeys = g_keycap = 0;
}

/* KL(k) expands to the pair "k, strlen(k)" used by most calls. */
#define KL(k) (k), strlen(k)

/* ---- reply inspection ---- */
static inline const char *type_name(int t) {
    switch (t) {
    case HOTKV_REPLY_STRING: return "simple-string";
    case HOTKV_REPLY_ERROR: return "error";
    case HOTKV_REPLY_INTEGER: return "integer";
    case HOTKV_REPLY_BULK: return "bulk";
    case HOTKV_REPLY_NIL: return "nil";
    case HOTKV_REPLY_ARRAY: return "array";
    case HOTKV_REPLY_MAP: return "map";
    case HOTKV_REPLY_SET: return "set";
    case HOTKV_REPLY_DOUBLE: return "double";
    case HOTKV_REPLY_BOOL: return "bool";
    case HOTKV_REPLY_PUSH: return "push";
    case HOTKV_REPLY_VERB: return "verbatim";
    default: return "other";
    }
}

/* Text of a scalar reply (bulk, simple, verbatim, bignum), or NULL. */
static inline const char *rtext(const hotkv_reply_t *r) {
    if (!r) return NULL;
    if (r->type == HOTKV_REPLY_BULK || r->type == HOTKV_REPLY_STRING ||
        r->type == HOTKV_REPLY_VERB || r->type == HOTKV_REPLY_BIGNUM)
        return r->str ? r->str : "";
    return NULL;
}

static inline int is_agg(const hotkv_reply_t *r) {
    return r && (r->type == HOTKV_REPLY_ARRAY || r->type == HOTKV_REPLY_SET ||
                 r->type == HOTKV_REPLY_MAP || r->type == HOTKV_REPLY_PUSH);
}

static inline const hotkv_reply_t *elem(const hotkv_reply_t *r, size_t i) {
    return (r && r->elements && i < r->nelem) ? r->elements[i] : NULL;
}

/* A reply as a double: accepts DOUBLE, INTEGER and numeric text. */
static inline int rdouble(const hotkv_reply_t *r, double *out) {
    const char *t;
    char *end;
    if (!r) return 0;
    if (r->type == HOTKV_REPLY_DOUBLE) { *out = r->dval; return 1; }
    if (r->type == HOTKV_REPLY_INTEGER) { *out = (double)r->integer; return 1; }
    t = rtext(r);
    if (!t || !*t) return 0;
    *out = strtod(t, &end);
    return *end == '\0';
}

static inline int rint64(const hotkv_reply_t *r, long long *out) {
    const char *t;
    char *end;
    if (!r) return 0;
    if (r->type == HOTKV_REPLY_INTEGER || r->type == HOTKV_REPLY_BOOL) { *out = r->integer; return 1; }
    t = rtext(r);
    if (!t || !*t) return 0;
    *out = strtoll(t, &end, 10);
    return *end == '\0';
}

/* Describes a reply for failure messages (short). */
static inline void describe(const hotkv_reply_t *r, char *buf, size_t n) {
    if (!r) { snprintf(buf, n, "NULL (transport error)"); return; }
    switch (r->type) {
    case HOTKV_REPLY_INTEGER: snprintf(buf, n, "integer %lld", (long long)r->integer); break;
    case HOTKV_REPLY_BULK:
    case HOTKV_REPLY_STRING:
    case HOTKV_REPLY_ERROR:
    case HOTKV_REPLY_VERB:
        snprintf(buf, n, "%s \"%.80s\"", type_name(r->type), r->str ? r->str : "");
        break;
    case HOTKV_REPLY_DOUBLE: snprintf(buf, n, "double %.17g", r->dval); break;
    case HOTKV_REPLY_ARRAY:
    case HOTKV_REPLY_SET:
    case HOTKV_REPLY_MAP:
    case HOTKV_REPLY_PUSH: snprintf(buf, n, "%s of %zu", type_name(r->type), r->nelem); break;
    default: snprintf(buf, n, "%s", type_name(r->type)); break;
    }
}

static inline void expect_int(hotkv_reply_t *r, long long want, const char *what) {
    long long got;
    char d[160];
    if (r && (r->type == HOTKV_REPLY_INTEGER || r->type == HOTKV_REPLY_BOOL) && r->integer == want) {
        pass();
    } else if (r && r->type == HOTKV_REPLY_DOUBLE && (long long)r->dval == want) {
        pass();
    } else {
        describe(r, d, sizeof(d));
        got = r ? r->integer : 0;
        (void)got;
        fail(what, "want integer %lld, got %s", want, d);
    }
    hotkv_free_reply(r);
}

static inline void expect_int_at_least(hotkv_reply_t *r, long long min, const char *what) {
    char d[160];
    if (r && r->type == HOTKV_REPLY_INTEGER && r->integer >= min) {
        pass();
    } else {
        describe(r, d, sizeof(d));
        fail(what, "want integer >= %lld, got %s", min, d);
    }
    hotkv_free_reply(r);
}

static inline void expect_ok(hotkv_reply_t *r, const char *what) {
    char d[160];
    if (r && r->type == HOTKV_REPLY_STRING && r->str && strcmp(r->str, "OK") == 0) {
        pass();
    } else {
        describe(r, d, sizeof(d));
        fail(what, "want +OK, got %s", d);
    }
    hotkv_free_reply(r);
}

/* Simple or bulk string with exact text. */
static inline void expect_text(hotkv_reply_t *r, const char *want, const char *what) {
    char d[160];
    const char *t = rtext(r);
    if (t && strcmp(t, want) == 0) {
        pass();
    } else {
        describe(r, d, sizeof(d));
        fail(what, "want \"%s\", got %s", want, d);
    }
    hotkv_free_reply(r);
}

static inline void expect_bin(hotkv_reply_t *r, const char *want, size_t wantlen, const char *what) {
    char d[160];
    if (r && rtext(r) && r->len == wantlen && memcmp(r->str, want, wantlen) == 0) {
        pass();
    } else {
        describe(r, d, sizeof(d));
        fail(what, "want %zu bytes, got %s", wantlen, d);
    }
    hotkv_free_reply(r);
}

static inline void expect_nil(hotkv_reply_t *r, const char *what) {
    char d[160];
    if (r && r->type == HOTKV_REPLY_NIL) {
        pass();
    } else {
        describe(r, d, sizeof(d));
        fail(what, "want nil, got %s", d);
    }
    hotkv_free_reply(r);
}

/* Error reply whose text starts with prefix. */
static inline void expect_err(hotkv_reply_t *r, const char *prefix, const char *what) {
    char d[160];
    if (r && r->type == HOTKV_REPLY_ERROR && r->str && strncmp(r->str, prefix, strlen(prefix)) == 0) {
        pass();
    } else {
        describe(r, d, sizeof(d));
        fail(what, "want error starting \"%s\", got %s", prefix, d);
    }
    hotkv_free_reply(r);
}

static inline void expect_double(hotkv_reply_t *r, double want, double tol, const char *what) {
    double got;
    char d[160];
    if (rdouble(r, &got) && got >= want - tol && got <= want + tol) {
        pass();
    } else {
        describe(r, d, sizeof(d));
        fail(what, "want %.10g (+-%g), got %s", want, tol, d);
    }
    hotkv_free_reply(r);
}

/* Array whose elements are bulk/simple strings, in order. */
static inline void expect_strs(hotkv_reply_t *r, size_t n, const char *what, ...) {
    va_list ap;
    size_t i;
    char d[160];
    int good = is_agg(r) && r->nelem == n;
    va_start(ap, what);
    for (i = 0; i < n; i++) {
        const char *want = va_arg(ap, const char *);
        const char *got = good ? rtext(elem(r, i)) : NULL;
        if (good && (!got || strcmp(got, want) != 0)) {
            describe(elem(r, i), d, sizeof(d));
            fail(what, "element %zu: want \"%s\", got %s", i, want, d);
            good = 0;
            break;
        }
    }
    va_end(ap);
    if (good) {
        pass();
    } else if (!is_agg(r) || r->nelem != n) {
        describe(r, d, sizeof(d));
        fail(what, "want array of %zu, got %s", n, d);
    }
    hotkv_free_reply(r);
}

/* Array/set of strings compared without regard to order. */
static inline void expect_strset(hotkv_reply_t *r, size_t n, const char *what, ...) {
    va_list ap;
    size_t i, j;
    char d[160];
    int good = (is_agg(r)) && r->nelem == n;
    unsigned char *used = (unsigned char *)calloc(n ? n : 1, 1);
    if (!used) exit(1);
    va_start(ap, what);
    for (i = 0; i < n; i++) {
        const char *want = va_arg(ap, const char *);
        int found = 0;
        if (!good) continue;
        for (j = 0; j < n; j++) {
            const char *got = rtext(elem(r, j));
            if (!used[j] && got && strcmp(got, want) == 0) { used[j] = 1; found = 1; break; }
        }
        if (!found) {
            fail(what, "missing member \"%s\"", want);
            good = -1;
            break;
        }
    }
    va_end(ap);
    free(used);
    if (good == 1) {
        pass();
    } else if (good == 0) {
        describe(r, d, sizeof(d));
        fail(what, "want %zu members, got %s", n, d);
    }
    hotkv_free_reply(r);
}

/* Array of integers in order. */
static inline void expect_ints(hotkv_reply_t *r, size_t n, const char *what, ...) {
    va_list ap;
    size_t i;
    char d[160];
    int good = is_agg(r) && r->nelem == n;
    va_start(ap, what);
    for (i = 0; i < n; i++) {
        long long want = va_arg(ap, long long);
        long long got;
        if (good && (!rint64(elem(r, i), &got) || got != want)) {
            describe(elem(r, i), d, sizeof(d));
            fail(what, "element %zu: want %lld, got %s", i, want, d);
            good = 0;
            break;
        }
    }
    va_end(ap);
    if (good) {
        pass();
    } else if (!is_agg(r) || r->nelem != n) {
        describe(r, d, sizeof(d));
        fail(what, "want array of %zu, got %s", n, d);
    }
    hotkv_free_reply(r);
}

/* Free a reply only if it is an error-free value; records failure for errors. */
static inline void expect_noerr(hotkv_reply_t *r, const char *what) {
    char d[160];
    if (r && r->type != HOTKV_REPLY_ERROR) {
        pass();
    } else {
        describe(r, d, sizeof(d));
        fail(what, "unexpected %s", d);
    }
    hotkv_free_reply(r);
}

/* Convenience for tests that build "k1, k2" argument arrays. */
#define ARR(...) ((const char *[]){__VA_ARGS__})

static inline size_t *lens_of(const char **items, size_t n, size_t *buf) {
    size_t i;
    for (i = 0; i < n; i++) buf[i] = strlen(items[i]);
    return buf;
}

/* Connects with the URL from HOTKV_TEST_URL; returns NULL (and a message) on failure. */
static inline hotkv_context_t *live_connect(const hotkv_url_t *u, int protocol) {
    hotkv_options_t opts;
    hotkv_context_t *c;
    memset(&opts, 0, sizeof(opts));
    opts.host = u->host;
    opts.port = u->port;
    opts.username = u->username;
    opts.password = u->password;
    opts.db = u->db;
    opts.protocol = protocol;
    opts.command_timeout.tv_sec = 10;
    opts.connect_timeout.tv_sec = 10;
#ifdef HOTKV_USE_SSL
    if (u->use_tls) {
        /* hotkvs:// URL: HOTKV_TEST_TLS_CA names the CA, or HOTKV_TEST_TLS_INSECURE=1
         * skips verification (testing only). */
        const char *ca = g_tls_ca ? g_tls_ca : getenv("HOTKV_TEST_TLS_CA");
        const char *insecure = getenv("HOTKV_TEST_TLS_INSECURE");
        opts.use_tls = 1;
        if (ca && *ca) opts.tls_ca = ca;
        if (insecure && *insecure == '1') opts.tls_insecure_skip_verify = 1;
    }
#endif
    c = hotkv_connect_with_options(&opts);
    if (!c || hotkv_context_err(c) != HOTKV_OK) {
        fprintf(stderr, "connect failed: %s\n", c ? hotkv_context_errstr(c) : "out of memory");
        if (c) hotkv_disconnect(c);
        return NULL;
    }
    return c;
}

/* Prints a reply tree on one line (debugging aid; HOTKV_SHOW=1). */
static inline void dump_reply(const hotkv_reply_t *r, int depth) {
    size_t i;
    if (!r) { printf("NULL"); return; }
    switch (r->type) {
    case HOTKV_REPLY_INTEGER: printf("%lld", (long long)r->integer); break;
    case HOTKV_REPLY_DOUBLE: printf("%.10g", r->dval); break;
    case HOTKV_REPLY_NIL: printf("nil"); break;
    case HOTKV_REPLY_BOOL: printf(r->integer ? "true" : "false"); break;
    case HOTKV_REPLY_ARRAY: case HOTKV_REPLY_SET: case HOTKV_REPLY_MAP: case HOTKV_REPLY_PUSH:
        printf("%s[", r->type == HOTKV_REPLY_MAP ? "map" : "");
        for (i = 0; i < r->nelem && i < 40; i++) {
            if (i) printf(", ");
            dump_reply(r->elements[i], depth + 1);
        }
        printf("]");
        break;
    default: printf("%s:\"%.100s\"", type_name(r->type), r->str ? r->str : ""); break;
    }
}

static inline hotkv_reply_t *show(const char *what, hotkv_reply_t *r) {
    const char *e = getenv("HOTKV_SHOW");
    if (e && *e == '1') {
        printf("  [%s] => ", what);
        dump_reply(r, 0);
        printf("\n");
    }
    return r;
}
#define SHOW(expr) show(#expr, (expr))

#endif /* HKV_LIVE_UTIL_H */
