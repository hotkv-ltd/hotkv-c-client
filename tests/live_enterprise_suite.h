/**
 * @file live_enterprise_suite.h
 * @brief Value-checked live tests for every typed enterprise / AI wrapper (licensed server), as a
 * header so test_live_core.c and test_remote.c share one copy. Needs
 * live_util.h included first. Entry point: live_enterprise_run(url, protocol).
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#ifndef HKV_LIVE_ENTERPRISE_SUITE_H
#define HKV_LIVE_ENTERPRISE_SUITE_H

/* Reply value for `name` in a flat key/value reply (map or pair array). */
static const hotkv_reply_t *fld(const hotkv_reply_t *r, const char *name) {
    size_t i, n = hotkv_reply_pair_count(r);
    for (i = 0; i < n; i++) {
        hotkv_reply_t *k = NULL, *v = NULL;
        if (hotkv_reply_pair_at(r, i, &k, &v) && rtext(k) && strcmp(rtext(k), name) == 0) return v;
    }
    return NULL;
}

static int fld_int(const hotkv_reply_t *r, const char *name, long long want) {
    long long got;
    const hotkv_reply_t *v = fld(r, name);
    return v && rint64(v, &got) && got == want;
}

static int fld_dbl(const hotkv_reply_t *r, const char *name, double want, double tol) {
    double got;
    const hotkv_reply_t *v = fld(r, name);
    return v && rdouble(v, &got) && got >= want - tol && got <= want + tol;
}

static int fld_str(const hotkv_reply_t *r, const char *name, const char *want) {
    const hotkv_reply_t *v = fld(r, name);
    return v && rtext(v) && strcmp(rtext(v), want) == 0;
}

/* Does the array reply contain the string `want`? */
static int list_has(const hotkv_reply_t *r, const char *want) {
    size_t i;
    if (!is_agg(r)) return 0;
    for (i = 0; i < r->nelem; i++) {
        if (rtext(r->elements[i]) && strcmp(rtext(r->elements[i]), want) == 0) return 1;
    }
    return 0;
}

#define NAMED(var, suffix) char *var = key(suffix)

/* ------------------------------------------------------------------------- */
/* True unless the server answers a real enterprise command with NOLICENSE. */
static int licensed(void) {
    hotkv_reply_t *r = hotkv_observe_info(ctx);
    int ok = r && !hotkv_reply_is_nolicense(r);
    hotkv_free_reply(r);
    return ok;
}

static void test_observe(void) {
    hotkv_reply_t *r;
    printf("-- OBSERVE, ALERTS --\n");
    r = hotkv_observe_info(ctx);
    CHECK(fld(r, "connected_clients") && fld(r, "uptime_seconds") && fld(r, "total_commands"),
          "OBSERVE INFO fields");
    hotkv_free_reply(r);
    r = hotkv_observe_metrics(ctx);
    CHECK(fld(r, "used_memory_bytes") && fld(r, "num_workers"), "OBSERVE METRICS fields");
    hotkv_free_reply(r);
    r = hotkv_alerts_info(ctx);
    CHECK(fld(r, "enabled") && fld(r, "min_severity") && fld(r, "supported_providers"), "ALERTS INFO fields");
    hotkv_free_reply(r);
}

/* ------------------------------------------------------------------------- */
static void test_gcra_vsim(void) {
    NAMED(g, "gcra");
    NAMED(idx, "vx");
    char snap[160];
    float v1[3] = {1, 0, 0}, v2[3] = {0, 1, 0}, v3[3] = {0.6f, 0.8f, 0};
    hotkv_reply_t *r;
    long long a = -9, rem = -9;
    printf("-- GCRA, VSIM --\n");

    /* GCRA.LIMIT key period_ms burst count -> [limited, limit, remaining, retry_after, reset_after] */
    r = hotkv_gcra_limit(ctx, KL(g), 1000, 2, 1);
    CHECK(is_agg(r) && r->nelem == 5, "GCRA.LIMIT returns 5 values");
    CHECK(rint64(elem(r, 0), &a) && a == 0 && rint64(elem(r, 2), &rem) && rem == 1,
          "first call allowed, 1 remaining of the burst of 2");
    hotkv_free_reply(r);
    hotkv_free_reply(hotkv_gcra_limit(ctx, KL(g), 1000, 2, 1));
    r = hotkv_gcra_limit(ctx, KL(g), 1000, 2, 1);
    a = -9;
    CHECK(is_agg(r) && rint64(elem(r, 0), &a) && a == 1, "third call is rate limited");
    hotkv_free_reply(r);
    expect_int(hotkv_gcra_reset(ctx, ARR(g), (size_t[]){strlen(g)}, 1), 1, "GCRA.RESET");

    expect_ok(hotkv_vsim_createindex(ctx, KL(idx), 3, "cosine", 0, 0, 0), "VSIM.CREATEINDEX");
    expect_int(hotkv_vsim_set(ctx, KL(idx), "k1", 2, v1, 3), 1, "VSIM.SET k1");
    expect_int(hotkv_vsim_set(ctx, KL(idx), "k2", 2, v2, 3), 1, "VSIM.SET k2");
    r = hotkv_vsim_get(ctx, KL(idx), "k1", 2);
    {
        double x = 0, y = 9, z = 9;
        CHECK(is_agg(r) && r->nelem == 3 && rdouble(elem(r, 0), &x) && rdouble(elem(r, 1), &y) &&
                  rdouble(elem(r, 2), &z) && x == 1.0 && y == 0.0 && z == 0.0,
              "VSIM.GET returns the stored vector");
    }
    hotkv_free_reply(r);
    r = hotkv_vsim_search(ctx, KL(idx), 2, v3, 3, NULL, NULL, 0, 0);
    {
        double d0 = -1, d1 = -1;
        CHECK(is_agg(r) && r->nelem == 4 && strcmp(rtext(elem(r, 0)), "k2") == 0 && rdouble(elem(r, 1), &d0) &&
                  strcmp(rtext(elem(r, 2)), "k1") == 0 && rdouble(elem(r, 3), &d1) && d0 < d1,
              "VSIM.SEARCH orders by distance (k2 is closer to (0.6,0.8,0))");
    }
    hotkv_free_reply(r);
    r = hotkv_vsim_info(ctx, KL(idx));
    CHECK(fld_int(r, "dimensions", 3) && fld_int(r, "vectors", 2) && fld_str(r, "metric", "cosine"), "VSIM.INFO");
    hotkv_free_reply(r);
    r = hotkv_vsim_info(ctx, NULL, 0);
    CHECK(list_has(r, idx), "VSIM.INFO (no index) lists the index");
    hotkv_free_reply(r);
    expect_int(hotkv_vsim_del(ctx, KL(idx), "k1", 2), 1, "VSIM.DEL");
    expect_nil(hotkv_vsim_get(ctx, KL(idx), "k1", 2), "VSIM.GET after DEL");

    snprintf(snap, sizeof(snap), "sdk_%s_snap", g_run_id);
    {
        char *p;
        for (p = snap; *p; p++) {
            if (*p == '.' || *p == ':' || *p == '/') *p = '_';
        }
    }
    expect_int_at_least(hotkv_vsim_save(ctx, snap, strlen(snap), "blob-data", 9), 9, "VSIM.SAVE");
    expect_text(hotkv_vsim_load(ctx, snap, strlen(snap)), "blob-data", "VSIM.LOAD returns the blob");
    r = hotkv_vsim_list(ctx);
    CHECK(list_has(r, snap), "VSIM.LIST lists the snapshot");
    hotkv_free_reply(r);
    expect_int(hotkv_vsim_drop(ctx, snap, strlen(snap)), 1, "VSIM.DROP");
    expect_ok(hotkv_vsim_dropindex(ctx, KL(idx)), "VSIM.DROPINDEX");
}

/* ------------------------------------------------------------------------- */
static void test_caches(void) {
    NAMED(sc, "scache");
    NAMED(pc, "pcache");
    float v1[3] = {1, 0, 0}, v2[3] = {0, 1, 0}, v3[3] = {0.5f, 0.5f, 0};
    double thr = 0.5, thr_hi = 0.9;
    hotkv_token_usage_t usage;
    hotkv_reply_t *r;
    printf("-- SCACHE, PCACHE --\n");
    memset(&usage, 0, sizeof(usage));
    usage.model = "gpt-4";
    usage.input_tokens = 100;
    usage.output_tokens = 50;

    expect_ok(hotkv_scache_create(ctx, KL(sc), 3, "cosine", &thr_hi, 0), "SCACHE.CREATE");
    expect_ok(hotkv_scache_set(ctx, KL(sc), "capital of france?", 18, "paris", 5, v1, 3, 0, &usage), "SCACHE.SET");
    r = hotkv_scache_get(ctx, KL(sc), v1, 3, NULL);
    {
        double sim = 0;
        CHECK(is_agg(r) && r->nelem == 2 && strcmp(rtext(elem(r, 0)), "paris") == 0 && rdouble(elem(r, 1), &sim) &&
                  sim > 0.99,
              "SCACHE.GET hit returns [response, similarity]");
    }
    hotkv_free_reply(r);
    expect_nil(hotkv_scache_get(ctx, KL(sc), v2, 3, NULL), "SCACHE.GET orthogonal vector misses");
    r = hotkv_scache_stats(ctx, KL(sc));
    {
        hotkv_scache_stats_t st;
        CHECK(r && hotkv_parse_scache_stats(r, &st) == HOTKV_OK, "parse SCACHE.STATS");
        CHECK(st.entries == 1 && st.hits == 1 && st.misses == 1, "SCACHE.STATS entries/hits/misses");
        CHECK(st.hit_rate > 0.49 && st.hit_rate < 0.51, "SCACHE.STATS hit_rate 0.5");
        CHECK(st.usage.requests == 1 && st.usage.input_tokens == 100 && st.usage.output_tokens == 50,
              "SCACHE.STATS usage savings (1 hit x 100/50 tokens)");
    }
    hotkv_free_reply(r);
    /* (0.5,0.5,0) has cosine similarity 0.707 to the stored (1,0,0): below 0.9. */
    expect_nil(hotkv_scache_get(ctx, KL(sc), v3, 3, NULL), "SCACHE.GET below the namespace threshold misses");
    /* The per-call THRESHOLD override is only checked to be accepted here. */
    expect_noerr(hotkv_scache_get(ctx, KL(sc), v3, 3, &thr), "SCACHE.GET with THRESHOLD override is accepted");
    expect_ok(hotkv_scache_threshold(ctx, KL(sc), 0.5), "SCACHE.THRESHOLD 0.5");
    r = hotkv_scache_get(ctx, KL(sc), v3, 3, NULL);
    {
        double sim = 0;
        CHECK(is_agg(r) && r->nelem == 2 && strcmp(rtext(elem(r, 0)), "paris") == 0 && rdouble(elem(r, 1), &sim) &&
                  sim > 0.70 && sim < 0.72,
              "SCACHE.GET hits after lowering the namespace threshold (similarity 0.707)");
    }
    hotkv_free_reply(r);
    r = hotkv_scache_list(ctx);
    CHECK(list_has(r, sc), "SCACHE.LIST");
    hotkv_free_reply(r);
    expect_int(hotkv_scache_flush(ctx, KL(sc)), 1, "SCACHE.FLUSH");
    expect_nil(hotkv_scache_get(ctx, KL(sc), v1, 3, NULL), "SCACHE.GET after FLUSH");
    expect_int(hotkv_scache_drop(ctx, KL(sc)), 1, "SCACHE.DROP");

    expect_ok(hotkv_pcache_create(ctx, KL(pc), 100, 600, 0.001), "PCACHE.CREATE with float cost");
    expect_ok(hotkv_pcache_set(ctx, KL(pc), "hello", 5, "world", 5, 0, "meta1", 5, &usage), "PCACHE.SET");
    expect_text(hotkv_pcache_get(ctx, KL(pc), "hello", 5), "world", "PCACHE.GET");
    expect_nil(hotkv_pcache_get(ctx, KL(pc), "other", 5), "PCACHE.GET miss");
    r = hotkv_pcache_getex(ctx, KL(pc), "hello", 5);
    CHECK(is_agg(r) && r->nelem == 3 && strcmp(rtext(elem(r, 0)), "world") == 0 &&
              strcmp(rtext(elem(r, 1)), "meta1") == 0,
          "PCACHE.GETEX returns [response, meta, ...]");
    hotkv_free_reply(r);
    r = hotkv_pcache_stats(ctx, KL(pc));
    {
        hotkv_pcache_stats_t st;
        CHECK(r && hotkv_parse_pcache_stats(r, &st) == HOTKV_OK, "parse PCACHE.STATS");
        CHECK(st.entries == 1 && st.hits == 2 && st.misses == 1, "PCACHE.STATS entries/hits/misses");
        CHECK(st.cost_saved_usd > 0.0019 && st.cost_saved_usd < 0.0021, "PCACHE.STATS cost_saved_usd (2 x 0.001)");
        CHECK(st.usage.requests == 2 && st.usage.input_tokens == 200, "PCACHE.STATS usage savings");
    }
    hotkv_free_reply(r);
    expect_ok(hotkv_pcache_config_set(ctx, KL(pc), "COST_PER_HIT", "0.5", 3), "PCACHE.CONFIG SET COST_PER_HIT");
    hotkv_free_reply(hotkv_pcache_get(ctx, KL(pc), "hello", 5));
    r = hotkv_pcache_stats(ctx, KL(pc));
    {
        hotkv_pcache_stats_t st;
        CHECK(r && hotkv_parse_pcache_stats(r, &st) == HOTKV_OK && st.cost_saved_usd > 0.49,
              "cost per hit updated by CONFIG (saved cost jumps by 0.5)");
    }
    hotkv_free_reply(r);
    r = hotkv_pcache_list(ctx);
    CHECK(list_has(r, pc), "PCACHE.LIST");
    hotkv_free_reply(r);
    expect_int(hotkv_pcache_del(ctx, KL(pc), "hello", 5), 1, "PCACHE.DEL");
    expect_int(hotkv_pcache_del(ctx, KL(pc), "hello", 5), 0, "PCACHE.DEL again");
    expect_int(hotkv_pcache_flush(ctx, KL(pc)), 0, "PCACHE.FLUSH on an empty cache");
    expect_int(hotkv_pcache_drop(ctx, KL(pc)), 1, "PCACHE.DROP");
}

/* Exercises one keyed cache family through its generated functions. */
#define KEYED_FAMILY_TEST(FNAME, NAME, LABEL)                                                        \
    static void FNAME(void) {                                                                       \
        char *ns = key(LABEL);                                                                      \
        hotkv_token_usage_t usage;                                                                  \
        hotkv_reply_t *r;                                                                           \
        hotkv_keyedcache_stats_t st;                                                                \
        memset(&usage, 0, sizeof(usage));                                                           \
        usage.model = "gpt-4";                                                                      \
        usage.input_tokens = 10;                                                                    \
        usage.output_tokens = 4;                                                                    \
        printf("-- " LABEL " --\n");                                                                \
        expect_ok(hotkv_##NAME##_create(ctx, KL(ns), 100, 600), LABEL ".CREATE");                    \
        expect_ok(hotkv_##NAME##_set(ctx, KL(ns), "k1", 2, "v1", 2, 0, "m", 1, &usage), LABEL ".SET"); \
        expect_text(hotkv_##NAME##_get(ctx, KL(ns), "k1", 2), "v1", LABEL ".GET");                   \
        expect_nil(hotkv_##NAME##_get(ctx, KL(ns), "zz", 2), LABEL ".GET miss");                     \
        r = hotkv_##NAME##_stats(ctx, KL(ns));                                                       \
        CHECK(r && hotkv_parse_keyedcache_stats(r, &st) == HOTKV_OK && st.entries == 1 &&           \
                  st.hits == 1 && st.misses == 1 && st.usage.requests == 1 &&                       \
                  st.usage.input_tokens == 10 && st.usage.output_tokens == 4,                       \
              LABEL ".STATS entries/hits/misses/usage");                                            \
        hotkv_free_reply(r);                                                                        \
        r = hotkv_##NAME##_list(ctx);                                                               \
        CHECK(list_has(r, ns), LABEL ".LIST");                                                      \
        hotkv_free_reply(r);                                                                        \
        expect_int(hotkv_##NAME##_del(ctx, KL(ns), "k1", 2), 1, LABEL ".DEL");                       \
        expect_ok(hotkv_##NAME##_set(ctx, KL(ns), "k2", 2, "v2", 2, 0, NULL, 0, NULL), LABEL ".SET again"); \
        expect_int(hotkv_##NAME##_flush(ctx, KL(ns)), 1, LABEL ".FLUSH");                            \
        expect_int(hotkv_##NAME##_drop(ctx, KL(ns)), 1, LABEL ".DROP");                              \
    }

KEYED_FAMILY_TEST(test_toolcache, toolcache, "TOOL.CACHE")
KEYED_FAMILY_TEST(test_ncache, ncache, "NCACHE")
KEYED_FAMILY_TEST(test_guardcache, guardcache, "GUARD")
KEYED_FAMILY_TEST(test_rerankcache, rerankcache, "RERANK")

/* ------------------------------------------------------------------------- */
static void test_llm(void) {
    NAMED(b, "bucket");
    NAMED(bud, "budget");
    NAMED(bud2, "budget2");
    NAMED(bud3, "budget3");
    char model[96];
    hotkv_reply_t *r;
    int err = 1;
    hotkv_token_usage_t usage;
    printf("-- LLMSTATS, LLMBUDGET --\n");

    expect_ok(hotkv_llmstats_bucket(ctx, KL(b)), "LLMSTATS.BUCKET");
    expect_ok(hotkv_llmstats_record(ctx, KL(b), 1500, 500, 0, 0.012, 350, NULL, NULL, 0, 0), "LLMSTATS.RECORD");
    expect_ok(hotkv_llmstats_record(ctx, KL(b), 100, 0, 0, 0.001, 50, &err, NULL, 0, 0), "LLMSTATS.RECORD ERROR 1");
    r = hotkv_llmstats_get(ctx, KL(b));
    {
        hotkv_llmstats_get_t g;
        CHECK(r && hotkv_parse_llmstats_get(r, &g) == HOTKV_OK, "parse LLMSTATS.GET");
        CHECK(g.requests == 2 && g.errors == 1 && g.input_tokens == 1600 && g.output_tokens == 500,
              "LLMSTATS.GET counters");
        CHECK(g.cost_usd > 0.0129 && g.cost_usd < 0.0131, "LLMSTATS.GET cost_usd 0.013 (0.012 + 0.001)");
        CHECK(g.avg_latency_ms > 199 && g.avg_latency_ms < 201, "LLMSTATS.GET avg_latency_ms 200");
        CHECK(g.requests_1m == 2, "LLMSTATS.GET rolling window");
    }
    hotkv_free_reply(r);
    r = hotkv_llmstats_list(ctx);
    CHECK(list_has(r, b), "LLMSTATS.LIST");
    hotkv_free_reply(r);

    /* custom prices: tiny and large USD-per-Mtok values must be accepted */
    snprintf(model, sizeof(model), "sdk-model-%d", (int)hkv_getpid());
    expect_ok(hotkv_llmstats_price_set(ctx, model, strlen(model), 0.8, 2.4, NULL, NULL, NULL), "PRICE SET");
    r = hotkv_llmstats_price_get(ctx, model, strlen(model));
    {
        hotkv_llm_price_t p;
        CHECK(r && hotkv_parse_llm_price(r, &p) == HOTKV_OK, "parse PRICE GET");
        CHECK(p.priced == 1 && strcmp(p.source, "custom") == 0, "PRICE GET custom/priced");
        CHECK(p.input_usd_per_mtok > 0.799 && p.input_usd_per_mtok < 0.801 && p.output_usd_per_mtok > 2.399 &&
                  p.output_usd_per_mtok < 2.401,
              "PRICE GET input/output");
    }
    hotkv_free_reply(r);
    {
        double cr = 0.000001, w5 = 1234567.5, w1 = 0.0;
        expect_ok(hotkv_llmstats_price_set(ctx, model, strlen(model), 1e-5, 12345678.25, &cr, &w5, &w1),
                  "PRICE SET tiny/huge/zero (1e-5, 12345678.25, 0.000001, 1234567.5, 0)");
        r = hotkv_llmstats_price_get(ctx, model, strlen(model));
        CHECK(fld_dbl(r, "input_usd_per_mtok", 0.00001, 1e-9) && fld_dbl(r, "output_usd_per_mtok", 12345678.25, 1e-6) &&
                  fld_dbl(r, "cache_read_usd_per_mtok", 0.000001, 1e-9) &&
                  fld_dbl(r, "cache_write_5m_usd_per_mtok", 1234567.5, 1e-6),
              "PRICE GET returns exactly what was set");
        hotkv_free_reply(r);
    }
    expect_err(hotkv_llmstats_price_set(ctx, model, strlen(model), -1.0, 1.0, NULL, NULL, NULL), "ERR invalid numeric",
               "PRICE SET negative is refused locally");
    r = hotkv_llmstats_price_list(ctx);
    CHECK(is_agg(r) && r->nelem > 5, "PRICE LIST includes the built-in table");
    hotkv_free_reply(r);
    expect_ok(hotkv_llmstats_record(ctx, KL(b), 1000, 200, 0, 0, 15, NULL, model, 0, 0), "RECORD priced by MODEL");
    r = hotkv_llmstats_get(ctx, KL(b));
    {
        hotkv_llmstats_get_t g;
        CHECK(r && hotkv_parse_llmstats_get(r, &g) == HOTKV_OK && g.requests == 3 && g.unpriced_requests == 0,
              "server-priced request counted");
    }
    hotkv_free_reply(r);
    expect_int(hotkv_llmstats_price_del(ctx, model, strlen(model)), 1, "PRICE DEL");
    expect_int(hotkv_llmstats_price_del(ctx, model, strlen(model)), 0, "PRICE DEL again");
    expect_int(hotkv_llmstats_reset(ctx, KL(b)), 1, "LLMSTATS.RESET");
    expect_int(hotkv_llmstats_drop(ctx, KL(b)), 1, "LLMSTATS.DROP");

    /* LLMBUDGET: float limits in every awkward range. The server only accepts
     * [0-9]+(.[0-9]{1,6})?, so "1e-05" or "1e+09" would be rejected. */
    expect_ok(hotkv_llmbudget_set(ctx, KL(bud), 50000, 1.5, 60), "LLMBUDGET.SET 1.5");
    expect_int(hotkv_llmbudget_consume(ctx, KL(bud), 1000, 0.02, NULL), 1, "CONSUME allowed");
    expect_int(hotkv_llmbudget_consume(ctx, KL(bud), 60000, 1.0, NULL), 0, "CONSUME over the token cap is rejected");
    r = hotkv_llmbudget_status(ctx, KL(bud));
    {
        hotkv_llmbudget_status_t s;
        CHECK(r && hotkv_parse_llmbudget_status(r, &s) == HOTKV_OK, "parse STATUS");
        CHECK(s.tokens_used == 1000 && s.token_limit == 50000 && s.window_seconds == 60, "STATUS tokens/window");
        CHECK(s.cost_used_usd > 0.0199 && s.cost_used_usd < 0.0201 && s.cost_limit_usd > 1.4999 && s.cost_limit_usd < 1.5001,
              "STATUS cost used 0.02 / limit 1.5");
        CHECK(s.requests_allowed == 1 && s.requests_rejected == 1, "STATUS allowed/rejected");
    }
    hotkv_free_reply(r);

    expect_ok(hotkv_llmbudget_set(ctx, KL(bud2), 0, 0.00001, 0), "LLMBUDGET.SET 1e-5 (would be sent as 1e-05 by %g)");
    expect_int(hotkv_llmbudget_consume(ctx, KL(bud2), 0, 0.000004, NULL), 1, "CONSUME 4e-6 allowed");
    expect_int(hotkv_llmbudget_consume(ctx, KL(bud2), 0, 0.000004, NULL), 1, "CONSUME 4e-6 allowed again");
    expect_int(hotkv_llmbudget_consume(ctx, KL(bud2), 0, 0.000004, NULL), 0, "third 4e-6 exceeds the 1e-5 limit");
    r = hotkv_llmbudget_status(ctx, KL(bud2));
    CHECK(fld_dbl(r, "cost_limit_usd", 0.00001, 1e-9) && fld_dbl(r, "cost_used_usd", 0.000008, 1e-9),
          "STATUS exact micro-dollar amounts");
    hotkv_free_reply(r);

    expect_ok(hotkv_llmbudget_set(ctx, KL(bud3), 1000000000ULL, 1e9, 3600), "LLMBUDGET.SET 1e9 (would be 1e+09)");
    r = hotkv_llmbudget_status(ctx, KL(bud3));
    CHECK(fld_dbl(r, "cost_limit_usd", 1e9, 1e-3) && fld_int(r, "token_limit", 1000000000LL), "STATUS 1e9 limit");
    hotkv_free_reply(r);
    expect_ok(hotkv_llmbudget_set(ctx, KL(bud3), 0, 123456.789012, 0), "LLMBUDGET.SET with 6 decimals");
    r = hotkv_llmbudget_status(ctx, KL(bud3));
    CHECK(fld_dbl(r, "cost_limit_usd", 123456.789012, 1e-6), "STATUS 123456.789012");
    hotkv_free_reply(r);
    expect_ok(hotkv_llmbudget_set(ctx, KL(bud3), 0, 0.0, 0), "LLMBUDGET.SET 0 (no cap)");
    expect_err(hotkv_llmbudget_set(ctx, KL(bud3), 0, -1.0, 0), "ERR invalid numeric", "negative limit refused locally");
    expect_err(hotkv_llmbudget_set(ctx, KL(bud3), 0, NAN, 0), "ERR invalid numeric", "NaN limit refused locally");
    expect_text(hotkv_ping(ctx), "PONG", "connection survives the local errors");

    memset(&usage, 0, sizeof(usage));
    usage.model = "claude-sonnet-5";
    usage.input_tokens = 1000;
    usage.output_tokens = 100;
    expect_ok(hotkv_llmbudget_set(ctx, KL(bud3), 0, 5.0, 3600), "LLMBUDGET.SET 5.0");
    expect_int(hotkv_llmbudget_consume(ctx, KL(bud3), 0, 0.0, &usage), 1, "CONSUME by MODEL usage");
    r = hotkv_llmbudget_status(ctx, KL(bud3));
    CHECK(fld(r, "tokens_used") && fld_int(r, "requests_allowed", 1), "STATUS after usage consume");
    hotkv_free_reply(r);
    r = hotkv_llmbudget_list(ctx);
    CHECK(list_has(r, bud) && list_has(r, bud2) && list_has(r, bud3), "LLMBUDGET.LIST");
    hotkv_free_reply(r);
    expect_int(hotkv_llmbudget_reset(ctx, KL(bud)), 1, "LLMBUDGET.RESET");
    r = hotkv_llmbudget_status(ctx, KL(bud));
    CHECK(fld_int(r, "tokens_used", 0), "STATUS after RESET");
    hotkv_free_reply(r);
    expect_int(hotkv_llmbudget_drop(ctx, KL(bud)), 1, "LLMBUDGET.DROP");
    expect_int(hotkv_llmbudget_drop(ctx, KL(bud2)), 1, "LLMBUDGET.DROP 2");
    expect_int(hotkv_llmbudget_drop(ctx, KL(bud3)), 1, "LLMBUDGET.DROP 3");
}

/* ------------------------------------------------------------------------- */
static void test_prompt_embed(void) {
    NAMED(p, "prompt");
    NAMED(ns, "embed");
    hotkv_reply_t *r;
    printf("-- PROMPT, EMBED.DEDUP --\n");

    expect_int(hotkv_prompt_put(ctx, KL(p), "Hello {{name}}", 14, "notes", 5), 1, "PROMPT.PUT v1");
    expect_int(hotkv_prompt_put(ctx, KL(p), "Hi {{name}}", 11, NULL, 0), 2, "PROMPT.PUT v2");
    r = hotkv_prompt_get(ctx, KL(p), 1);
    CHECK(is_agg(r) && r->nelem == 4 && elem(r, 0)->integer == 1 && strcmp(rtext(elem(r, 1)), "Hello {{name}}") == 0 &&
              strcmp(rtext(elem(r, 2)), "notes") == 0,
          "PROMPT.GET v1 [version, body, meta, updated_at]");
    hotkv_free_reply(r);
    r = hotkv_prompt_get(ctx, KL(p), 0);
    CHECK(is_agg(r) && elem(r, 0)->integer == 2 && strcmp(rtext(elem(r, 1)), "Hi {{name}}") == 0,
          "PROMPT.GET 0 means the active version (v2)");
    hotkv_free_reply(r);
    r = hotkv_prompt_latest(ctx, KL(p));
    CHECK(is_agg(r) && elem(r, 0)->integer == 2, "PROMPT.LATEST v2");
    hotkv_free_reply(r);
    expect_ok(hotkv_prompt_setactive(ctx, KL(p), 1), "PROMPT.SETACTIVE 1");
    r = hotkv_prompt_latest(ctx, KL(p));
    CHECK(is_agg(r) && elem(r, 0)->integer == 1 && strcmp(rtext(elem(r, 1)), "Hello {{name}}") == 0,
          "PROMPT.LATEST after rollback is v1");
    hotkv_free_reply(r);
    expect_ints(hotkv_prompt_versions(ctx, KL(p)), 2, "PROMPT.VERSIONS", 1LL, 2LL);
    r = hotkv_prompt_list(ctx);
    CHECK(list_has(r, p), "PROMPT.LIST");
    hotkv_free_reply(r);
    expect_int(hotkv_prompt_drop(ctx, KL(p)), 1, "PROMPT.DROP");

    expect_int(hotkv_embed_dedup_put(ctx, KL(ns), "h1", 2, "ada-002", 7, "vector-bytes", 12), 1, "EMBED.DEDUP.PUT new");
    expect_int(hotkv_embed_dedup_put(ctx, KL(ns), "h1", 2, "ada-002", 7, "vector-bytes", 12), 0, "EMBED.DEDUP.PUT duplicate");
    expect_text(hotkv_embed_dedup_get(ctx, KL(ns), "h1", 2), "vector-bytes", "EMBED.DEDUP.GET");
    expect_nil(hotkv_embed_dedup_get(ctx, KL(ns), "nope", 4), "EMBED.DEDUP.GET miss");
    r = hotkv_embed_dedup_stats(ctx, KL(ns));
    CHECK(fld_int(r, "hits", 1) && fld_int(r, "misses", 1) && fld_int(r, "entries", 1), "EMBED.DEDUP.STATS");
    hotkv_free_reply(r);
    expect_int(hotkv_embed_dedup_release(ctx, KL(ns), "h1", 2), 1, "EMBED.DEDUP.RELEASE (refcount 2 -> 1)");
    expect_int(hotkv_embed_dedup_release(ctx, KL(ns), "h1", 2), 1, "EMBED.DEDUP.RELEASE (1 -> 0)");
    expect_int_at_least(hotkv_embed_dedup_purge(ctx, KL(ns)), 0, "EMBED.DEDUP.PURGE");
    r = hotkv_embed_dedup_list(ctx);
    CHECK(list_has(r, ns), "EMBED.DEDUP.LIST");
    hotkv_free_reply(r);
    expect_int(hotkv_embed_dedup_drop(ctx, KL(ns)), 1, "EMBED.DEDUP.DROP");
}

/* ------------------------------------------------------------------------- */
static void test_amem(void) {
    NAMED(sess, "sess");
    NAMED(agent, "agent");
    float v1[3] = {1, 0, 0}, v2[3] = {0, 1, 0}, v3[3] = {0.9f, 0.1f, 0};
    hotkv_reply_t *r;
    int64_t lim_from = 0;
    uint32_t lim = 1;
    printf("-- AMEM --\n");

    expect_ok(hotkv_amem_conv_create(ctx, KL(sess), 5, 0), "AMEM.CONV.CREATE");
    expect_ok(hotkv_amem_conv_append(ctx, KL(sess), "user", 4, "hi", 2), "AMEM.CONV.APPEND user");
    expect_ok(hotkv_amem_conv_append(ctx, KL(sess), "assistant", 9, "hello", 5), "AMEM.CONV.APPEND assistant");
    r = hotkv_amem_conv_get(ctx, KL(sess));
    CHECK(is_agg(r) && r->nelem == 2 && fld_str(elem(r, 0), "role", "user") && fld_str(elem(r, 1), "content", "hello"),
          "AMEM.CONV.GET returns both messages in order");
    hotkv_free_reply(r);
    r = hotkv_amem_conv_window(ctx, KL(sess), 1);
    CHECK(is_agg(r) && r->nelem == 1 && fld_str(elem(r, 0), "role", "assistant"), "AMEM.CONV.WINDOW 1 is the last message");
    hotkv_free_reply(r);
    expect_int(hotkv_amem_conv_clear(ctx, KL(sess)), 1, "AMEM.CONV.CLEAR");
    r = hotkv_amem_conv_get(ctx, KL(sess));
    CHECK(is_agg(r) && r->nelem == 0, "AMEM.CONV.GET empty after CLEAR");
    hotkv_free_reply(r);
    expect_int(hotkv_amem_conv_drop(ctx, KL(sess)), 1, "AMEM.CONV.DROP");

    expect_ok(hotkv_amem_sem_create(ctx, KL(agent), 3), "AMEM.SEM.CREATE");
    expect_ok(hotkv_amem_sem_store(ctx, KL(agent), "m1", 2, "first memory", 12, v1, 3, 0), "AMEM.SEM.STORE m1");
    expect_ok(hotkv_amem_sem_store(ctx, KL(agent), "m2", 2, "second memory", 13, v2, 3, 0), "AMEM.SEM.STORE m2");
    r = hotkv_amem_sem_recall(ctx, KL(agent), 1, v3, 3);
    CHECK(is_agg(r) && r->nelem == 1 && fld_str(elem(r, 0), "key", "m1") && fld_str(elem(r, 0), "content", "first memory"),
          "AMEM.SEM.RECALL nearest to (0.9,0.1,0) is m1");
    hotkv_free_reply(r);
    r = hotkv_amem_search(ctx, KL(agent), 2, v2, 3);
    CHECK(is_agg(r) && r->nelem == 2 && fld_str(elem(r, 0), "key", "m2") && fld_str(elem(r, 0), "source", "semantic"),
          "AMEM.SEARCH ranks m2 first for (0,1,0)");
    hotkv_free_reply(r);
    expect_strset(hotkv_amem_sem_list(ctx, KL(agent)), 2, "AMEM.SEM.LIST", "m1", "m2");
    expect_int(hotkv_amem_sem_forget(ctx, KL(agent), "m1", 2), 1, "AMEM.SEM.FORGET");
    expect_strs(hotkv_amem_sem_list(ctx, KL(agent)), 1, "AMEM.SEM.LIST after FORGET", "m2");

    expect_ok(hotkv_amem_work_set(ctx, KL(sess), "k", 1, "v", 1, 0), "AMEM.WORK.SET");
    expect_text(hotkv_amem_work_get(ctx, KL(sess), "k", 1), "v", "AMEM.WORK.GET");
    expect_strs(hotkv_amem_work_list(ctx, KL(sess)), 1, "AMEM.WORK.LIST", "k");
    expect_int(hotkv_amem_work_del(ctx, KL(sess), "k", 1), 1, "AMEM.WORK.DEL");
    expect_ok(hotkv_amem_work_set(ctx, KL(sess), "k2", 2, "v2", 2, 0), "AMEM.WORK.SET again");
    expect_int(hotkv_amem_work_clear(ctx, KL(sess)), 1, "AMEM.WORK.CLEAR");

    expect_ok(hotkv_amem_epis_log(ctx, KL(agent), "login", 5, "user logged in", 14, NULL, 0), "AMEM.EPIS.LOG");
    expect_ok(hotkv_amem_epis_log(ctx, KL(agent), "click", 5, "button", 6, "{\"a\":1}", 7), "AMEM.EPIS.LOG with metadata");
    r = hotkv_amem_epis_query(ctx, KL(agent), NULL, 0, NULL, NULL, NULL);
    CHECK(is_agg(r) && r->nelem == 2 && fld_str(elem(r, 0), "type", "login") && fld_str(elem(r, 1), "metadata", "{\"a\":1}"),
          "AMEM.EPIS.QUERY all events");
    hotkv_free_reply(r);
    r = hotkv_amem_epis_query(ctx, KL(agent), "click", 5, NULL, NULL, NULL);
    CHECK(is_agg(r) && r->nelem == 1 && fld_str(elem(r, 0), "type", "click"), "AMEM.EPIS.QUERY TYPE");
    hotkv_free_reply(r);
    r = hotkv_amem_epis_query(ctx, KL(agent), NULL, 0, &lim_from, NULL, &lim);
    CHECK(is_agg(r) && r->nelem == 1, "AMEM.EPIS.QUERY FROM 0 LIMIT 1");
    hotkv_free_reply(r);
    r = hotkv_amem_epis_stats(ctx, KL(agent));
    CHECK(fld_int(r, "total_events", 2), "AMEM.EPIS.STATS total_events");
    hotkv_free_reply(r);
    expect_int(hotkv_amem_epis_clear(ctx, KL(agent)), 1, "AMEM.EPIS.CLEAR");

    expect_int(hotkv_amem_summary_set(ctx, KL(agent), "summary", 7), 1, "AMEM.SUMMARY.SET (version 1)");
    expect_int(hotkv_amem_summary_append(ctx, KL(agent), " more", 5), 2, "AMEM.SUMMARY.APPEND (version 2)");
    r = hotkv_amem_summary_get(ctx, KL(agent));
    CHECK(is_agg(r) && r->nelem == 3 && rtext(elem(r, 0)) && strstr(rtext(elem(r, 0)), "summary") != NULL &&
              strstr(rtext(elem(r, 0)), "more") != NULL && elem(r, 1)->integer == 2,
          "AMEM.SUMMARY.GET [text, version, updated_at]");
    hotkv_free_reply(r);
    r = hotkv_amem_summary_list(ctx);
    CHECK(list_has(r, agent), "AMEM.SUMMARY.LIST");
    hotkv_free_reply(r);
    expect_int(hotkv_amem_summary_drop(ctx, KL(agent)), 1, "AMEM.SUMMARY.DROP");
}

/* ------------------------------------------------------------------------- */
static void test_rag_rate_ts_feature(void) {
    NAMED(pipe, "rag");
    NAMED(rl, "rl");
    NAMED(rl2, "rl2");
    NAMED(ts, "ts");
    NAMED(ts2, "ts-ds");
    NAMED(fg, "fg");
    float v1[3] = {1, 0, 0}, v2[3] = {0, 1, 0};
    const float *vs[] = {v1};
    size_t vl[] = {3};
    hotkv_reply_t *r;
    int64_t t0 = 1700000000000LL, t1 = 1700000001000LL, t2 = 1700000061000LL;
    printf("-- RAG, RATELIMIT, TS, FEATURE --\n");

    expect_ok(hotkv_rag_create(ctx, KL(pipe), 3, NULL, 0, 0), "RAG.CREATE");
    expect_int(hotkv_rag_ingest(ctx, KL(pipe), "doc-1", 5, "Paris is the capital of France. It is on the Seine.", 51,
                                ARR("source"), (size_t[]){6}, ARR("wiki"), (size_t[]){4}, 1, vs, vl, 1),
               1, "RAG.INGEST (1 chunk)");
    r = hotkv_rag_search(ctx, KL(pipe), "capital France", 14, 3, NULL, v1, 3, NULL, NULL, NULL, NULL, 0);
    CHECK(is_agg(r) && r->nelem == 1 && is_agg(elem(r, 0)) && strcmp(rtext(elem(elem(r, 0), 1)), "doc-1") == 0 &&
              strstr(rtext(elem(elem(r, 0), 2)), "capital of France") != NULL,
          "RAG.SEARCH returns [chunk id, doc id, text, ...]");
    hotkv_free_reply(r);
    r = hotkv_rag_search(ctx, KL(pipe), "capital", 7, 3, (double[]){0.5}, v2, 3, ARR("source"), (size_t[]){6},
                         ARR("wiki"), (size_t[]){4}, 1);
    CHECK(is_agg(r) && r->nelem == 1, "RAG.SEARCH with ALPHA and a matching metadata filter");
    hotkv_free_reply(r);
    r = hotkv_rag_search(ctx, KL(pipe), "capital", 7, 3, (double[]){0.5}, v2, 3, ARR("source"), (size_t[]){6},
                         ARR("other"), (size_t[]){5}, 1);
    CHECK(is_agg(r) && r->nelem == 0, "RAG.SEARCH with a non-matching filter returns nothing");
    hotkv_free_reply(r);
    r = hotkv_rag_stats(ctx, KL(pipe));
    CHECK(fld_int(r, "dimensions", 3) && fld_int(r, "totalDocs", 1) && fld_int(r, "totalChunks", 1), "RAG.STATS");
    hotkv_free_reply(r);
    r = hotkv_rag_list(ctx);
    CHECK(list_has(r, pipe), "RAG.LIST");
    hotkv_free_reply(r);
    expect_ok(hotkv_rag_drop(ctx, KL(pipe)), "RAG.DROP");

    expect_ok(hotkv_ratelimit_create(ctx, KL(rl), 3, 60000, NULL, 0.0, 0.0), "RATELIMIT.CREATE sliding window");
    expect_ints(hotkv_ratelimit_check(ctx, KL(rl), "u1", 2, 1), 5, "RATELIMIT.CHECK 1", 1LL, 2LL, 0LL, 3LL, 0LL);
    r = hotkv_ratelimit_check(ctx, KL(rl), "u1", 2, 2);
    {
        long long allowed = -1, remaining = -1;
        CHECK(is_agg(r) && rint64(elem(r, 0), &allowed) && allowed == 1 && rint64(elem(r, 1), &remaining) && remaining == 0,
              "RATELIMIT.CHECK cost 2 uses the rest of the budget");
    }
    hotkv_free_reply(r);
    r = hotkv_ratelimit_check(ctx, KL(rl), "u1", 2, 1);
    {
        long long allowed = -1, retry = -1;
        CHECK(is_agg(r) && rint64(elem(r, 0), &allowed) && allowed == 0 && rint64(elem(r, 2), &retry) && retry > 0,
              "RATELIMIT.CHECK over the limit is denied with a retry delay");
    }
    hotkv_free_reply(r);
    r = hotkv_ratelimit_peek(ctx, KL(rl), "u1", 2);
    CHECK(is_agg(r) && elem(r, 0)->integer == 0, "RATELIMIT.PEEK shows denied");
    hotkv_free_reply(r);
    r = hotkv_ratelimit_stats(ctx, KL(rl));
    CHECK(fld_int(r, "limit", 3) && fld_int(r, "total_allowed", 3) && fld_int(r, "total_denied", 1) &&
              fld_str(r, "algo", "sliding_window"),
          "RATELIMIT.STATS");
    hotkv_free_reply(r);
    expect_ok(hotkv_ratelimit_reset(ctx, KL(rl), "u1", 2), "RATELIMIT.RESET");
    r = hotkv_ratelimit_check(ctx, KL(rl), "u1", 2, 1);
    CHECK(is_agg(r) && elem(r, 0)->integer == 1, "allowed again after RESET");
    hotkv_free_reply(r);
    expect_ok(hotkv_ratelimit_create(ctx, KL(rl2), 10, 1000, "token_bucket", 5.5, 0.0), "RATELIMIT.CREATE token bucket with float refill");
    r = hotkv_ratelimit_list(ctx);
    CHECK(list_has(r, rl) && list_has(r, rl2), "RATELIMIT.LIST");
    hotkv_free_reply(r);
    expect_ok(hotkv_ratelimit_drop(ctx, KL(rl2)), "RATELIMIT.DROP");
    expect_ok(hotkv_ratelimit_drop(ctx, KL(rl)), "RATELIMIT.DROP 1");

    expect_ok(hotkv_ts_create(ctx, KL(ts), 0, ARR("host"), (size_t[]){4}, ARR("a"), (size_t[]){1}, 1), "TS.CREATE with a label");
    expect_ok(hotkv_ts_create(ctx, KL(ts2), 0, NULL, NULL, NULL, NULL, 0), "TS.CREATE destination");
    expect_int(hotkv_ts_add(ctx, KL(ts), &t0, 1.5), t0, "TS.ADD returns the timestamp");
    expect_int(hotkv_ts_add(ctx, KL(ts), &t1, 2.5), t1, "TS.ADD 2");
    expect_int(hotkv_ts_add(ctx, KL(ts), &t2, 4.0), t2, "TS.ADD 3");
    r = hotkv_ts_add(ctx, KL(ts), NULL, 1e-5);
    CHECK(r && r->type == HOTKV_REPLY_INTEGER && r->integer > 1700000061000LL, "TS.ADD without timestamp uses now");
    hotkv_free_reply(r);
    r = hotkv_ts_range(ctx, KL(ts), t0, t2, 3);
    CHECK(is_agg(r) && r->nelem == 3 && is_agg(elem(r, 0)) && elem(elem(r, 0), 0)->integer == t0,
          "TS.RANGE COUNT 3 (first sample at t0)");
    hotkv_free_reply(r);
    r = hotkv_ts_range(ctx, KL(ts), t0, t2, 0);
    {
        double v = 0;
        CHECK(is_agg(r) && r->nelem == 3 && rdouble(elem(elem(r, 1), 1), &v) && v == 2.5, "TS.RANGE value of the second sample");
    }
    hotkv_free_reply(r);
    r = hotkv_ts_agg(ctx, KL(ts), t0, t2, 60000, "avg");
    {
        double v0 = 0, v1v = 0;
        CHECK(is_agg(r) && r->nelem == 2 && rdouble(elem(elem(r, 0), 1), &v0) && v0 == 2.0 && rdouble(elem(elem(r, 1), 1), &v1v) &&
                  v1v == 4.0,
              "TS.AGG avg over 60 s buckets: 2.0 then 4.0");
    }
    hotkv_free_reply(r);
    r = hotkv_ts_info(ctx, KL(ts));
    CHECK(fld_int(r, "totalSamples", 4) && fld_int(r, "firstTimestamp", t0), "TS.INFO");
    hotkv_free_reply(r);
    expect_ok(hotkv_ts_retention(ctx, KL(ts), 86400000ULL), "TS.RETENTION");
    expect_ok(hotkv_ts_downsample(ctx, KL(ts), KL(ts2), "avg", 60000), "TS.DOWNSAMPLE");
    r = hotkv_ts_list(ctx, "host", 4, "a", 1);
    CHECK(list_has(r, ts), "TS.LIST by label");
    hotkv_free_reply(r);
    r = hotkv_ts_list(ctx, NULL, 0, NULL, 0);
    CHECK(list_has(r, ts) && list_has(r, ts2), "TS.LIST all");
    hotkv_free_reply(r);
    expect_ok(hotkv_ts_delete(ctx, KL(ts)), "TS.DELETE");
    expect_ok(hotkv_ts_delete(ctx, KL(ts2)), "TS.DELETE 2");
    expect_err(hotkv_ts_delete(ctx, KL(ts)), "ERR", "TS.DELETE twice reports an error");

    expect_ok(hotkv_feature_create(ctx, KL(fg), "demo", 4, ARR("age", "ltv", "country"), (size_t[]){3, 3, 7},
                                   ARR("int64", "double", "string"), (size_t[]){5, 6, 6}, 3),
              "FEATURE.CREATE");
    expect_ok(hotkv_feature_set(ctx, KL(fg), "e1", 2, ARR("age", "ltv", "country"), (size_t[]){3, 3, 7},
                                ARR("30", "12.5", "DE"), (size_t[]){2, 4, 2}, 3),
              "FEATURE.SET");
    r = hotkv_feature_get(ctx, KL(fg), "e1", 2, NULL, NULL, 0);
    CHECK(is_agg(r) && r->nelem == 3 && fld_int(elem(r, 0), "age", 30) && fld_dbl(elem(r, 0), "ltv", 12.5, 1e-9) &&
              fld_str(elem(r, 0), "country", "DE"),
          "FEATURE.GET all features, typed");
    hotkv_free_reply(r);
    r = hotkv_feature_get(ctx, KL(fg), "e1", 2, ARR("age", "country"), (size_t[]){3, 7}, 2);
    CHECK(is_agg(r) && is_agg(elem(r, 0)) && elem(r, 0)->nelem == 4 && !fld(elem(r, 0), "ltv"),
          "FEATURE.GET only the requested features");
    hotkv_free_reply(r);
    r = hotkv_feature_batch(ctx, KL(fg), ARR("e1", "e2"), (size_t[]){2, 2}, 2, ARR("age"), (size_t[]){3}, 1);
    CHECK(is_agg(r) && r->nelem == 2 && is_agg(elem(r, 0)) && elem(r, 1)->type == HOTKV_REPLY_NIL,
          "FEATURE.BATCH: entity e1 found, e2 nil");
    hotkv_free_reply(r);
    r = hotkv_feature_schema(ctx, KL(fg));
    CHECK(is_agg(r) && r->nelem == 3 && strcmp(rtext(elem(elem(r, 1), 1)), "double") == 0, "FEATURE.SCHEMA");
    hotkv_free_reply(r);
    r = hotkv_feature_stats(ctx, KL(fg));
    CHECK(fld_int(r, "entities", 1), "FEATURE.STATS entities");
    hotkv_free_reply(r);
    r = hotkv_feature_list(ctx);
    CHECK(list_has(r, fg), "FEATURE.LIST");
    hotkv_free_reply(r);
    expect_ok(hotkv_feature_drop(ctx, KL(fg)), "FEATURE.DROP");
}

/* ------------------------------------------------------------------------- */
static void live_enterprise_run(const hotkv_url_t *u, int proto) {
    char tag[16];
    snprintf(tag, sizeof(tag), "ent-r%d", proto);
    live_init_run_id(tag);
    printf("\n===== RESP%d =====\n", proto);
    ctx = live_connect(u, proto);
    if (!ctx) {
        g_failed++;
        return;
    }
    if (!licensed()) {
        skip("enterprise suite", "server is not licensed (NOLICENSE): cannot be verified end to end");
        hotkv_disconnect(ctx);
        ctx = NULL;
        return;
    }
    test_observe();
    test_gcra_vsim();
    test_caches();
    test_toolcache();
    test_ncache();
    test_guardcache();
    test_rerankcache();
    test_llm();
    test_prompt_embed();
    test_amem();
    test_rag_rate_ts_feature();
    hotkv_disconnect(ctx);
    ctx = NULL;
}

#endif /* HKV_LIVE_ENTERPRISE_SUITE_H */
