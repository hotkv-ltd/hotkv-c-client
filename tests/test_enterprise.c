/**
 * @file test_enterprise.c
 * @brief Unit tests for the typed-error-prefix helpers used by the
 * enterprise/AI command wrappers (hotkv_reply_is_nolicense() and friends),
 * plus exact wire argument lists for the shared
 * token usage group and every new option (captured with a loopback fake
 * server; no HotKV server involved), and by-field-name parsing of
 * hand-built RESP2 flat array / RESP3 map replies for
 * LLMSTATS.PRICE and the other changed STATS/GET/STATUS replies.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "hotkv.h"
#include "hotkv_private.h"

#ifdef _WIN32
/* No pthreads on MSVC: a minimal shim over CreateThread for this file. */
#include <stdlib.h>
typedef HANDLE pthread_t;
typedef struct {
    void *(*fn)(void *);
    void *arg;
} hkv_thread_start;
static DWORD WINAPI hkv_thread_tramp(LPVOID p) {
    hkv_thread_start s = *(hkv_thread_start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
static int pthread_create(pthread_t *t, void *attr, void *(*fn)(void *), void *arg) {
    hkv_thread_start *s = (hkv_thread_start *)malloc(sizeof(*s));
    (void)attr;
    if (!s) return 1;
    s->fn = fn;
    s->arg = arg;
    *t = CreateThread(NULL, 0, hkv_thread_tramp, s, 0, NULL);
    return *t ? 0 : 1;
}
static int pthread_join(pthread_t t, void **ret) {
    (void)ret;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}
#else
#include <pthread.h>
#endif
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

#define ASSERT_STR_EQ(a, b)                                                        \
    do {                                                                           \
        const char *_a = (a);                                                      \
        const char *_b = (b);                                                      \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                   \
            printf("FAIL\n    %s == \"%s\", expected \"%s\" at %s:%d\n", #a,      \
                   _a ? _a : "(null)", _b ? _b : "(null)", __FILE__, __LINE__);    \
            g_tests_failed++;                                                      \
            return;                                                                \
        }                                                                          \
    } while (0)

#define ASSERT_DBL_EQ(a, b)                                                       \
    do {                                                                          \
        double _a = (double)(a);                                                  \
        double _b = (double)(b);                                                  \
        double _diff = _a > _b ? _a - _b : _b - _a;                               \
        if (_diff > 1e-9) {                                                       \
            printf("FAIL\n    %s == %.9g, expected %.9g at %s:%d\n", #a, _a, _b,  \
                   __FILE__, __LINE__);                                           \
            g_tests_failed++;                                                     \
            return;                                                               \
        }                                                                         \
    } while (0)

#define PASS() do { printf("OK\n"); g_tests_passed++; } while (0)

/* ========================================================================= */

static hotkv_reply_t make_error(const char *text) {
    hotkv_reply_t r;
    memset(&r, 0, sizeof(r));
    r.type = HOTKV_REPLY_ERROR;
    r.str = (char *)text;
    r.len = strlen(text);
    return r;
}

/* =========================================================================
 * Hand-built pair-shaped replies (RESP2 flat array or RESP3 map: both are
 * hotkv_reply_t with type ARRAY/MAP and a flat 2*N elements array, see
 * hotkv_reply_t's own docs), for testing hotkv_parse_* by-field-name
 * decoding without any bytes ever touching a socket.
 * ========================================================================= */

#define HKV_MAX_FIELDS 24

typedef struct {
    hotkv_reply_t keys[HKV_MAX_FIELDS];
    hotkv_reply_t vals[HKV_MAX_FIELDS];
    hotkv_reply_t *elements[HKV_MAX_FIELDS * 2];
    hotkv_reply_t reply;
} hkv_pairs_t;

static void hkv_pairs_init(hkv_pairs_t *b, int as_map) {
    memset(b, 0, sizeof(*b));
    b->reply.type = as_map ? HOTKV_REPLY_MAP : HOTKV_REPLY_ARRAY;
    b->reply.elements = b->elements;
}

static void hkv_pairs_add_str(hkv_pairs_t *b, const char *name, const char *val) {
    size_t i = b->reply.nelem / 2;
    b->keys[i].type = HOTKV_REPLY_BULK;
    b->keys[i].str = (char *)name;
    b->keys[i].len = strlen(name);
    b->vals[i].type = HOTKV_REPLY_BULK;
    b->vals[i].str = (char *)val;
    b->vals[i].len = strlen(val);
    b->elements[i * 2] = &b->keys[i];
    b->elements[i * 2 + 1] = &b->vals[i];
    b->reply.nelem += 2;
}

static void hkv_pairs_add_int(hkv_pairs_t *b, const char *name, int64_t val) {
    size_t i = b->reply.nelem / 2;
    b->keys[i].type = HOTKV_REPLY_BULK;
    b->keys[i].str = (char *)name;
    b->keys[i].len = strlen(name);
    b->vals[i].type = HOTKV_REPLY_INTEGER;
    b->vals[i].integer = val;
    b->elements[i * 2] = &b->keys[i];
    b->elements[i * 2 + 1] = &b->vals[i];
    b->reply.nelem += 2;
}

/* =========================================================================
 * Loopback fake server: proves the exact wire argv a typed command sends,
 * for the token usage group and the newer options. The
 * real client code (this codebase's own hotkv_connect()/hotkv_command_argv())
 * runs on the calling thread; a background thread plays the "server":
 * accept the one connection, decode the one command sent with a real
 * hotkv_reader_t (so argv is exactly what the wire carried, not a guess),
 * hand it to the caller, and write back a canned raw reply. Neither side
 * ever negotiates HELLO/AUTH/SELECT (hotkv_connect() with a fresh, all-zero
 * hotkv_options_t sends none of those), so the first bytes the fake reads
 * are always the one command under test.
 * ========================================================================= */

typedef struct {
    hotkv_fd_t listen_fd;
    int port;
    const char *raw_reply;
    size_t raw_reply_len;
    char captured[16][256];
    size_t captured_argc;
    int ok;
} hkv_fake_t;

static void *hkv_fake_thread(void *arg) {
    hkv_fake_t *f = (hkv_fake_t *)arg;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    hotkv_fd_t conn = accept(f->listen_fd, (struct sockaddr *)&addr, &alen);
    hotkv_reader_t *reader;
    hotkv_reply_t *cmd = NULL;
    char buf[4096];

    if (conn == HOTKV_INVALID_FD) {
        f->ok = 0;
        return NULL;
    }
    reader = hotkv_reader_new();
    for (;;) {
        if (hotkv_reader_next(reader, &cmd) != HOTKV_OK) {
            f->ok = 0;
            goto done;
        }
        if (cmd) {
            break;
        }
        {
            hotkv_ssize_t n = recv(conn, buf, sizeof(buf), 0);
            if (n <= 0 || hotkv_reader_feed(reader, buf, (size_t)n) != HOTKV_OK) {
                f->ok = 0;
                goto done;
            }
        }
    }
    f->captured_argc = cmd->nelem;
    for (size_t i = 0; i < cmd->nelem && i < 16; i++) {
        snprintf(f->captured[i], sizeof(f->captured[i]), "%s",
                 cmd->elements[i]->str ? cmd->elements[i]->str : "");
    }
    hotkv_free_reply(cmd);
    send(conn, f->raw_reply, f->raw_reply_len, 0);
    f->ok = 1;
done:
    hotkv_reader_free(reader);
    hotkv_close_socket(conn);
    return NULL;
}

/* Binds an ephemeral loopback port and starts listening (does not accept
 * yet; that happens on hkv_fake_thread once a wire test connects). */
static int hkv_fake_start(hkv_fake_t *f, const char *raw_reply, size_t raw_reply_len) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    memset(f, 0, sizeof(*f));
    if (hotkv_net_init() != HOTKV_OK) {
        return 0; /* Winsock must be started before the first socket() on Windows */
    }
    f->raw_reply = raw_reply;
    f->raw_reply_len = raw_reply_len;

    f->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (f->listen_fd == HOTKV_INVALID_FD) {
        return 0;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(f->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(f->listen_fd, 1) != 0 ||
        getsockname(f->listen_fd, (struct sockaddr *)&addr, &alen) != 0) {
        hotkv_close_socket(f->listen_fd);
        return 0;
    }
    f->port = ntohs(addr.sin_port);
    return 1;
}

static void hkv_fake_stop(hkv_fake_t *f) {
    hotkv_close_socket(f->listen_fd);
}

TEST(nolicense_detected) {
    hotkv_reply_t r =
        make_error("NOLICENSE enterprise feature 'prompt-cache' requires a valid license");
    ASSERT(hotkv_reply_is_nolicense(&r));
    PASS();
}

TEST(nolicense_prefix_must_be_whole_word) {
    /* "NOLICENSED" must NOT match the "NOLICENSE" prefix check. */
    hotkv_reply_t r = make_error("NOLICENSED something else entirely");
    ASSERT(!hotkv_reply_is_nolicense(&r));
    PASS();
}

TEST(non_error_reply_never_matches) {
    hotkv_reply_t r;
    memset(&r, 0, sizeof(r));
    r.type = HOTKV_REPLY_STRING;
    r.str = (char *)"NOLICENSE";
    r.len = strlen(r.str);
    ASSERT(!hotkv_reply_is_nolicense(&r));
    PASS();
}

TEST(moved_ask_wrongtype_noauth_readonly) {
    hotkv_reply_t moved = make_error("MOVED 3999 127.0.0.1:6400");
    hotkv_reply_t ask = make_error("ASK 3999 127.0.0.1:6400");
    hotkv_reply_t wrongtype = make_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    hotkv_reply_t noauth = make_error("NOAUTH Authentication required.");
    hotkv_reply_t readonly = make_error("READONLY You can't write against a read only replica.");

    ASSERT(hotkv_reply_is_moved(&moved));
    ASSERT(!hotkv_reply_is_ask(&moved));

    ASSERT(hotkv_reply_is_ask(&ask));
    ASSERT(!hotkv_reply_is_moved(&ask));

    ASSERT(hotkv_reply_is_wrongtype(&wrongtype));
    ASSERT(hotkv_reply_is_noauth(&noauth));
    ASSERT(hotkv_reply_is_readonly(&readonly));
    PASS();
}

TEST(error_prefix_extraction) {
    hotkv_reply_t r = make_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    char buf[32];
    ASSERT_STR_EQ(hotkv_reply_error_prefix(&r, buf, sizeof(buf)), "WRONGTYPE");
    PASS();
}

TEST(error_prefix_truncates_to_fit) {
    hotkv_reply_t r = make_error("SOMEVERYLONGPREFIXWORDINDEED rest of message");
    char buf[8];
    const char *out = hotkv_reply_error_prefix(&r, buf, sizeof(buf));
    ASSERT(out == buf);
    ASSERT_EQ_INT(strlen(buf), sizeof(buf) - 1);
    PASS();
}

TEST(error_prefix_single_word_no_space) {
    hotkv_reply_t r = make_error("SOMEERROR");
    char buf[32];
    ASSERT_STR_EQ(hotkv_reply_error_prefix(&r, buf, sizeof(buf)), "SOMEERROR");
    PASS();
}

TEST(null_safety) {
    ASSERT(!hotkv_reply_is_nolicense(NULL));
    ASSERT(!hotkv_reply_has_error_prefix(NULL, "X"));
    ASSERT(hotkv_reply_error_prefix(NULL, NULL, 0) == NULL);
    PASS();
}

/* =========================================================================
 * Exact wire argument lists, via the loopback fake
 * server, for the token usage group and every new option.
 * =========================================================================
 */

TEST(wire_scache_set_no_usage) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    float vec[2] = {0.5f, 0.25f};
    hotkv_reply_t *r;

    ASSERT(hkv_fake_start(&f, "+OK\r\n", 5));
    pthread_t th;
    ASSERT(pthread_create(&th, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);

    r = hotkv_scache_set(ctx, "n", 1, "q", 1, "r", 1, vec, 2, 0, NULL);
    hotkv_free_reply(r);
    pthread_join(th, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);

    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 6);
    ASSERT_STR_EQ(f.captured[0], "SCACHE.SET");
    ASSERT_STR_EQ(f.captured[4], "0.5");
    ASSERT_STR_EQ(f.captured[5], "0.25");
    PASS();
}

TEST(wire_scache_set_with_usage) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    float vec[2] = {0.5f, 0.25f};
    hotkv_token_usage_t usage;
    hotkv_reply_t *r;

    memset(&usage, 0, sizeof(usage));
    usage.model = "gpt-4";
    usage.cache_write_5m_tokens = 7;

    ASSERT(hkv_fake_start(&f, "+OK\r\n", 5));
    pthread_t th;
    ASSERT(pthread_create(&th, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);

    r = hotkv_scache_set(ctx, "n", 1, "q", 1, "r", 1, vec, 2, 0, &usage);
    hotkv_free_reply(r);
    pthread_join(th, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);

    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 10);
    static const char *want[] = {"SCACHE.SET", "n",     "q",  "r", "0.5",
                                 "0.25",       "MODEL", "gpt-4", "CACHEWRITE5M", "7"};
    for (size_t i = 0; i < 10; i++) {
        ASSERT_STR_EQ(f.captured[i], want[i]);
    }
    PASS();
}

TEST(wire_pcache_set_ttl_meta_and_usage) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    hotkv_token_usage_t usage;
    hotkv_reply_t *r;

    memset(&usage, 0, sizeof(usage));
    usage.model = "gpt-4";
    usage.input_tokens = 12;
    usage.output_tokens = 8;
    usage.cache_read_tokens = 4;

    ASSERT(hkv_fake_start(&f, "+OK\r\n", 5));
    pthread_t th;
    ASSERT(pthread_create(&th, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);

    r = hotkv_pcache_set(ctx, "n", 1, "p", 1, "r", 1, 30, "{\"k\":\"v\"}", 9, &usage);
    hotkv_free_reply(r);
    pthread_join(th, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);

    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 16);
    static const char *want[] = {"PCACHE.SET", "n",  "p",         "r",          "TTL",
                                 "30",         "META", "{\"k\":\"v\"}", "MODEL",     "gpt-4",
                                 "INPUT",      "12", "OUTPUT",    "8",          "CACHEREAD",
                                 "4"};
    for (size_t i = 0; i < 16; i++) {
        ASSERT_STR_EQ(f.captured[i], want[i]);
    }
    PASS();
}

TEST(wire_keyedcache_set_with_usage) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    hotkv_token_usage_t usage;
    hotkv_reply_t *r;

    memset(&usage, 0, sizeof(usage));
    usage.model = "gpt-4";
    usage.cache_write_1h_tokens = 9;

    ASSERT(hkv_fake_start(&f, "+OK\r\n", 5));
    pthread_t th;
    ASSERT(pthread_create(&th, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);

    r = hotkv_toolcache_set(ctx, "n", 1, "k", 1, "v", 1, 60, "meta", 4, &usage);
    hotkv_free_reply(r);
    pthread_join(th, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);

    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 12);
    static const char *want[] = {"TOOL.CACHE.SET", "n",    "k",     "v",
                                 "TTL",            "60",   "META",  "meta",
                                 "MODEL",          "gpt-4", "CACHEWRITE1H", "9"};
    for (size_t i = 0; i < 12; i++) {
        ASSERT_STR_EQ(f.captured[i], want[i]);
    }
    PASS();
}

TEST(wire_llmbudget_consume_legacy) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    hotkv_reply_t *r;

    ASSERT(hkv_fake_start(&f, ":1\r\n", 4));
    pthread_t th;
    ASSERT(pthread_create(&th, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);

    r = hotkv_llmbudget_consume(ctx, "name", 4, 10, 0.01, NULL);
    hotkv_free_reply(r);
    pthread_join(th, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);

    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 4);
    static const char *want[] = {"LLMBUDGET.CONSUME", "name", "10", "0.01"};
    for (size_t i = 0; i < 4; i++) {
        ASSERT_STR_EQ(f.captured[i], want[i]);
    }
    PASS();
}

TEST(wire_llmbudget_consume_with_usage) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    hotkv_token_usage_t usage;
    hotkv_reply_t *r;

    memset(&usage, 0, sizeof(usage));
    usage.model = "gpt-4";
    usage.input_tokens = 10;
    usage.output_tokens = 5;

    ASSERT(hkv_fake_start(&f, ":1\r\n", 4));
    pthread_t th;
    ASSERT(pthread_create(&th, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);

    r = hotkv_llmbudget_consume(ctx, "name", 4, 0, 0.0, &usage);
    hotkv_free_reply(r);
    pthread_join(th, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);

    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 10);
    static const char *want[] = {"LLMBUDGET.CONSUME", "name",  "0",      "0",     "MODEL",
                                 "gpt-4",              "INPUT", "10",     "OUTPUT", "5"};
    for (size_t i = 0; i < 10; i++) {
        ASSERT_STR_EQ(f.captured[i], want[i]);
    }
    PASS();
}

TEST(wire_llmstats_record_no_options) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    hotkv_reply_t *r;

    ASSERT(hkv_fake_start(&f, "+OK\r\n", 5));
    pthread_t th;
    ASSERT(pthread_create(&th, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);

    r = hotkv_llmstats_record(ctx, "bucket", 6, 100, 50, 0, 0.002, 120, NULL, NULL, 0, 0);
    hotkv_free_reply(r);
    pthread_join(th, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);

    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 7);
    static const char *want[] = {"LLMSTATS.RECORD", "bucket", "100", "50", "0", "0.002", "120"};
    for (size_t i = 0; i < 7; i++) {
        ASSERT_STR_EQ(f.captured[i], want[i]);
    }
    PASS();
}

TEST(wire_llmstats_record_model_and_cachewrites) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    hotkv_reply_t *r;
    int error_flag = 0;

    ASSERT(hkv_fake_start(&f, "+OK\r\n", 5));
    pthread_t th;
    ASSERT(pthread_create(&th, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);

    r = hotkv_llmstats_record(ctx, "bucket", 6, 100, 50, 10, 0, 80, &error_flag,
                               "claude-sonnet-5", 20, 30);
    hotkv_free_reply(r);
    pthread_join(th, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);

    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 15);
    static const char *want[] = {"LLMSTATS.RECORD", "bucket",           "100",
                                 "50",              "10",               "0",
                                 "80",              "ERROR",            "0",
                                 "MODEL",           "claude-sonnet-5", "CACHEWRITE5M",
                                 "20",              "CACHEWRITE1H",     "30"};
    for (size_t i = 0; i < 15; i++) {
        ASSERT_STR_EQ(f.captured[i], want[i]);
    }
    PASS();
}

TEST(wire_llmstats_price_set_required_only) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    hotkv_reply_t *r;

    ASSERT(hkv_fake_start(&f, "+OK\r\n", 5));
    pthread_t th;
    ASSERT(pthread_create(&th, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);

    r = hotkv_llmstats_price_set(ctx, "my-model", 8, 3.0, 15.0, NULL, NULL, NULL);
    hotkv_free_reply(r);
    pthread_join(th, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);

    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 7);
    static const char *want[] = {"LLMSTATS.PRICE", "SET", "my-model", "INPUT", "3", "OUTPUT", "15"};
    for (size_t i = 0; i < 7; i++) {
        ASSERT_STR_EQ(f.captured[i], want[i]);
    }
    PASS();
}

TEST(wire_llmstats_price_set_all_overrides) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    hotkv_reply_t *r;
    double cache_read = 0.5, cache_write_5m = 4.0, cache_write_1h = 6.0;

    ASSERT(hkv_fake_start(&f, "+OK\r\n", 5));
    pthread_t th;
    ASSERT(pthread_create(&th, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);

    r = hotkv_llmstats_price_set(ctx, "my-model", 8, 3.0, 15.0, &cache_read, &cache_write_5m,
                                  &cache_write_1h);
    hotkv_free_reply(r);
    pthread_join(th, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);

    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 13);
    static const char *want[] = {"LLMSTATS.PRICE", "SET",          "my-model",
                                 "INPUT",          "3",            "OUTPUT",
                                 "15",             "CACHEREAD",    "0.5",
                                 "CACHEWRITE5M",   "4",            "CACHEWRITE1H",
                                 "6"};
    for (size_t i = 0; i < 13; i++) {
        ASSERT_STR_EQ(f.captured[i], want[i]);
    }
    PASS();
}

TEST(wire_llmstats_price_get_list_del_args) {
    hkv_fake_t f;
    hotkv_context_t *ctx;
    hotkv_reply_t *r;

    /* GET */
    ASSERT(hkv_fake_start(&f, "*0\r\n", 4));
    pthread_t th1;
    ASSERT(pthread_create(&th1, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);
    r = hotkv_llmstats_price_get(ctx, "gpt-4", 5);
    hotkv_free_reply(r);
    pthread_join(th1, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);
    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 3);
    ASSERT_STR_EQ(f.captured[0], "LLMSTATS.PRICE");
    ASSERT_STR_EQ(f.captured[1], "GET");
    ASSERT_STR_EQ(f.captured[2], "gpt-4");

    /* LIST */
    ASSERT(hkv_fake_start(&f, "*0\r\n", 4));
    pthread_t th2;
    ASSERT(pthread_create(&th2, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);
    r = hotkv_llmstats_price_list(ctx);
    hotkv_free_reply(r);
    pthread_join(th2, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);
    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 2);
    ASSERT_STR_EQ(f.captured[0], "LLMSTATS.PRICE");
    ASSERT_STR_EQ(f.captured[1], "LIST");

    /* DEL */
    ASSERT(hkv_fake_start(&f, ":1\r\n", 4));
    pthread_t th3;
    ASSERT(pthread_create(&th3, NULL, hkv_fake_thread, &f) == 0);
    ctx = hotkv_connect("127.0.0.1", f.port);
    ASSERT(ctx && hotkv_context_err(ctx) == HOTKV_OK);
    r = hotkv_llmstats_price_del(ctx, "gpt-4", 5);
    hotkv_free_reply(r);
    pthread_join(th3, NULL);
    hotkv_disconnect(ctx);
    hkv_fake_stop(&f);
    ASSERT(f.ok);
    ASSERT_EQ_INT(f.captured_argc, 3);
    ASSERT_STR_EQ(f.captured[0], "LLMSTATS.PRICE");
    ASSERT_STR_EQ(f.captured[1], "DEL");
    ASSERT_STR_EQ(f.captured[2], "gpt-4");
    PASS();
}

/* =========================================================================
 * By-field-name parsing of hand-built pair-shaped
 * replies (RESP2 flat array and RESP3 map), never positional.
 * ========================================================================= */

static void fill_price_fields(hkv_pairs_t *b, const char *model, const char *source, int priced,
                               int retired) {
    hkv_pairs_add_str(b, "model", model);
    hkv_pairs_add_str(b, "source", source);
    hkv_pairs_add_int(b, "priced", priced);
    hkv_pairs_add_int(b, "retired", retired);
    hkv_pairs_add_str(b, "input_usd_per_mtok", "3.000000");
    hkv_pairs_add_str(b, "output_usd_per_mtok", "15.000000");
    hkv_pairs_add_str(b, "cache_read_usd_per_mtok", "0.300000");
    hkv_pairs_add_str(b, "cache_write_5m_usd_per_mtok", "3.750000");
    hkv_pairs_add_str(b, "cache_write_1h_usd_per_mtok", "6.000000");
}

TEST(llm_price_parses_resp3_map) {
    hkv_pairs_t b;
    hotkv_llm_price_t price;
    hkv_pairs_init(&b, 1);
    fill_price_fields(&b, "claude-sonnet-5", "custom", 1, 0);
    ASSERT_EQ_INT(hotkv_parse_llm_price(&b.reply, &price), HOTKV_OK);
    ASSERT_STR_EQ(price.model, "claude-sonnet-5");
    ASSERT_STR_EQ(price.source, "custom");
    ASSERT(price.priced);
    ASSERT(!price.retired);
    ASSERT_DBL_EQ(price.input_usd_per_mtok, 3.0);
    ASSERT_DBL_EQ(price.output_usd_per_mtok, 15.0);
    ASSERT_DBL_EQ(price.cache_read_usd_per_mtok, 0.3);
    ASSERT_DBL_EQ(price.cache_write_5m_usd_per_mtok, 3.75);
    ASSERT_DBL_EQ(price.cache_write_1h_usd_per_mtok, 6.0);
    PASS();
}

TEST(llm_price_unknown_model_is_not_an_error) {
    hkv_pairs_t b;
    hotkv_llm_price_t price;
    hkv_pairs_init(&b, 1);
    fill_price_fields(&b, "unknown-model", "unpriced", 0, 0);
    ASSERT_EQ_INT(hotkv_parse_llm_price(&b.reply, &price), HOTKV_OK);
    ASSERT_STR_EQ(price.source, "unpriced");
    ASSERT(!price.priced);
    PASS();
}

TEST(scache_stats_parses_resp3_map_with_usage_saved) {
    hkv_pairs_t b;
    hotkv_scache_stats_t stats;
    hkv_pairs_init(&b, 1);
    hkv_pairs_add_int(&b, "entries", 3);
    hkv_pairs_add_int(&b, "hits", 10);
    hkv_pairs_add_int(&b, "misses", 2);
    hkv_pairs_add_str(&b, "hit_rate", "0.833333");
    hkv_pairs_add_int(&b, "p50_us", 150);
    hkv_pairs_add_int(&b, "usage_saved_requests", 10);
    hkv_pairs_add_int(&b, "usage_saved_input_tokens", 120);
    hkv_pairs_add_int(&b, "usage_saved_output_tokens", 80);
    hkv_pairs_add_int(&b, "usage_saved_cache_read_tokens", 0);
    hkv_pairs_add_int(&b, "usage_saved_cache_write_tokens", 0);
    hkv_pairs_add_str(&b, "usage_saved_cost_usd", "0.000160");

    ASSERT_EQ_INT(hotkv_parse_scache_stats(&b.reply, &stats), HOTKV_OK);
    ASSERT_EQ_INT(stats.entries, 3);
    ASSERT_EQ_INT(stats.hits, 10);
    ASSERT_EQ_INT(stats.misses, 2);
    ASSERT_DBL_EQ(stats.hit_rate, 0.833333);
    ASSERT_EQ_INT(stats.p50_us, 150);
    ASSERT_EQ_INT(stats.usage.requests, 10);
    ASSERT_EQ_INT(stats.usage.input_tokens, 120);
    ASSERT_DBL_EQ(stats.usage.cost_usd, 0.00016);
    PASS();
}

TEST(pcache_stats_parses_resp2_flat_array_missing_fields_default_zero) {
    hkv_pairs_t b;
    hotkv_pcache_stats_t stats;
    hkv_pairs_init(&b, 0);
    hkv_pairs_add_int(&b, "entries", 1);
    hkv_pairs_add_int(&b, "hits", 4);

    ASSERT_EQ_INT(hotkv_parse_pcache_stats(&b.reply, &stats), HOTKV_OK);
    ASSERT_EQ_INT(stats.entries, 1);
    ASSERT_EQ_INT(stats.hits, 4);
    ASSERT_EQ_INT(stats.misses, 0);
    ASSERT_DBL_EQ(stats.cost_saved_usd, 0.0);
    ASSERT_EQ_INT(stats.usage.requests, 0);
    ASSERT_DBL_EQ(stats.usage.cost_usd, 0.0);
    PASS();
}

TEST(keyedcache_stats_parses_resp3_map) {
    hkv_pairs_t b;
    hotkv_keyedcache_stats_t stats;
    hkv_pairs_init(&b, 1);
    hkv_pairs_add_int(&b, "entries", 7);
    hkv_pairs_add_int(&b, "hits", 5);
    hkv_pairs_add_int(&b, "misses", 1);
    hkv_pairs_add_int(&b, "evictions", 0);
    hkv_pairs_add_int(&b, "memory_bytes", 2048);
    hkv_pairs_add_int(&b, "usage_saved_requests", 5);
    hkv_pairs_add_int(&b, "usage_saved_input_tokens", 50);
    hkv_pairs_add_int(&b, "usage_saved_output_tokens", 10);
    hkv_pairs_add_int(&b, "usage_saved_cache_read_tokens", 0);
    hkv_pairs_add_int(&b, "usage_saved_cache_write_tokens", 0);
    hkv_pairs_add_str(&b, "usage_saved_cost_usd", "0.000050");

    ASSERT_EQ_INT(hotkv_parse_keyedcache_stats(&b.reply, &stats), HOTKV_OK);
    ASSERT_EQ_INT(stats.entries, 7);
    ASSERT_EQ_INT(stats.memory_bytes, 2048);
    ASSERT_DBL_EQ(stats.usage.cost_usd, 0.00005);
    PASS();
}

TEST(llmstats_get_parses_resp2_flat_array) {
    hkv_pairs_t b;
    hotkv_llmstats_get_t stats;
    hkv_pairs_init(&b, 0);
    hkv_pairs_add_int(&b, "requests", 10);
    hkv_pairs_add_int(&b, "errors", 1);
    hkv_pairs_add_int(&b, "input_tokens", 500);
    hkv_pairs_add_int(&b, "output_tokens", 200);
    hkv_pairs_add_int(&b, "cached_tokens", 50);
    hkv_pairs_add_str(&b, "cost_usd", "0.0123");
    hkv_pairs_add_str(&b, "avg_latency_ms", "42.5");
    hkv_pairs_add_int(&b, "requests_1m", 3);
    hkv_pairs_add_int(&b, "tokens_1m", 150);
    hkv_pairs_add_str(&b, "cost_usd_1m", "0.004");
    hkv_pairs_add_int(&b, "cache_write_5m_tokens", 20);
    hkv_pairs_add_int(&b, "cache_write_1h_tokens", 0);
    hkv_pairs_add_int(&b, "unpriced_requests", 2);

    ASSERT_EQ_INT(hotkv_parse_llmstats_get(&b.reply, &stats), HOTKV_OK);
    ASSERT_EQ_INT(stats.requests, 10);
    ASSERT_EQ_INT(stats.errors, 1);
    ASSERT_DBL_EQ(stats.cost_usd, 0.0123);
    ASSERT_DBL_EQ(stats.avg_latency_ms, 42.5);
    ASSERT_EQ_INT(stats.cache_write_5m_tokens, 20);
    ASSERT_EQ_INT(stats.unpriced_requests, 2);
    PASS();
}

TEST(llmbudget_status_parses_resp3_map) {
    hkv_pairs_t b;
    hotkv_llmbudget_status_t status;
    hkv_pairs_init(&b, 1);
    hkv_pairs_add_int(&b, "tokens_used", 100);
    hkv_pairs_add_int(&b, "token_limit", 1000);
    hkv_pairs_add_str(&b, "cost_used_usd", "0.01");
    hkv_pairs_add_str(&b, "cost_limit_usd", "5.00");
    hkv_pairs_add_int(&b, "window_seconds", 3600);
    hkv_pairs_add_int(&b, "window_remaining", 1200);
    hkv_pairs_add_int(&b, "requests_allowed", 9);
    hkv_pairs_add_int(&b, "requests_rejected", 1);
    hkv_pairs_add_int(&b, "unpriced_requests", 0);

    ASSERT_EQ_INT(hotkv_parse_llmbudget_status(&b.reply, &status), HOTKV_OK);
    ASSERT_EQ_INT(status.tokens_used, 100);
    ASSERT_EQ_INT(status.token_limit, 1000);
    ASSERT_DBL_EQ(status.cost_used_usd, 0.01);
    ASSERT_EQ_INT(status.requests_allowed, 9);
    ASSERT_EQ_INT(status.unpriced_requests, 0);
    PASS();
}

int main(void) {
    printf("=== Enterprise reply-helper tests ===\n\n");

    run_test_nolicense_detected();
    run_test_nolicense_prefix_must_be_whole_word();
    run_test_non_error_reply_never_matches();
    run_test_moved_ask_wrongtype_noauth_readonly();
    run_test_error_prefix_extraction();
    run_test_error_prefix_truncates_to_fit();
    run_test_error_prefix_single_word_no_space();
    run_test_null_safety();

    printf("\n-- Token usage group: wire argument lists (loopback fake server) --\n");
    run_test_wire_scache_set_no_usage();
    run_test_wire_scache_set_with_usage();
    run_test_wire_pcache_set_ttl_meta_and_usage();
    run_test_wire_keyedcache_set_with_usage();
    run_test_wire_llmbudget_consume_legacy();
    run_test_wire_llmbudget_consume_with_usage();
    run_test_wire_llmstats_record_no_options();
    run_test_wire_llmstats_record_model_and_cachewrites();
    run_test_wire_llmstats_price_set_required_only();
    run_test_wire_llmstats_price_set_all_overrides();
    run_test_wire_llmstats_price_get_list_del_args();

    printf("\n-- By-field-name reply parsing --\n");
    run_test_llm_price_parses_resp3_map();
    run_test_llm_price_unknown_model_is_not_an_error();
    run_test_scache_stats_parses_resp3_map_with_usage_saved();
    run_test_pcache_stats_parses_resp2_flat_array_missing_fields_default_zero();
    run_test_keyedcache_stats_parses_resp3_map();
    run_test_llmstats_get_parses_resp2_flat_array();
    run_test_llmbudget_status_parses_resp3_map();

    printf("\n=================================\n");
    printf("Results: %d passed, %d failed, %d total\n", g_tests_passed, g_tests_failed,
           g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
