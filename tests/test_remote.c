/**
 * @file test_remote.c
 * @brief What a customer does: connect to a public HotKV server over TLS with a
 * password and use it. Skipped unless HOTKV_REMOTE_HOST is set.
 *
 * Environment:
 *   HOTKV_REMOTE_HOST       server host or IP (required, else exit 77)
 *   HOTKV_REMOTE_PORT       TLS port (default 6379)
 *   HOTKV_REMOTE_PASSWORD   the server password (never printed, never stored)
 *   HOTKV_REMOTE_CA         CA certificate (PEM) that signed the server certificate
 *
 * The server is shared, so this test only touches names that start with
 * `sdktest:remote-c-<random>-`, removes them again, and never runs FLUSHALL,
 * FLUSHDB, CONFIG SET, ACL changes, SHUTDOWN or other global commands
 * (the core and enterprise suites run in their safe mode).
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "live_util.h"

#ifndef HOTKV_USE_SSL

int main(void) {
    printf("libhotkv was built without HOTKV_USE_SSL; the remote test needs TLS. Skipping.\n");
    return 77;
}

#else

#include "live_core_suite.h"
#include "live_enterprise_suite.h"

#ifdef _WIN32
#include <windows.h>
typedef HANDLE thread_t;
typedef struct {
    void (*fn)(void *);
    void *arg;
} thr_start;
static DWORD WINAPI thr_tramp(LPVOID p) {
    thr_start s = *(thr_start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
static int thread_start(thread_t *t, void (*fn)(void *), void *arg) {
    thr_start *s = (thr_start *)malloc(sizeof(*s));
    if (!s) return 1;
    s->fn = fn;
    s->arg = arg;
    *t = CreateThread(NULL, 0, thr_tramp, s, 0, NULL);
    return *t ? 0 : 1;
}
static void thread_join(thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
#include <pthread.h>
typedef pthread_t thread_t;
typedef struct {
    void (*fn)(void *);
    void *arg;
} thr_start;
static void *thr_tramp(void *p) {
    thr_start s = *(thr_start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
static int thread_start(thread_t *t, void (*fn)(void *), void *arg) {
    thr_start *s = (thr_start *)malloc(sizeof(*s));
    if (!s) return 1;
    s->fn = fn;
    s->arg = arg;
    return pthread_create(t, NULL, thr_tramp, s);
}
static void thread_join(thread_t t) {
    pthread_join(t, NULL);
}
#endif

static const char *g_host, *g_ca, *g_pw;
static int g_port;
static char g_url[512];

/* Percent-encodes a password for the userinfo part of a URL. */
static void pct_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

static hotkv_options_t base_opts(void) {
    hotkv_options_t o;
    memset(&o, 0, sizeof(o));
    o.tls_ca = g_ca;
    o.connect_timeout.tv_sec = 15;
    o.command_timeout.tv_sec = 30;
    return o;
}

/* ------------------------------------------------------------------------- */
static void test_connect_forms(void) {
    hotkv_options_t o = base_opts();
    hotkv_context_t *c;
    hotkv_reply_t *r;
    printf("-- connecting --\n");

    c = hotkv_connect_url(g_url, &o);
    CHECK(c && hotkv_context_err(c) == HOTKV_OK, "hotkvs:// URL with password and CA");
    if (c && hotkv_context_err(c) == HOTKV_OK) {
        expect_text(hotkv_ping(c), "PONG", "PING over the internet");
        r = hotkv_command(c, "ECHO %s", "hello from libhotkv");
        CHECK(rtext(r) && strcmp(rtext(r), "hello from libhotkv") == 0, "ECHO through hotkv_command");
        hotkv_free_reply(r);
    } else if (c) {
        fprintf(stderr, "    connect error: %s\n", hotkv_context_errstr(c));
    }
    hotkv_disconnect(c);

    o = base_opts();
    o.host = g_host;
    o.port = g_port;
    o.password = g_pw;
    o.use_tls = 1;
    c = hotkv_connect_with_options(&o);
    CHECK(c && hotkv_context_err(c) == HOTKV_OK, "options form (host, port, password, use_tls, tls_ca)");
    if (c && hotkv_context_err(c) == HOTKV_OK) expect_text(hotkv_ping(c), "PONG", "PING (options form)");
    hotkv_disconnect(c);

    o = base_opts();
    o.host = g_host;
    o.port = g_port;
    o.password = g_pw;
    o.protocol = HOTKV_RESP3;
    c = hotkv_connect_with_options(&o);
    CHECK(c && hotkv_context_err(c) == HOTKV_OK, "RESP3 over TLS (tls_ca implies use_tls)");
    if (c && hotkv_context_err(c) == HOTKV_OK) {
        r = hotkv_command(c, "HELLO");
        CHECK(r && (r->type == HOTKV_REPLY_MAP || r->type == HOTKV_REPLY_ARRAY), "HELLO reply");
        hotkv_free_reply(r);
    }
    hotkv_disconnect(c);

    /* the one insecure-skip-verify test */
    o = base_opts();
    o.host = g_host;
    o.port = g_port;
    o.password = g_pw;
    o.tls_ca = NULL;
    o.use_tls = 1;
    o.tls_insecure_skip_verify = 1;
    c = hotkv_connect_with_options(&o);
    CHECK(c && hotkv_context_err(c) == HOTKV_OK, "tls_insecure_skip_verify connects without the CA");
    if (c && hotkv_context_err(c) == HOTKV_OK) expect_text(hotkv_ping(c), "PONG", "PING (insecure)");
    hotkv_disconnect(c);

    /* without the CA and without skipping, verification must refuse the self-signed certificate */
    o = base_opts();
    o.host = g_host;
    o.port = g_port;
    o.tls_ca = NULL;
    o.use_tls = 1;
    c = hotkv_connect_with_options(&o);
    CHECK(c && hotkv_context_err(c) != HOTKV_OK, "self-signed certificate is refused without the CA");
    hotkv_disconnect(c);
}

static void test_auth_failures(void) {
    hotkv_options_t o = base_opts();
    hotkv_context_t *c;
    hotkv_reply_t *r;
    printf("-- authentication --\n");

    o.host = g_host;
    o.port = g_port;
    o.password = "this-is-not-the-password";
    c = hotkv_connect_with_options(&o);
    CHECK(c && hotkv_context_err(c) != HOTKV_OK, "wrong password fails the connect");
    CHECK(c && strstr(hotkv_context_errstr(c), "WRONGPASS") != NULL, "error says WRONGPASS");
    hotkv_disconnect(c);

    o.password = NULL;
    c = hotkv_connect_with_options(&o);
    CHECK(c && hotkv_context_err(c) == HOTKV_OK, "connecting without a password works (TLS only)");
    if (c && hotkv_context_err(c) == HOTKV_OK) {
        r = hotkv_ping(c);
        CHECK(r && r->type == HOTKV_REPLY_ERROR && hotkv_reply_is_noauth(r), "first command without AUTH is NOAUTH");
        hotkv_free_reply(r);
        r = hotkv_command(c, "AUTH %s", "nope");
        CHECK(r && r->type == HOTKV_REPLY_ERROR, "explicit AUTH with a wrong password is an error reply");
        hotkv_free_reply(r);
        r = hotkv_set(c, KL(key("noauth")), "x", 1);
        CHECK(r && r->type == HOTKV_REPLY_ERROR, "writes are refused while unauthenticated");
        hotkv_free_reply(r);
    }
    hotkv_disconnect(c);
}

static void test_basics(void) {
    hotkv_options_t o = base_opts();
    hotkv_context_t *c;
    char *s = key("str"), *h = key("hash"), *l = key("list"), *st = key("set"), *z = key("zset"), *ex = key("exp"),
         *big = key("big"), *tx = key("tx"), *pl = key("pipe");
    hotkv_reply_t *r;
    size_t i, n = 1024 * 1024;
    char *blob;
    printf("-- data basics over TLS --\n");

    c = hotkv_connect_url(g_url, &o);
    if (!c || hotkv_context_err(c) != HOTKV_OK) {
        fail("connect", "cannot connect");
        hotkv_disconnect(c);
        return;
    }
    expect_ok(hotkv_set(c, KL(s), "value", 5), "SET");
    expect_text(hotkv_get(c, KL(s)), "value", "GET");
    expect_int(hotkv_incr(c, KL(key("counter"))), 1, "INCR");
    expect_int(hotkv_hset(c, KL(h), ARR("a", "b"), (size_t[]){1, 1}, ARR("1", "2"), (size_t[]){1, 1}, 2), 2, "HSET");
    expect_text(hotkv_hget(c, KL(h), "b", 1), "2", "HGET");
    expect_int(hotkv_rpush(c, KL(l), ARR("x", "y", "z"), (size_t[]){1, 1, 1}, 3), 3, "RPUSH");
    expect_strs(hotkv_lrange(c, KL(l), 0, -1), 3, "LRANGE", "x", "y", "z");
    expect_int(hotkv_sadd(c, KL(st), ARR("m1", "m2"), (size_t[]){2, 2}, 2), 2, "SADD");
    expect_int(hotkv_scard(c, KL(st)), 2, "SCARD");
    expect_int(hotkv_zadd(c, KL(z), (double[]){1.5, 2.5}, ARR("a", "b"), (size_t[]){1, 1}, 2), 2, "ZADD");
    expect_strs(hotkv_zrange(c, KL(z), 0, -1, 0), 2, "ZRANGE", "a", "b");

    /* expiry */
    expect_ok(hotkv_setex(c, KL(ex), 1, "short", 5), "SETEX 1s");
    expect_text(hotkv_get(c, KL(ex)), "short", "GET before expiry");
#ifdef _WIN32
    Sleep(1700);
#else
    {
        struct timespec ts = {1, 700000000L};
        nanosleep(&ts, NULL);
    }
#endif
    expect_nil(hotkv_get(c, KL(ex)), "GET after expiry");

    /* pipeline */
    for (i = 0; i < 100; i++) {
        hotkv_append_command(c, "SET %s %d", pl, (int)i);
        hotkv_append_command(c, "GET %s", pl);
    }
    {
        int ok = 1;
        for (i = 0; i < 200; i++) {
            hotkv_reply_t *pr = NULL;
            char want[16];
            if (hotkv_get_reply(c, &pr) != HOTKV_OK || !pr) { ok = 0; break; }
            snprintf(want, sizeof(want), "%d", (int)(i / 2));
            if (i % 2 == 1 && (!rtext(pr) || strcmp(rtext(pr), want) != 0)) ok = 0;
            hotkv_free_reply(pr);
        }
        CHECK(ok, "200 pipelined commands");
    }

    /* MULTI / EXEC */
    expect_ok(hotkv_multi(c), "MULTI");
    expect_text(hotkv_command(c, "SET %s %s", tx, "t"), "QUEUED", "queued SET");
    expect_text(hotkv_command(c, "INCR %s", key("txc")), "QUEUED", "queued INCR");
    r = hotkv_exec(c);
    CHECK(is_agg(r) && r->nelem == 2 && strcmp(rtext(elem(r, 0)), "OK") == 0 && elem(r, 1)->integer == 1, "EXEC results");
    hotkv_free_reply(r);

    /* 1 MiB value, both directions */
    blob = (char *)malloc(n);
    for (i = 0; i < n; i++) blob[i] = (char)('A' + (i * 13 + i / 97) % 26);
    expect_ok(hotkv_set(c, KL(big), blob, n), "SET 1 MiB");
    r = hotkv_get(c, KL(big));
    CHECK(r && r->type == HOTKV_REPLY_BULK && r->len == n && memcmp(r->str, blob, n) == 0, "GET 1 MiB round trip");
    hotkv_free_reply(r);
    expect_int(hotkv_strlen(c, KL(big)), (long long)n, "STRLEN of the 1 MiB value");
    free(blob);

    /* clean up what this section made */
    {
        const char *ks[] = {s, h, l, st, z, ex, big, tx, pl, key("counter"), key("txc")};
        size_t kl[11];
        lens_of(ks, 11, kl);
        expect_int_at_least(hotkv_del(c, ks, kl, 11), 1, "DEL test keys");
    }
    hotkv_disconnect(c);
}

/* ---- concurrency: threads with their own connection, and a pool ---- */
static hotkv_pool_t *g_pool;
static char g_counter_key[200];
#define NTHREADS 4
#define NITER 25
static volatile int g_thread_errors;

static void worker_own_connection(void *arg) {
    hotkv_options_t o = base_opts();
    hotkv_context_t *c = hotkv_connect_url(g_url, &o);
    int i;
    (void)arg;
    if (!c || hotkv_context_err(c) != HOTKV_OK) {
        g_thread_errors++;
        hotkv_disconnect(c);
        return;
    }
    for (i = 0; i < NITER; i++) {
        hotkv_reply_t *r = hotkv_incr(c, g_counter_key, strlen(g_counter_key));
        if (!r || r->type != HOTKV_REPLY_INTEGER) g_thread_errors++;
        hotkv_free_reply(r);
    }
    hotkv_disconnect(c);
}

static void worker_pool(void *arg) {
    int i;
    (void)arg;
    for (i = 0; i < NITER; i++) {
        hotkv_context_t *c = hotkv_pool_acquire(g_pool);
        hotkv_reply_t *r;
        if (!c) { g_thread_errors++; continue; }
        r = hotkv_incr(c, g_counter_key, strlen(g_counter_key));
        if (!r || r->type != HOTKV_REPLY_INTEGER) g_thread_errors++;
        hotkv_free_reply(r);
        hotkv_pool_release(g_pool, c);
    }
}

static void test_concurrency(void) {
    thread_t th[NTHREADS];
    int i, started;
    hotkv_options_t o = base_opts();
    hotkv_context_t *c = hotkv_connect_url(g_url, &o);
    hotkv_pool_options_t po;
    printf("-- concurrent use --\n");
    if (!c || hotkv_context_err(c) != HOTKV_OK) {
        fail("connect", "cannot connect");
        hotkv_disconnect(c);
        return;
    }
    snprintf(g_counter_key, sizeof(g_counter_key), "%s", key("conc"));

    g_thread_errors = 0;
    for (i = 0, started = 0; i < NTHREADS; i++) if (thread_start(&th[i], worker_own_connection, NULL) == 0) started++;
    for (i = 0; i < started; i++) thread_join(th[i]);
    CHECK(started == NTHREADS && g_thread_errors == 0, "4 threads, one TLS connection each, no errors");
    expect_text(hotkv_get(c, g_counter_key, strlen(g_counter_key)), "100", "counter is exactly 4 x 25");

    memset(&po, 0, sizeof(po));
    po.conn_opts = base_opts();
    po.conn_opts.host = g_host;
    po.conn_opts.port = g_port;
    po.conn_opts.password = g_pw;
    po.conn_opts.use_tls = 1;
    po.min_connections = 2;
    po.max_connections = 3;
    g_pool = hotkv_pool_new(&po);
    CHECK(g_pool != NULL, "TLS connection pool");
    if (g_pool) {
        g_thread_errors = 0;
        for (i = 0, started = 0; i < NTHREADS; i++) if (thread_start(&th[i], worker_pool, NULL) == 0) started++;
        for (i = 0; i < started; i++) thread_join(th[i]);
        CHECK(started == NTHREADS && g_thread_errors == 0, "4 threads sharing a pool of 3 TLS connections");
        expect_text(hotkv_get(c, g_counter_key, strlen(g_counter_key)), "200", "counter is exactly 8 x 25");
        hotkv_pool_free(g_pool);
        g_pool = NULL;
    }
    expect_int(hotkv_del(c, ARR(g_counter_key), (size_t[]){strlen(g_counter_key)}, 1), 1, "DEL counter");
    hotkv_disconnect(c);
}

/* ---- the suites, in safe mode ---- */
static void test_suites(void) {
    hotkv_url_t u;
    printf("-- core and enterprise suites (safe mode) --\n");
    memset(&u, 0, sizeof(u));
    u.host = (char *)g_host;
    u.port = g_port;
    u.password = (char *)g_pw;
    u.use_tls = 1;
    live_core_run(&u, HOTKV_RESP2);
    live_core_run(&u, HOTKV_RESP3);
    live_enterprise_run(&u, HOTKV_RESP2);
    live_enterprise_run(&u, HOTKV_RESP3);
}

/* Removes whatever the run left under its prefix and reports leftovers. */
static void cleanup(void) {
    hotkv_options_t o = base_opts();
    hotkv_context_t *c = hotkv_connect_url(g_url, &o);
    char pattern[200];
    uint64_t cur = 0;
    size_t removed = 0;
    int rounds = 0, leftovers = 0;
    const char *lists[] = {"SCACHE.LIST", "PCACHE.LIST", "TOOL.CACHE.LIST", "NCACHE.LIST", "GUARD.LIST",
                           "RERANK.LIST", "LLMSTATS.LIST", "LLMBUDGET.LIST", "PROMPT.LIST", "EMBED.DEDUP.LIST",
                           "RAG.LIST", "RATELIMIT.LIST", "TS.LIST", "FEATURE.LIST", "VSIM.LIST", "AMEM.SUMMARY.LIST"};
    size_t i, j;
    printf("-- cleanup --\n");
    if (!c || hotkv_context_err(c) != HOTKV_OK) {
        fail("cleanup", "cannot connect");
        hotkv_disconnect(c);
        return;
    }
    snprintf(pattern, sizeof(pattern), "sdktest:%s*", g_run_prefix);
    do {
        hotkv_reply_t *r = hotkv_scan(c, cur, pattern, strlen(pattern), 500);
        if (!is_agg(r) || r->nelem != 2) { hotkv_free_reply(r); break; }
        cur = strtoull(rtext(elem(r, 0)), NULL, 10);
        if (is_agg(elem(r, 1)) && elem(r, 1)->nelem) {
            const hotkv_reply_t *ks = elem(r, 1);
            const char **kp = (const char **)calloc(ks->nelem, sizeof(char *));
            size_t *kl = (size_t *)calloc(ks->nelem, sizeof(size_t));
            for (j = 0; j < ks->nelem; j++) { kp[j] = ks->elements[j]->str; kl[j] = ks->elements[j]->len; }
            hotkv_free_reply(hotkv_unlink(c, kp, kl, ks->nelem));
            removed += ks->nelem;
            free(kp);
            free(kl);
        }
        hotkv_free_reply(r);
    } while (cur != 0 && ++rounds < 1000);
    printf("  removed %zu leftover keys\n", removed);
    /* The suites create plain keys and leave them; removing them is this runner's job. */

    for (i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        hotkv_reply_t *r = hotkv_command(c, lists[i]);
        for (j = 0; is_agg(r) && j < r->nelem; j++) {
            const char *name = rtext(elem(r, j));
            if (name && strstr(name, g_run_prefix)) {
                leftovers++;
                printf("  leftover in %s: %s\n", lists[i], name);
            }
        }
        hotkv_free_reply(r);
    }
    CHECK(leftovers == 0, "the suites left no namespaces, buckets or series behind");
    hotkv_disconnect(c);
}

int main(void) {
    const char *host = getenv("HOTKV_REMOTE_HOST");
    const char *port = getenv("HOTKV_REMOTE_PORT");
    char enc[400];
    unsigned seed;
    if (!host || !*host) {
        printf("HOTKV_REMOTE_HOST not set; skipping the remote test.\n");
        return 77;
    }
    g_host = host;
    g_port = (port && *port) ? atoi(port) : 6379;
    g_ca = getenv("HOTKV_REMOTE_CA");
    g_pw = getenv("HOTKV_REMOTE_PASSWORD");
    if (g_ca && !*g_ca) g_ca = NULL;
    if (g_pw && !*g_pw) g_pw = NULL;
    if (!g_pw) {
        fprintf(stderr, "HOTKV_REMOTE_PASSWORD is not set\n");
        return 1;
    }
    pct_encode(g_pw, enc, sizeof(enc));
    snprintf(g_url, sizeof(g_url), "hotkvs://:%s@%s:%d", enc, g_host, g_port);

    seed = (unsigned)time(NULL) ^ ((unsigned)hkv_getpid() << 16);
    snprintf(g_run_prefix, sizeof(g_run_prefix), "remote-c-%08x-", seed);
    live_init_run_id("t");
    g_safe = 1;
    g_tls_ca = g_ca;
    printf("Remote test against %s:%d (TLS%s), prefix %s\n", g_host, g_port, g_ca ? ", CA verified" : "", g_run_prefix);

    test_connect_forms();
    test_auth_failures();
    test_basics();
    test_concurrency();
    test_suites();
    cleanup();

    free_keys();
    printf("\n=================================\n");
    printf("Results: %d ok, %d skipped, %d failed\n", g_ok, g_skipped, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#endif /* HOTKV_USE_SSL */
