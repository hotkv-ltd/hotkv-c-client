/**
 * @file test_integration.c
 * @brief Integration tests against a live HotKV server. Enabled when the
 * HOTKV_TEST_URL environment variable is set (e.g. "hotkv://127.0.0.1:6379");
 * otherwise this exits 77 (CTest SKIP) without opening a connection.
 *
 * Covers the common command families (sanity), pipelining, transactions,
 * pub/sub, and every HotKV exclusive / enterprise command family. Enterprise
 * commands that reply NOLICENSE on an unlicensed server are counted as
 * SKIPPED, not FAILED, so this suite starts passing those checks
 * automatically once a license is loaded.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "hotkv.h"
#include "hotkv_private.h"

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

static int g_checks = 0;
static int g_ok = 0;
static int g_failed = 0;
static int g_skipped = 0;
static char g_run_id[32];

/* Counts and frees. Fails on NULL or any error reply that is not NOLICENSE. */
static void check(hotkv_reply_t *r, const char *desc) {
    g_checks++;
    if (!r) {
        g_failed++;
        printf("  FAIL %s (no reply / connection error)\n", desc);
        return;
    }
    if (r->type == HOTKV_REPLY_ERROR) {
        if (hotkv_reply_is_nolicense(r)) {
            g_skipped++;
            printf("  SKIP %s (%s)\n", desc, r->str);
        } else {
            g_failed++;
            printf("  FAIL %s -> %s\n", desc, r->str);
        }
        hotkv_free_reply(r);
        return;
    }
    g_ok++;
    hotkv_free_reply(r);
}

/* Like check(), but hands the still-open reply to a caller-supplied
 * validator for deeper shape assertions. The validator receives 1 if the
 * reply looks right. Frees the reply either way. */
typedef int (*validator_fn)(hotkv_reply_t *r);

static void check_shape(hotkv_reply_t *r, const char *desc, validator_fn fn) {
    g_checks++;
    if (!r) {
        g_failed++;
        printf("  FAIL %s (no reply / connection error)\n", desc);
        return;
    }
    if (r->type == HOTKV_REPLY_ERROR) {
        if (hotkv_reply_is_nolicense(r)) {
            g_skipped++;
            printf("  SKIP %s (%s)\n", desc, r->str);
        } else {
            g_failed++;
            printf("  FAIL %s -> %s\n", desc, r->str);
        }
        hotkv_free_reply(r);
        return;
    }
    if (fn(r)) {
        g_ok++;
    } else {
        g_failed++;
        printf("  FAIL %s (unexpected shape, type=%d)\n", desc, r->type);
    }
    hotkv_free_reply(r);
}

/* Returns a heap-allocated "sdktest:<run>:<suffix>" key. Every key is
 * remembered and released by free_keys() at the end of main, so several can
 * be alive at once (a single shared static buffer would alias them) and the
 * suite stays clean under ASan and valgrind. */
static char **g_keys = NULL;
static size_t g_nkeys = 0;
static size_t g_keycap = 0;

static char *key(const char *suffix) {
    char buf[256];
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

static void free_keys(void) {
    size_t i;
    for (i = 0; i < g_nkeys; i++) hotkv_free(g_keys[i]);
    free(g_keys);
    g_keys = NULL;
    g_nkeys = g_keycap = 0;
}

/* =========================================================================
 * Sanity: strings, keys, lists, sets, hashes, sorted sets, server
 * ========================================================================= */

static void test_section3(hotkv_context_t *ctx) {
    printf("\n-- Core command sanity --\n");
    check(hotkv_ping(ctx), "PING");
    check(hotkv_set(ctx, key("str"), strlen(key("str")), "v1", 2), "SET");
    check(hotkv_get(ctx, key("str"), strlen(key("str"))), "GET");
    check(hotkv_incr(ctx, key("ctr"), strlen(key("ctr"))), "INCR");
    check(hotkv_expire(ctx, key("str"), strlen(key("str")), 3600), "EXPIRE");
    check(hotkv_ttl(ctx, key("str"), strlen(key("str"))), "TTL");
    {
        const char *members[] = {"a", "b", "c"};
        size_t lens[] = {1, 1, 1};
        check(hotkv_sadd(ctx, key("set"), strlen(key("set")), members, lens, 3), "SADD");
    }
    check(hotkv_smembers(ctx, key("set"), strlen(key("set"))), "SMEMBERS");
    {
        const char *fields[] = {"f1"};
        size_t flens[] = {2};
        const char *vals[] = {"v1"};
        size_t vlens[] = {2};
        check(hotkv_hset(ctx, key("hash"), strlen(key("hash")), fields, flens, vals, vlens, 1),
              "HSET");
    }
    check(hotkv_hgetall(ctx, key("hash"), strlen(key("hash"))), "HGETALL");
    {
        double scores[] = {1.5};
        const char *members[] = {"m1"};
        size_t mlens[] = {2};
        check(hotkv_zadd(ctx, key("zset"), strlen(key("zset")), scores, members, mlens, 1),
              "ZADD");
    }
    check(hotkv_zrange(ctx, key("zset"), strlen(key("zset")), 0, -1, 0), "ZRANGE");
    {
        const char *vals[] = {"x", "y"};
        size_t vlens[] = {1, 1};
        check(hotkv_lpush(ctx, key("list"), strlen(key("list")), vals, vlens, 2), "LPUSH");
    }
    check(hotkv_lrange(ctx, key("list"), strlen(key("list")), 0, -1), "LRANGE");
    check(hotkv_dbsize(ctx), "DBSIZE");
    check(hotkv_info(ctx, NULL), "INFO");
}

/* =========================================================================
 * Pipelining, transactions
 * ========================================================================= */

static void test_pipeline_and_tx(hotkv_context_t *ctx) {
    printf("\n-- Pipelining and transactions --\n");

    g_checks++;
    int rc = hotkv_append_command(ctx, "SET %s %s", key("pl1"), "a");
    rc |= hotkv_append_command(ctx, "SET %s %s", key("pl2"), "b");
    rc |= hotkv_append_command(ctx, "GET %s", key("pl1"));
    if (rc == HOTKV_OK) {
        g_ok++;
    } else {
        g_failed++;
        printf("  FAIL pipeline append\n");
    }

    for (int i = 0; i < 3; i++) {
        hotkv_reply_t *r = NULL;
        g_checks++;
        if (hotkv_get_reply(ctx, &r) == HOTKV_OK && r) {
            g_ok++;
        } else {
            g_failed++;
            printf("  FAIL pipeline reply %d\n", i);
        }
        hotkv_free_reply(r);
    }

    check(hotkv_multi(ctx), "MULTI");
    g_checks++;
    if (hotkv_append_command(ctx, "SET %s %s", key("tx1"), "v") == HOTKV_OK) {
        g_ok++;
    } else {
        g_failed++;
    }
    {
        hotkv_reply_t *queued = NULL;
        hotkv_get_reply(ctx, &queued); /* +QUEUED */
        hotkv_free_reply(queued);
    }
    check(hotkv_exec(ctx), "EXEC");
}

/* =========================================================================
 * Pub/Sub (via the generic command call)
 * ========================================================================= */

static void test_pubsub(const hotkv_url_t *u) {
    printf("\n-- Pub/Sub --\n");

    hotkv_options_t sub_opts;
    memset(&sub_opts, 0, sizeof(sub_opts));
    sub_opts.host = u->host;
    sub_opts.port = u->port;
    sub_opts.command_timeout.tv_sec = 3;

    hotkv_context_t *sub = hotkv_connect_with_options(&sub_opts);
    g_checks++;
    if (!sub || hotkv_context_err(sub) != HOTKV_OK) {
        g_failed++;
        printf("  FAIL pubsub subscriber connect\n");
        hotkv_disconnect(sub);
        return;
    }
    g_ok++;

    char *chan = key("chan");
    hotkv_reply_t *confirm = hotkv_command(sub, "SUBSCRIBE %s", chan);
    check(confirm, "SUBSCRIBE");

    hotkv_options_t pub_opts;
    memset(&pub_opts, 0, sizeof(pub_opts));
    pub_opts.host = u->host;
    pub_opts.port = u->port;
    hotkv_context_t *pub = hotkv_connect_with_options(&pub_opts);
    check(hotkv_publish(pub, chan, strlen(chan), "hello", 5), "PUBLISH");
    hotkv_disconnect(pub);

    hotkv_reply_t *msg = NULL;
    int rc = hotkv_get_reply(sub, &msg);
    g_checks++;
    if (rc == HOTKV_OK && msg && msg->type == HOTKV_REPLY_ARRAY) {
        g_ok++;
    } else {
        g_failed++;
        printf("  FAIL pubsub message delivery\n");
    }
    hotkv_free_reply(msg);
    hotkv_disconnect(sub);
}

/* =========================================================================
 * TAG.*, NEXPIRE/NTTL/NEXPIREAT, HOTKEYS
 * ========================================================================= */

static void test_tag_and_nexpire_and_hotkeys(hotkv_context_t *ctx) {
    printf("\n-- TAG.*, NEXPIRE/NTTL/NEXPIREAT, HOTKEYS --\n");
    check(hotkv_set(ctx, key("tagged"), strlen(key("tagged")), "v", 1), "SET for TAG");
    {
        const char *tags[] = {"alpha", "beta"};
        size_t lens[] = {5, 4};
        check(hotkv_tag_set(ctx, key("tagged"), strlen(key("tagged")), tags, lens, 2), "TAG.SET");
    }
    check(hotkv_tag_get(ctx, key("tagged"), strlen(key("tagged"))), "TAG.GET");
    check(hotkv_tag_members(ctx, "alpha", 5), "TAG.MEMBERS");
    check(hotkv_tag_count(ctx, key("tagged"), strlen(key("tagged"))), "TAG.COUNT");
    check(hotkv_tag_exists(ctx, key("tagged"), strlen(key("tagged")), "alpha", 5), "TAG.EXISTS");
    {
        const char *tags[] = {"alpha"};
        size_t lens[] = {5};
        check(hotkv_tag_rem(ctx, key("tagged"), strlen(key("tagged")), tags, lens, 1), "TAG.REM");
    }

    check(hotkv_nexpire(ctx, key("tagged"), strlen(key("tagged")), 5000000000ULL), "NEXPIRE");
    check(hotkv_nttl(ctx, key("tagged"), strlen(key("tagged"))), "NTTL");
    check(hotkv_nexpireat(ctx, key("tagged"), strlen(key("tagged")),
                          (uint64_t)time(NULL) * 1000000000ULL + 5000000000ULL),
          "NEXPIREAT");

    check(hotkv_hotkeys_top(ctx, 5), "HOTKEYS TOP");
    check(hotkv_hotkeys_reset(ctx), "HOTKEYS RESET");
}

/* =========================================================================
 * GCRA.*
 * ========================================================================= */

static int gcra_shape(hotkv_reply_t *r) {
    return r->type == HOTKV_REPLY_ARRAY && r->nelem == 5;
}

static void test_gcra(hotkv_context_t *ctx) {
    printf("\n-- GCRA.* --\n");
    check_shape(hotkv_gcra_limit(ctx, key("gcra"), strlen(key("gcra")), 10000, 5, 0), "GCRA.LIMIT",
                gcra_shape);
    {
        const char *keys[] = {key("gcra")};
        size_t lens[] = {strlen(key("gcra"))};
        check(hotkv_gcra_reset(ctx, keys, lens, 1), "GCRA.RESET");
    }
}

/* =========================================================================
 * VSIM.*
 * ========================================================================= */

static void test_vsim(hotkv_context_t *ctx) {
    printf("\n-- VSIM.* --\n");
    char *idx = key("vsimidx");
    float vec[3] = {0.1f, 0.2f, 0.3f};

    check(hotkv_vsim_createindex(ctx, idx, strlen(idx), 3, "cosine", 0, 0, 0), "VSIM.CREATEINDEX");
    check(hotkv_vsim_set(ctx, idx, strlen(idx), "v1", 2, vec, 3), "VSIM.SET");
    check(hotkv_vsim_get(ctx, idx, strlen(idx), "v1", 2), "VSIM.GET");
    check(hotkv_vsim_search(ctx, idx, strlen(idx), 1, vec, 3, NULL, NULL, 0, 0), "VSIM.SEARCH");
    check(hotkv_vsim_info(ctx, idx, strlen(idx)), "VSIM.INFO <idx>");
    check(hotkv_vsim_info(ctx, NULL, 0), "VSIM.INFO (list)");
    check(hotkv_vsim_del(ctx, idx, strlen(idx), "v1", 2), "VSIM.DEL");
    check(hotkv_vsim_dropindex(ctx, idx, strlen(idx)), "VSIM.DROPINDEX");

    /* Snapshot names may not contain '.', ':', '/' or a backslash. */
    char snap[64];
    snprintf(snap, sizeof(snap), "sdktest_snap_%d_%d", (int)time(NULL), (int)hkv_getpid());
    printf("  (VSIM persistence needs a license with the VSIM persistence feature)\n");
    check(hotkv_vsim_save(ctx, snap, strlen(snap), "blob", 4), "VSIM.SAVE");
    check(hotkv_vsim_load(ctx, snap, strlen(snap)), "VSIM.LOAD");
    check(hotkv_vsim_list(ctx), "VSIM.LIST");
    check(hotkv_vsim_drop(ctx, snap, strlen(snap)), "VSIM.DROP");
}

/* =========================================================================
 * SCACHE.*
 * ========================================================================= */

static void test_scache(hotkv_context_t *ctx) {
    printf("\n-- SCACHE.* --\n");
    char *ns = key("scache");
    float vec[3] = {0.1f, 0.2f, 0.3f};
    double threshold = 0.5;

    check(hotkv_scache_create(ctx, ns, strlen(ns), 3, "cosine", &threshold, 0), "SCACHE.CREATE");
    {
        hotkv_token_usage_t usage;
        memset(&usage, 0, sizeof(usage));
        usage.model = "gpt-4";
        usage.input_tokens = 10;
        usage.output_tokens = 4;
        check(hotkv_scache_set(ctx, ns, strlen(ns), "q1", 2, "r1", 2, vec, 3, 0, &usage),
              "SCACHE.SET");
    }
    check(hotkv_scache_get(ctx, ns, strlen(ns), vec, 3, NULL), "SCACHE.GET");
    {
        hotkv_scache_stats_t stats;
        hotkv_reply_t *r = hotkv_scache_stats(ctx, ns, strlen(ns));
        g_checks++;
        if (r && r->type != HOTKV_REPLY_ERROR && hotkv_parse_scache_stats(r, &stats) == HOTKV_OK) {
            g_ok++;
        } else if (r && hotkv_reply_is_nolicense(r)) {
            g_skipped++;
            printf("  SKIP SCACHE.STATS (%s)\n", r->str);
        } else {
            g_failed++;
            printf("  FAIL SCACHE.STATS parse\n");
        }
        hotkv_free_reply(r);
    }
    check(hotkv_scache_threshold(ctx, ns, strlen(ns), 0.6), "SCACHE.THRESHOLD");
    check(hotkv_scache_list(ctx), "SCACHE.LIST");
    check(hotkv_scache_flush(ctx, ns, strlen(ns)), "SCACHE.FLUSH");
    check(hotkv_scache_drop(ctx, ns, strlen(ns)), "SCACHE.DROP");
}

/* =========================================================================
 * PCACHE.*
 * ========================================================================= */

static void test_pcache(hotkv_context_t *ctx) {
    printf("\n-- PCACHE.* (needs a license with the prompt-cache feature) --\n");
    char *ns = key("pcache");
    check(hotkv_pcache_create(ctx, ns, strlen(ns), 0, 0, 0.0), "PCACHE.CREATE");
    {
        hotkv_token_usage_t usage;
        memset(&usage, 0, sizeof(usage));
        usage.model = "gpt-4";
        usage.input_tokens = 12;
        usage.output_tokens = 8;
        check(hotkv_pcache_set(ctx, ns, strlen(ns), "prompt1", 7, "resp1", 5, 0, NULL, 0, &usage),
              "PCACHE.SET");
    }
    check(hotkv_pcache_get(ctx, ns, strlen(ns), "prompt1", 7), "PCACHE.GET");
    check(hotkv_pcache_getex(ctx, ns, strlen(ns), "prompt1", 7), "PCACHE.GETEX");
    {
        hotkv_pcache_stats_t stats;
        hotkv_reply_t *r = hotkv_pcache_stats(ctx, ns, strlen(ns));
        g_checks++;
        if (r && r->type != HOTKV_REPLY_ERROR && hotkv_parse_pcache_stats(r, &stats) == HOTKV_OK) {
            g_ok++;
        } else if (r && hotkv_reply_is_nolicense(r)) {
            g_skipped++;
            printf("  SKIP PCACHE.STATS (%s)\n", r->str);
        } else {
            g_failed++;
            printf("  FAIL PCACHE.STATS parse\n");
        }
        hotkv_free_reply(r);
    }
    check(hotkv_pcache_config_set(ctx, ns, strlen(ns), "TTL", "60", 2), "PCACHE.CONFIG");
    check(hotkv_pcache_list(ctx), "PCACHE.LIST");
    check(hotkv_pcache_del(ctx, ns, strlen(ns), "prompt1", 7), "PCACHE.DEL");
    check(hotkv_pcache_flush(ctx, ns, strlen(ns)), "PCACHE.FLUSH");
    check(hotkv_pcache_drop(ctx, ns, strlen(ns)), "PCACHE.DROP");
}

/* =========================================================================
 * Keyed caches
 * ========================================================================= */

static void test_keyed_cache_family(hotkv_context_t *ctx, const char *label, char *ns,
                                     hotkv_reply_t *(*create)(hotkv_context_t *, const char *,
                                                               size_t, uint64_t, uint32_t),
                                     hotkv_reply_t *(*set)(hotkv_context_t *, const char *, size_t,
                                                            const char *, size_t, const char *,
                                                            size_t, uint32_t, const char *, size_t,
                                                            const hotkv_token_usage_t *),
                                     hotkv_reply_t *(*get)(hotkv_context_t *, const char *, size_t,
                                                            const char *, size_t),
                                     hotkv_reply_t *(*del)(hotkv_context_t *, const char *, size_t,
                                                            const char *, size_t),
                                     hotkv_reply_t *(*stats)(hotkv_context_t *, const char *,
                                                              size_t),
                                     hotkv_reply_t *(*flush)(hotkv_context_t *, const char *,
                                                              size_t),
                                     hotkv_reply_t *(*drop)(hotkv_context_t *, const char *,
                                                             size_t),
                                     hotkv_reply_t *(*list)(hotkv_context_t *)) {
    char desc[128];
    hotkv_token_usage_t usage;
    memset(&usage, 0, sizeof(usage));
    usage.model = "gpt-4";
    usage.cache_write_1h_tokens = 9;

    printf("  %s\n", label);
    snprintf(desc, sizeof(desc), "%s.CREATE", label);
    check(create(ctx, ns, strlen(ns), 0, 0), desc);
    snprintf(desc, sizeof(desc), "%s.SET", label);
    check(set(ctx, ns, strlen(ns), "k1", 2, "v1", 2, 0, NULL, 0, &usage), desc);
    snprintf(desc, sizeof(desc), "%s.GET", label);
    check(get(ctx, ns, strlen(ns), "k1", 2), desc);
    {
        hotkv_keyedcache_stats_t kstats;
        hotkv_reply_t *r = stats(ctx, ns, strlen(ns));
        g_checks++;
        snprintf(desc, sizeof(desc), "%s.STATS parse", label);
        if (r && r->type != HOTKV_REPLY_ERROR &&
            hotkv_parse_keyedcache_stats(r, &kstats) == HOTKV_OK) {
            g_ok++;
        } else if (r && hotkv_reply_is_nolicense(r)) {
            g_skipped++;
            printf("  SKIP %s (%s)\n", desc, r->str);
        } else {
            g_failed++;
            printf("  FAIL %s\n", desc);
        }
        hotkv_free_reply(r);
    }
    snprintf(desc, sizeof(desc), "%s.LIST", label);
    check(list(ctx), desc);
    snprintf(desc, sizeof(desc), "%s.DEL", label);
    check(del(ctx, ns, strlen(ns), "k1", 2), desc);
    snprintf(desc, sizeof(desc), "%s.FLUSH", label);
    check(flush(ctx, ns, strlen(ns)), desc);
    snprintf(desc, sizeof(desc), "%s.DROP", label);
    check(drop(ctx, ns, strlen(ns)), desc);
}

static void test_keyed_caches(hotkv_context_t *ctx) {
    printf("\n-- Keyed caches: TOOL.CACHE / NCACHE / GUARD / RERANK --\n");
    test_keyed_cache_family(ctx, "TOOL.CACHE", key("toolc"), hotkv_toolcache_create,
                             hotkv_toolcache_set, hotkv_toolcache_get, hotkv_toolcache_del,
                             hotkv_toolcache_stats, hotkv_toolcache_flush, hotkv_toolcache_drop,
                             hotkv_toolcache_list);
    test_keyed_cache_family(ctx, "NCACHE", key("ncache"), hotkv_ncache_create, hotkv_ncache_set,
                             hotkv_ncache_get, hotkv_ncache_del, hotkv_ncache_stats,
                             hotkv_ncache_flush, hotkv_ncache_drop, hotkv_ncache_list);
    test_keyed_cache_family(ctx, "GUARD", key("guardc"), hotkv_guardcache_create,
                             hotkv_guardcache_set, hotkv_guardcache_get, hotkv_guardcache_del,
                             hotkv_guardcache_stats, hotkv_guardcache_flush, hotkv_guardcache_drop,
                             hotkv_guardcache_list);
    test_keyed_cache_family(ctx, "RERANK", key("rerankc"), hotkv_rerankcache_create,
                             hotkv_rerankcache_set, hotkv_rerankcache_get, hotkv_rerankcache_del,
                             hotkv_rerankcache_stats, hotkv_rerankcache_flush,
                             hotkv_rerankcache_drop, hotkv_rerankcache_list);
}

/* =========================================================================
 * LLMSTATS.*, LLMBUDGET.*, PROMPT.*, EMBED.DEDUP.*
 * ========================================================================= */

static void test_llmstats_budget_prompt_embed(hotkv_context_t *ctx) {
    printf("\n-- LLMSTATS.* --\n");
    char *bucket = key("llmb");
    check(hotkv_llmstats_bucket(ctx, bucket, strlen(bucket)), "LLMSTATS.BUCKET");
    check(hotkv_llmstats_record(ctx, bucket, strlen(bucket), 100, 50, 0, 0.002, 120, NULL, NULL, 0,
                                0),
          "LLMSTATS.RECORD");
    check(hotkv_llmstats_record(ctx, bucket, strlen(bucket), 100, 50, 10, 0, 80, NULL, "gpt-4", 20,
                                0),
          "LLMSTATS.RECORD (MODEL)");
    {
        hotkv_llmstats_get_t stats;
        hotkv_reply_t *r = hotkv_llmstats_get(ctx, bucket, strlen(bucket));
        g_checks++;
        if (r && r->type != HOTKV_REPLY_ERROR && hotkv_parse_llmstats_get(r, &stats) == HOTKV_OK) {
            g_ok++;
        } else if (r && hotkv_reply_is_nolicense(r)) {
            g_skipped++;
            printf("  SKIP LLMSTATS.GET parse (%s)\n", r->str);
        } else {
            g_failed++;
            printf("  FAIL LLMSTATS.GET parse\n");
        }
        hotkv_free_reply(r);
    }
    check(hotkv_llmstats_list(ctx), "LLMSTATS.LIST");
    check(hotkv_llmstats_reset(ctx, bucket, strlen(bucket)), "LLMSTATS.RESET");
    check(hotkv_llmstats_drop(ctx, bucket, strlen(bucket)), "LLMSTATS.DROP");

    printf("\n-- LLMSTATS.PRICE.* --\n");
    char *model = key("model");
    check(hotkv_llmstats_price_set(ctx, model, strlen(model), 3.0, 15.0, NULL, NULL, NULL),
          "LLMSTATS.PRICE SET");
    {
        hotkv_llm_price_t price;
        hotkv_reply_t *r = hotkv_llmstats_price_get(ctx, model, strlen(model));
        g_checks++;
        if (r && r->type != HOTKV_REPLY_ERROR && hotkv_parse_llm_price(r, &price) == HOTKV_OK) {
            g_ok++;
        } else if (r && hotkv_reply_is_nolicense(r)) {
            g_skipped++;
            printf("  SKIP LLMSTATS.PRICE GET parse (%s)\n", r->str);
        } else {
            g_failed++;
            printf("  FAIL LLMSTATS.PRICE GET parse\n");
        }
        hotkv_free_reply(r);
    }
    check(hotkv_llmstats_price_list(ctx), "LLMSTATS.PRICE LIST");
    check(hotkv_llmstats_price_del(ctx, model, strlen(model)), "LLMSTATS.PRICE DEL");

    printf("\n-- LLMBUDGET.* --\n");
    char *budget = key("budget");
    check(hotkv_llmbudget_set(ctx, budget, strlen(budget), 1000, 5.0, 3600), "LLMBUDGET.SET");
    check(hotkv_llmbudget_consume(ctx, budget, strlen(budget), 10, 0.01, NULL),
          "LLMBUDGET.CONSUME");
    {
        hotkv_token_usage_t usage;
        memset(&usage, 0, sizeof(usage));
        usage.model = "gpt-4";
        usage.input_tokens = 10;
        usage.output_tokens = 5;
        check(hotkv_llmbudget_consume(ctx, budget, strlen(budget), 0, 0.0, &usage),
              "LLMBUDGET.CONSUME (MODEL)");
    }
    {
        hotkv_llmbudget_status_t status;
        hotkv_reply_t *r = hotkv_llmbudget_status(ctx, budget, strlen(budget));
        g_checks++;
        if (r && r->type != HOTKV_REPLY_ERROR &&
            hotkv_parse_llmbudget_status(r, &status) == HOTKV_OK) {
            g_ok++;
        } else if (r && hotkv_reply_is_nolicense(r)) {
            g_skipped++;
            printf("  SKIP LLMBUDGET.STATUS parse (%s)\n", r->str);
        } else {
            g_failed++;
            printf("  FAIL LLMBUDGET.STATUS parse\n");
        }
        hotkv_free_reply(r);
    }
    check(hotkv_llmbudget_list(ctx), "LLMBUDGET.LIST");
    check(hotkv_llmbudget_reset(ctx, budget, strlen(budget)), "LLMBUDGET.RESET");
    check(hotkv_llmbudget_drop(ctx, budget, strlen(budget)), "LLMBUDGET.DROP");

    printf("\n-- PROMPT.* --\n");
    char *pname = key("prompt");
    check(hotkv_prompt_put(ctx, pname, strlen(pname), "hello {{name}}", 14, NULL, 0),
          "PROMPT.PUT");
    check(hotkv_prompt_get(ctx, pname, strlen(pname), 0), "PROMPT.GET");
    check(hotkv_prompt_latest(ctx, pname, strlen(pname)), "PROMPT.LATEST");
    check(hotkv_prompt_versions(ctx, pname, strlen(pname)), "PROMPT.VERSIONS");
    check(hotkv_prompt_list(ctx), "PROMPT.LIST");
    check(hotkv_prompt_drop(ctx, pname, strlen(pname)), "PROMPT.DROP");

    printf("\n-- EMBED.DEDUP.* --\n");
    char *ns = key("embed");
    check(hotkv_embed_dedup_put(ctx, ns, strlen(ns), "hash1", 5, "modelA", 6, "0.1,0.2", 7),
          "EMBED.DEDUP.PUT");
    check(hotkv_embed_dedup_get(ctx, ns, strlen(ns), "hash1", 5), "EMBED.DEDUP.GET");
    check(hotkv_embed_dedup_stats(ctx, ns, strlen(ns)), "EMBED.DEDUP.STATS");
    check(hotkv_embed_dedup_list(ctx), "EMBED.DEDUP.LIST");
    check(hotkv_embed_dedup_release(ctx, ns, strlen(ns), "hash1", 5), "EMBED.DEDUP.RELEASE");
    check(hotkv_embed_dedup_purge(ctx, ns, strlen(ns)), "EMBED.DEDUP.PURGE");
    check(hotkv_embed_dedup_drop(ctx, ns, strlen(ns)), "EMBED.DEDUP.DROP");
}

/* =========================================================================
 * AMEM.* and AMEM.SUMMARY.*
 * ========================================================================= */

static void test_amem(hotkv_context_t *ctx) {
    printf("\n-- AMEM.* --\n");
    char *sess = key("sess");
    char *agent = key("agent");
    float vec[3] = {0.1f, 0.2f, 0.3f};

    check(hotkv_amem_conv_create(ctx, sess, strlen(sess), 0, 0), "AMEM.CONV.CREATE");
    check(hotkv_amem_conv_append(ctx, sess, strlen(sess), "user", 4, "hi", 2), "AMEM.CONV.APPEND");
    check(hotkv_amem_conv_get(ctx, sess, strlen(sess)), "AMEM.CONV.GET");
    check(hotkv_amem_conv_window(ctx, sess, strlen(sess), 5), "AMEM.CONV.WINDOW");
    check(hotkv_amem_conv_clear(ctx, sess, strlen(sess)), "AMEM.CONV.CLEAR");
    check(hotkv_amem_conv_drop(ctx, sess, strlen(sess)), "AMEM.CONV.DROP");

    check(hotkv_amem_sem_create(ctx, agent, strlen(agent), 3), "AMEM.SEM.CREATE");
    check(hotkv_amem_sem_store(ctx, agent, strlen(agent), "k1", 2, "fact", 4, vec, 3, 0),
          "AMEM.SEM.STORE");
    check(hotkv_amem_sem_recall(ctx, agent, strlen(agent), 1, vec, 3), "AMEM.SEM.RECALL");
    check(hotkv_amem_sem_list(ctx, agent, strlen(agent)), "AMEM.SEM.LIST");
    check(hotkv_amem_sem_forget(ctx, agent, strlen(agent), "k1", 2), "AMEM.SEM.FORGET");

    check(hotkv_amem_work_set(ctx, sess, strlen(sess), "wk", 2, "wv", 2, 0), "AMEM.WORK.SET");
    check(hotkv_amem_work_get(ctx, sess, strlen(sess), "wk", 2), "AMEM.WORK.GET");
    check(hotkv_amem_work_list(ctx, sess, strlen(sess)), "AMEM.WORK.LIST");
    check(hotkv_amem_work_del(ctx, sess, strlen(sess), "wk", 2), "AMEM.WORK.DEL");
    check(hotkv_amem_work_clear(ctx, sess, strlen(sess)), "AMEM.WORK.CLEAR");

    check(hotkv_amem_epis_log(ctx, agent, strlen(agent), "note", 4, "content", 7, NULL, 0),
          "AMEM.EPIS.LOG");
    check(hotkv_amem_epis_query(ctx, agent, strlen(agent), NULL, 0, NULL, NULL, NULL),
          "AMEM.EPIS.QUERY");
    check(hotkv_amem_epis_stats(ctx, agent, strlen(agent)), "AMEM.EPIS.STATS");
    check(hotkv_amem_epis_clear(ctx, agent, strlen(agent)), "AMEM.EPIS.CLEAR");

    check(hotkv_amem_search(ctx, agent, strlen(agent), 1, vec, 3), "AMEM.SEARCH");

    printf("\n-- AMEM.SUMMARY.* (needs a license with the agent-memory summary feature) --\n");
    check(hotkv_amem_summary_set(ctx, agent, strlen(agent), "summary v1", 10),
          "AMEM.SUMMARY.SET");
    check(hotkv_amem_summary_append(ctx, agent, strlen(agent), " more", 5),
          "AMEM.SUMMARY.APPEND");
    check(hotkv_amem_summary_get(ctx, agent, strlen(agent)), "AMEM.SUMMARY.GET");
    check(hotkv_amem_summary_list(ctx), "AMEM.SUMMARY.LIST");
    check(hotkv_amem_summary_drop(ctx, agent, strlen(agent)), "AMEM.SUMMARY.DROP");
}

/* =========================================================================
 * RAG.*
 * ========================================================================= */

static void test_rag(hotkv_context_t *ctx) {
    printf("\n-- RAG.* --\n");
    char *pipeline = key("rag");
    float vecbuf[3] = {0.1f, 0.2f, 0.3f};
    const float *vectors[1];
    size_t vector_lens[1];
    vectors[0] = vecbuf;
    vector_lens[0] = 3;

    check(hotkv_rag_create(ctx, pipeline, strlen(pipeline), 3, NULL, 0, 0), "RAG.CREATE");
    check(hotkv_rag_ingest(ctx, pipeline, strlen(pipeline), "doc1", 4, "hello world", 11, NULL,
                           NULL, NULL, NULL, 0, vectors, vector_lens, 1),
          "RAG.INGEST");
    check(hotkv_rag_search(ctx, pipeline, strlen(pipeline), "hello", 5, 0, NULL, NULL, 0, NULL,
                           NULL, NULL, NULL, 0),
          "RAG.SEARCH");
    check(hotkv_rag_stats(ctx, pipeline, strlen(pipeline)), "RAG.STATS");
    check(hotkv_rag_list(ctx), "RAG.LIST");
    check(hotkv_rag_drop(ctx, pipeline, strlen(pipeline)), "RAG.DROP");
}

/* =========================================================================
 * RATELIMIT.*
 * ========================================================================= */

static int ratelimit_shape(hotkv_reply_t *r) {
    return r->type == HOTKV_REPLY_ARRAY && r->nelem == 5;
}

static void test_ratelimit(hotkv_context_t *ctx) {
    printf("\n-- RATELIMIT.* --\n");
    char *name = key("rl");
    check(hotkv_ratelimit_create(ctx, name, strlen(name), 10, 1000, "sliding_window", 0, 0),
          "RATELIMIT.CREATE");
    check_shape(hotkv_ratelimit_check(ctx, name, strlen(name), "clientA", 7, 0),
                "RATELIMIT.CHECK", ratelimit_shape);
    check_shape(hotkv_ratelimit_peek(ctx, name, strlen(name), "clientA", 7), "RATELIMIT.PEEK",
                ratelimit_shape);
    check(hotkv_ratelimit_stats(ctx, name, strlen(name)), "RATELIMIT.STATS");
    check(hotkv_ratelimit_list(ctx), "RATELIMIT.LIST");
    check(hotkv_ratelimit_reset(ctx, name, strlen(name), "clientA", 7), "RATELIMIT.RESET");
    check(hotkv_ratelimit_drop(ctx, name, strlen(name)), "RATELIMIT.DROP");
}

/* =========================================================================
 * TS.*
 * ========================================================================= */

static void test_ts(hotkv_context_t *ctx) {
    printf("\n-- TS.* --\n");
    char *series = key("ts");
    char *dest = key("ts_down");

    check(hotkv_ts_create(ctx, series, strlen(series), 0, NULL, NULL, NULL, NULL, 0),
          "TS.CREATE");
    check(hotkv_ts_add(ctx, series, strlen(series), NULL, 1.5), "TS.ADD");
    check(hotkv_ts_range(ctx, series, strlen(series), 0, 99999999999999LL, 0), "TS.RANGE");
    check(hotkv_ts_agg(ctx, series, strlen(series), 0, 99999999999999LL, 60000, "avg"), "TS.AGG");
    check(hotkv_ts_info(ctx, series, strlen(series)), "TS.INFO");
    check(hotkv_ts_retention(ctx, series, strlen(series), 3600000), "TS.RETENTION");
    check(hotkv_ts_list(ctx, NULL, 0, NULL, 0), "TS.LIST");

    check(hotkv_ts_create(ctx, dest, strlen(dest), 0, NULL, NULL, NULL, NULL, 0),
          "TS.CREATE (downsample dest)");
    check(hotkv_ts_downsample(ctx, series, strlen(series), dest, strlen(dest), "avg", 60000),
          "TS.DOWNSAMPLE");
    check(hotkv_ts_delete(ctx, dest, strlen(dest)), "TS.DELETE");
    check(hotkv_ts_delete(ctx, series, strlen(series)), "TS.DELETE (series)");
}

/* =========================================================================
 * FEATURE.*
 * ========================================================================= */

static void test_feature(hotkv_context_t *ctx) {
    printf("\n-- FEATURE.* --\n");
    char *group = key("feat");
    const char *names[] = {"score"};
    size_t namelens[] = {5};
    const char *types[] = {"int64"};
    size_t typelens[] = {5};
    const char *values[] = {"42"};
    size_t valuelens[] = {2};
    const char *entities[] = {"e1", "e2"};
    size_t entitylens[] = {2, 2};

    check(hotkv_feature_create(ctx, group, strlen(group), "desc", 4, names, namelens, types,
                               typelens, 1),
          "FEATURE.CREATE");
    check(hotkv_feature_set(ctx, group, strlen(group), "e1", 2, names, namelens, values,
                            valuelens, 1),
          "FEATURE.SET");
    check(hotkv_feature_get(ctx, group, strlen(group), "e1", 2, NULL, NULL, 0), "FEATURE.GET");
    check(hotkv_feature_batch(ctx, group, strlen(group), entities, entitylens, 2, names,
                              namelens, 1),
          "FEATURE.BATCH");
    check(hotkv_feature_schema(ctx, group, strlen(group)), "FEATURE.SCHEMA");
    check(hotkv_feature_stats(ctx, group, strlen(group)), "FEATURE.STATS");
    check(hotkv_feature_list(ctx), "FEATURE.LIST");
    check(hotkv_feature_drop(ctx, group, strlen(group)), "FEATURE.DROP");
}

/* =========================================================================
 * OBSERVE / ALERTS
 * ========================================================================= */

static void test_observe(hotkv_context_t *ctx) {
    printf("\n-- OBSERVE / ALERTS --\n");
    check(hotkv_observe_info(ctx), "OBSERVE INFO");
    check(hotkv_observe_metrics(ctx), "OBSERVE METRICS");
    check(hotkv_alerts_info(ctx), "ALERTS INFO");
}

/* =========================================================================
 * AI helpers: PromptCache and SemanticCache get-or-call
 * ========================================================================= */

static int fake_llm_call(void *userdata, char **out_response, hotkv_llm_usage_t *out_usage) {
    int *calls = (int *)userdata;
    (*calls)++;
    *out_response = hotkv_strdup("42");
    out_usage->model = "gpt-4";
    out_usage->input_tokens = 5;
    out_usage->output_tokens = 1;
    out_usage->cost_usd = 0.001;
    out_usage->latency_ms = 10;
    return *out_response ? HOTKV_OK : HOTKV_ERR_OOM;
}

static void test_promptcache_helper(hotkv_context_t *ctx) {
    printf("\n-- PromptCache helper (needs a license with the prompt-cache feature) --\n");
    hotkv_promptcache_t pc;
    int calls = 0;
    char *resp = NULL;
    int hit = 0;
    const char *roles[] = {"user"};
    const char *contents[] = {"what is the answer?"};

    hotkv_promptcache_init(&pc, ctx, key("pc-helper"), 60);
    hotkv_promptcache_set_stats_bucket(&pc, key("pc-helper-stats"));
    g_checks++;
    if (hotkv_promptcache_create_namespace(&pc, 0, 0.0) == HOTKV_OK) {
        g_ok++;
    } else {
        g_failed++;
        printf("  FAIL PromptCache.CreateNamespace\n");
    }

    g_checks++;
    int rc = hotkv_promptcache_get_or_call(&pc, "test", "test-model", NULL, roles, contents, 1,
                                           NULL, NULL, 0, fake_llm_call, &calls, &resp, &hit);
    if (rc == HOTKV_OK && resp && strcmp(resp, "42") == 0 && !hit && calls == 1) {
        g_ok++;
    } else {
        g_failed++;
        printf("  FAIL PromptCache.GetOrCall (miss) rc=%d resp=%s hit=%d calls=%d\n", rc,
               resp ? resp : "(null)", hit, calls);
    }
    /* Usage savings are counted on hits only, so ask the same prompt again. */
    {
        char *resp2 = NULL;
        int hit2 = 0;
        g_checks++;
        rc = hotkv_promptcache_get_or_call(&pc, "test", "test-model", NULL, roles, contents, 1,
                                           NULL, NULL, 0, fake_llm_call, &calls, &resp2, &hit2);
        if (rc == HOTKV_OK && resp2 && strcmp(resp2, "42") == 0 && hit2 && calls == 1) {
            g_ok++;
        } else {
            g_failed++;
            printf("  FAIL PromptCache.GetOrCall (hit) rc=%d hit=%d calls=%d\n", rc, hit2, calls);
        }
        hotkv_free(resp2);
    }
    {
        hotkv_pcache_stats_t stats;
        int prc = hotkv_promptcache_stats(&pc, &stats);
        g_checks++;
        if (prc == HOTKV_OK) {
            if (stats.usage.requests >= 1 && stats.usage.input_tokens >= 5) {
                g_ok++;
            } else {
                g_failed++;
                printf("  FAIL PromptCache.Stats usage_saved (requests=%lld input_tokens=%lld)\n",
                       (long long)stats.usage.requests, (long long)stats.usage.input_tokens);
            }
        } else {
            g_skipped++;
            printf("  SKIP PromptCache.Stats (prompt-cache not licensed on this test server)\n");
        }
    }
    hotkv_free(resp);
    hotkv_promptcache_destroy(&pc);
}

static int fake_semantic_call(void *userdata, char **out_response, hotkv_llm_usage_t *out_usage) {
    int *calls = (int *)userdata;
    (*calls)++;
    *out_response = hotkv_strdup("paris");
    out_usage->model = "gpt-4";
    out_usage->input_tokens = 10;
    out_usage->output_tokens = 4;
    return *out_response ? HOTKV_OK : HOTKV_ERR_OOM;
}

static void test_semanticcache_helper(hotkv_context_t *ctx) {
    printf("\n-- SemanticCache helper --\n");
    hotkv_semanticcache_t sc;
    int calls = 0;
    char *resp1 = NULL;
    char *resp2 = NULL;
    int hit1 = 0, hit2 = 0;
    float vec[3] = {0.4f, 0.5f, 0.6f};

    hotkv_semanticcache_init(&sc, ctx, key("sc-helper"), 0);
    {
        hotkv_reply_t *r = hotkv_scache_create(ctx, sc.ns, strlen(sc.ns), 3, "cosine", NULL, 0);
        g_checks++;
        if (r && hotkv_reply_is_nolicense(r)) {
            g_skipped++;
            printf("  SKIP SemanticCache helper (%s)\n", r->str);
            hotkv_free_reply(r);
            hotkv_semanticcache_destroy(&sc);
            return;
        }
        if (!r || r->type == HOTKV_REPLY_ERROR) {
            g_failed++;
            printf("  FAIL SemanticCache namespace setup\n");
            hotkv_free_reply(r);
            hotkv_semanticcache_destroy(&sc);
            return;
        }
        g_ok++;
        hotkv_free_reply(r);
    }

    g_checks++;
    int rc1 = hotkv_semanticcache_get_or_call(&sc, "capital of france?", 19, vec, 3,
                                              fake_semantic_call, &calls, &resp1, &hit1);
    if (rc1 == HOTKV_OK && resp1 && strcmp(resp1, "paris") == 0 && !hit1 && calls == 1) {
        g_ok++;
    } else {
        g_failed++;
        printf("  FAIL SemanticCache.GetOrCall (miss) rc=%d resp=%s hit=%d calls=%d\n", rc1,
               resp1 ? resp1 : "(null)", hit1, calls);
    }

    g_checks++;
    int rc2 = hotkv_semanticcache_get_or_call(&sc, "capital of france?", 19, vec, 3,
                                              fake_semantic_call, &calls, &resp2, &hit2);
    if (rc2 == HOTKV_OK && resp2 && strcmp(resp2, "paris") == 0 && hit2 && calls == 1) {
        g_ok++;
    } else {
        g_failed++;
        printf("  FAIL SemanticCache.GetOrCall (hit) rc=%d resp=%s hit=%d calls=%d\n", rc2,
               resp2 ? resp2 : "(null)", hit2, calls);
    }

    {
        hotkv_scache_stats_t stats;
        g_checks++;
        if (hotkv_semanticcache_stats(&sc, &stats) == HOTKV_OK && stats.entries >= 1) {
            g_ok++;
        } else {
            g_failed++;
            printf("  FAIL SemanticCache.Stats\n");
        }
    }

    hotkv_free(resp1);
    hotkv_free(resp2);
    hotkv_semanticcache_destroy(&sc);
}

/* =========================================================================
 * RESP2 and RESP3 conformance: HGETALL, HRANDFIELD WITHVALUES, SMEMBERS,
 * ZSCORE, ZRANGE WITHSCORES, ZPOPMIN with a count, ZRANDMEMBER WITHSCORES,
 * XREAD, XINFO STREAM, CONFIG GET and INFO must decode to the same shape
 * under both protocol versions.
 * ========================================================================= */

static int validate_hgetall(hotkv_reply_t *r) {
    if (r->nelem != 4) return 0;
    int f1 = 0, f2 = 0;
    for (size_t i = 0; i + 1 < r->nelem; i += 2) {
        if (strcmp(r->elements[i]->str, "f1") == 0 && strcmp(r->elements[i + 1]->str, "v1") == 0) f1 = 1;
        if (strcmp(r->elements[i]->str, "f2") == 0 && strcmp(r->elements[i + 1]->str, "v2") == 0) f2 = 1;
    }
    return f1 && f2;
}

static int validate_hrandfield_withvalues(hotkv_reply_t *r) {
    if (hotkv_reply_pair_count(r) != 2) return 0;
    int f1 = 0, f2 = 0;
    for (size_t i = 0; i < 2; i++) {
        hotkv_reply_t *k = NULL, *v = NULL;
        if (!hotkv_reply_pair_at(r, i, &k, &v)) return 0;
        if (strcmp(k->str, "f1") == 0 && strcmp(v->str, "v1") == 0) f1 = 1;
        if (strcmp(k->str, "f2") == 0 && strcmp(v->str, "v2") == 0) f2 = 1;
    }
    return f1 && f2;
}

static int validate_smembers(hotkv_reply_t *r) {
    if (r->nelem != 3) return 0;
    int a = 0, b = 0, c = 0;
    for (size_t i = 0; i < r->nelem; i++) {
        if (strcmp(r->elements[i]->str, "a") == 0) a = 1;
        if (strcmp(r->elements[i]->str, "b") == 0) b = 1;
        if (strcmp(r->elements[i]->str, "c") == 0) c = 1;
    }
    return a && b && c;
}

/* Reads a score reply as a double: RESP3 sends a native double, RESP2 (or
 * a RESP3 connection reading a WITHSCORES member's paired value) a bulk
 * string holding the formatted number. */
static double reply_num(const hotkv_reply_t *r) {
    if (r->type == HOTKV_REPLY_DOUBLE) return r->dval;
    return atof(r->str);
}

static int validate_zscore_b(hotkv_reply_t *r) {
    return reply_num(r) == 2.0;
}

static int validate_zrange_withscores(hotkv_reply_t *r) {
    static const char *names[] = {"a", "b", "c"};
    static const double scores[] = {1, 2, 3};
    if (hotkv_reply_pair_count(r) != 3) return 0;
    for (size_t i = 0; i < 3; i++) {
        hotkv_reply_t *k = NULL, *v = NULL;
        if (!hotkv_reply_pair_at(r, i, &k, &v)) return 0;
        if (strcmp(k->str, names[i]) != 0 || reply_num(v) != scores[i]) return 0;
    }
    return 1;
}

static int validate_zpopmin_count2(hotkv_reply_t *r) {
    hotkv_reply_t *k = NULL, *v = NULL;
    if (hotkv_reply_pair_count(r) != 2) return 0;
    if (!hotkv_reply_pair_at(r, 0, &k, &v) || strcmp(k->str, "a") != 0 || reply_num(v) != 1) return 0;
    if (!hotkv_reply_pair_at(r, 1, &k, &v) || strcmp(k->str, "b") != 0 || reply_num(v) != 2) return 0;
    return 1;
}

static int validate_zrandmember_withscores3(hotkv_reply_t *r) {
    if (hotkv_reply_pair_count(r) != 3) return 0;
    int a = 0, b = 0, c = 0;
    for (size_t i = 0; i < 3; i++) {
        hotkv_reply_t *k = NULL, *v = NULL;
        if (!hotkv_reply_pair_at(r, i, &k, &v)) return 0;
        if (strcmp(k->str, "a") == 0 && reply_num(v) == 1) a = 1;
        if (strcmp(k->str, "b") == 0 && reply_num(v) == 2) b = 1;
        if (strcmp(k->str, "c") == 0 && reply_num(v) == 3) c = 1;
    }
    return a && b && c;
}

static char g_xread_stream_key[256];
static char g_xread_id1[64];
static char g_xread_id2[64];

static int validate_xread(hotkv_reply_t *r) {
    hotkv_reply_t *sname = NULL, *entries = NULL;
    if (hotkv_reply_pair_count(r) != 1) return 0;
    if (!hotkv_reply_pair_at(r, 0, &sname, &entries)) return 0;
    if (strcmp(sname->str, g_xread_stream_key) != 0) return 0;
    if (entries->nelem != 2) return 0;

    hotkv_reply_t *e0 = entries->elements[0];
    hotkv_reply_t *e1 = entries->elements[1];
    if (e0->nelem != 2 || strcmp(e0->elements[0]->str, g_xread_id1) != 0) return 0;
    if (e1->nelem != 2 || strcmp(e1->elements[0]->str, g_xread_id2) != 0) return 0;

    hotkv_reply_t *fk = NULL, *fv = NULL;
    if (hotkv_reply_pair_count(e0->elements[1]) != 1) return 0;
    if (!hotkv_reply_pair_at(e0->elements[1], 0, &fk, &fv)) return 0;
    return strcmp(fk->str, "field1") == 0 && strcmp(fv->str, "v1") == 0;
}

static int validate_xinfo_stream(hotkv_reply_t *r) {
    for (size_t i = 0; i + 1 < r->nelem; i += 2) {
        if (strcmp(r->elements[i]->str, "length") == 0) {
            return r->elements[i + 1]->integer == 2;
        }
    }
    return 0;
}

static int validate_config_get_maxmemory(hotkv_reply_t *r) {
    for (size_t i = 0; i + 1 < r->nelem; i += 2) {
        if (strcmp(r->elements[i]->str, "maxmemory") == 0) return 1;
    }
    return 0;
}

static int validate_info(hotkv_reply_t *r) {
    /* Must not leak the RESP3 verbatim string "txt:"/"mkd:" encoding
     * prefix, and must contain a field every INFO reply carries. */
    if (r->len >= 4 && (memcmp(r->str, "txt:", 4) == 0 || memcmp(r->str, "mkd:", 4) == 0)) return 0;
    return strstr(r->str, "hotkv_version") != NULL;
}

static void test_resp_conformance(const hotkv_url_t *u, int protocol, const char *label) {
    printf("\n-- RESP conformance (%s) --\n", label);

    hotkv_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.host = u->host;
    opts.port = u->port;
    opts.username = u->username;
    opts.password = u->password;
    opts.db = u->db;
    opts.protocol = protocol;

    hotkv_context_t *ctx = hotkv_connect_with_options(&opts);
    if (!ctx || hotkv_context_err(ctx) != HOTKV_OK) {
        g_checks++;
        g_failed++;
        printf("  FAIL connect (%s)\n", label);
        return;
    }

    /* label distinguishes the RESP2 and RESP3 runs' keys: XADD always
     * creates fresh entries, so sharing a stream key across both runs
     * would make the second run's stream longer than expected. */
    char buf[80];
    snprintf(buf, sizeof(buf), "conform-%s:hash", label);
    char *hkey = key(buf);
    snprintf(buf, sizeof(buf), "conform-%s:set", label);
    char *skey = key(buf);
    snprintf(buf, sizeof(buf), "conform-%s:zset", label);
    char *zkey = key(buf);
    snprintf(buf, sizeof(buf), "conform-%s:stream", label);
    char *xkey = key(buf);
    strncpy(g_xread_stream_key, xkey, sizeof(g_xread_stream_key) - 1);

    {
        const char *fields[] = {"f1", "f2"};
        size_t flens[] = {2, 2};
        const char *vals[] = {"v1", "v2"};
        size_t vlens[] = {2, 2};
        check(hotkv_hset(ctx, hkey, strlen(hkey), fields, flens, vals, vlens, 2), "HSET");
    }
    check_shape(hotkv_hgetall(ctx, hkey, strlen(hkey)), "HGETALL", validate_hgetall);
    check_shape(hotkv_hrandfield_count(ctx, hkey, strlen(hkey), 2, 1),
                "HRANDFIELD ... WITHVALUES", validate_hrandfield_withvalues);

    {
        const char *members[] = {"a", "b", "c"};
        size_t mlens[] = {1, 1, 1};
        check(hotkv_sadd(ctx, skey, strlen(skey), members, mlens, 3), "SADD");
    }
    check_shape(hotkv_smembers(ctx, skey, strlen(skey)), "SMEMBERS", validate_smembers);

    {
        double scores[] = {1, 2, 3};
        const char *members[] = {"a", "b", "c"};
        size_t mlens[] = {1, 1, 1};
        check(hotkv_zadd(ctx, zkey, strlen(zkey), scores, members, mlens, 3), "ZADD");
    }
    check_shape(hotkv_zscore(ctx, zkey, strlen(zkey), "b", 1), "ZSCORE", validate_zscore_b);
    check_shape(hotkv_zrange(ctx, zkey, strlen(zkey), 0, -1, 1), "ZRANGE ... WITHSCORES",
                validate_zrange_withscores);
    check_shape(hotkv_zpopmin(ctx, zkey, strlen(zkey), 2), "ZPOPMIN with a count",
                validate_zpopmin_count2);
    {
        double scores[] = {1, 2};
        const char *members[] = {"a", "b"};
        size_t mlens[] = {1, 1};
        check(hotkv_zadd(ctx, zkey, strlen(zkey), scores, members, mlens, 2), "ZADD (restore a, b)");
    }
    check_shape(hotkv_zrandmember_count(ctx, zkey, strlen(zkey), 3, 1), "ZRANDMEMBER ... WITHSCORES",
                validate_zrandmember_withscores3);

    {
        const char *f1[] = {"field1"};
        size_t f1len[] = {6};
        const char *v1[] = {"v1"};
        size_t v1len[] = {2};
        hotkv_reply_t *r1 = hotkv_xadd(ctx, xkey, strlen(xkey), "*", 1, f1, f1len, v1, v1len, 1);
        g_checks++;
        if (r1 && r1->type == HOTKV_REPLY_BULK) {
            g_ok++;
            strncpy(g_xread_id1, r1->str, sizeof(g_xread_id1) - 1);
        } else {
            g_failed++;
            printf("  FAIL XADD 1\n");
        }
        hotkv_free_reply(r1);

        const char *f2[] = {"field2"};
        size_t f2len[] = {6};
        const char *v2[] = {"v2"};
        size_t v2len[] = {2};
        hotkv_reply_t *r2 = hotkv_xadd(ctx, xkey, strlen(xkey), "*", 1, f2, f2len, v2, v2len, 1);
        g_checks++;
        if (r2 && r2->type == HOTKV_REPLY_BULK) {
            g_ok++;
            strncpy(g_xread_id2, r2->str, sizeof(g_xread_id2) - 1);
        } else {
            g_failed++;
            printf("  FAIL XADD 2\n");
        }
        hotkv_free_reply(r2);
    }
    {
        const char *keys[] = {xkey};
        size_t keylens[] = {strlen(xkey)};
        const char *ids[] = {"0"};
        size_t idlens[] = {1};
        check_shape(hotkv_xread(ctx, keys, keylens, ids, idlens, 1, -1), "XREAD", validate_xread);
    }
    check_shape(hotkv_xinfo_stream(ctx, xkey, strlen(xkey)), "XINFO STREAM", validate_xinfo_stream);
    check_shape(hotkv_config_get(ctx, "maxmemory", 9), "CONFIG GET", validate_config_get_maxmemory);
    check_shape(hotkv_info(ctx, NULL), "INFO", validate_info);

    hotkv_disconnect(ctx);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void) {
    const char *url = getenv("HOTKV_TEST_URL");
    if (!url || !*url) {
        printf("HOTKV_TEST_URL not set; skipping integration tests.\n");
        printf("Example: HOTKV_TEST_URL=hotkv://127.0.0.1:6379\n");
        return 77;
    }

    hotkv_url_t u;
    if (hotkv_url_parse(url, &u) != HOTKV_OK) {
        fprintf(stderr, "invalid HOTKV_TEST_URL: %s\n", url);
        return 1;
    }

    snprintf(g_run_id, sizeof(g_run_id), "%ld-%d", (long)time(NULL), (int)hkv_getpid());

    hotkv_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.host = u.host;
    opts.port = u.port;
    opts.username = u.username;
    opts.password = u.password;
    opts.db = u.db;
    opts.protocol = HOTKV_RESP3;

    hotkv_context_t *ctx = hotkv_connect_with_options(&opts);
    if (!ctx || hotkv_context_err(ctx) != HOTKV_OK) {
        fprintf(stderr, "failed to connect to %s: %s\n", url,
                ctx ? hotkv_context_errstr(ctx) : "OOM");
        hotkv_url_free(&u);
        return 1;
    }

    printf("Connected to %s (run id %s)\n", url, g_run_id);

    test_section3(ctx);
    test_pipeline_and_tx(ctx);
    test_pubsub(&u);
    test_tag_and_nexpire_and_hotkeys(ctx);
    test_gcra(ctx);
    test_vsim(ctx);
    test_scache(ctx);
    test_pcache(ctx);
    test_keyed_caches(ctx);
    test_llmstats_budget_prompt_embed(ctx);
    test_amem(ctx);
    test_rag(ctx);
    test_ratelimit(ctx);
    test_ts(ctx);
    test_feature(ctx);
    test_observe(ctx);
    test_promptcache_helper(ctx);
    test_semanticcache_helper(ctx);
    test_resp_conformance(&u, HOTKV_RESP3, "RESP3");
    test_resp_conformance(&u, HOTKV_RESP2, "RESP2");

    hotkv_disconnect(ctx);
    hotkv_url_free(&u);
    free_keys();

    printf("\n=================================\n");
    printf("Results: %d ok, %d skipped (NOLICENSE), %d failed, %d total\n", g_ok, g_skipped,
           g_failed, g_checks);

    return g_failed > 0 ? 1 : 0;
}
