/**
 * @file hotkv_enterprise.c
 * @brief HotKV exclusive and AI/enterprise command wrappers (GCRA, VSIM,
 * SCACHE, PCACHE, keyed caches, LLMSTATS, LLMBUDGET, PROMPT, EMBED.DEDUP,
 * AMEM, RAG, RATELIMIT, TS, FEATURE, OBSERVE/ALERTS).
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * Every wrapper here builds the exact argument sequence the server parses
 * and returns the raw hotkv_reply_t*, same as
 * the rest of libhotkv. Enterprise features return a NOLICENSE error reply
 * when the server's license does not cover them; hotkv_reply_is_nolicense()
 * detects that case.
 *
 * Sentinel conventions used throughout this file (documented once here to
 * keep the per-function comments short):
 *   - uint32_t/uint64_t "ttl"/"max_entries"/"count" style params: 0 means
 *     "omit the option" (matches the server's own default).
 *   - double params such as cost_per_hit / refill / drain: 0.0 means omit.
 *   - const char* enum-like params (metric, strategy, algo, severity): NULL
 *     or "" means omit (server applies its own default).
 *   - "const T *" for an otherwise-required optional (threshold, alpha,
 *     from_ts/to_ts, error flag, explicit timestamp): NULL means omit.
 */

#include "hotkv.h"
#include "hotkv_private.h"
#include "hotkv_cmd.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* =========================================================================
 * Shared token usage group, USD-per-1M-token price
 * formatting, and by-field-name parsing for STATS/GET/STATUS-like replies.
 * ========================================================================= */

/* Appends usage's wire options to c (MODEL always first); a no-op when
 * usage is NULL. Shared by PCACHE.SET, SCACHE.SET, every keyed cache SET
 * and LLMBUDGET.CONSUME. */
static void hkv_cmd_add_token_usage(hkv_cmd_t *c, const hotkv_token_usage_t *usage) {
    if (!usage) {
        return;
    }
    hkv_cmd_adds(c, "MODEL");
    hkv_cmd_adds(c, usage->model);
    if (usage->input_tokens > 0) {
        hkv_cmd_adds(c, "INPUT");
        hkv_cmd_addf(c, "%" PRIu64, usage->input_tokens);
    }
    if (usage->output_tokens > 0) {
        hkv_cmd_adds(c, "OUTPUT");
        hkv_cmd_addf(c, "%" PRIu64, usage->output_tokens);
    }
    if (usage->cache_read_tokens > 0) {
        hkv_cmd_adds(c, "CACHEREAD");
        hkv_cmd_addf(c, "%" PRIu64, usage->cache_read_tokens);
    }
    if (usage->cache_write_5m_tokens > 0) {
        hkv_cmd_adds(c, "CACHEWRITE5M");
        hkv_cmd_addf(c, "%" PRIu64, usage->cache_write_5m_tokens);
    }
    if (usage->cache_write_1h_tokens > 0) {
        hkv_cmd_adds(c, "CACHEWRITE1H");
        hkv_cmd_addf(c, "%" PRIu64, usage->cache_write_1h_tokens);
    }
}

/* Appends a USD-per-1,000,000-token price as a plain decimal string: never
 * an exponent, at most 6 fractional digits, trailing zeros (and a then
 * bare trailing '.') trimmed. */
static void hkv_cmd_add_usd_per_mtok(hkv_cmd_t *c, double usd_per_mtok) {
    hkv_cmd_add_usd(c, usd_per_mtok);
}

/*
 * Looks up field `name` in a pair-shaped reply (RESP2 flat array of pairs,
 * or RESP3 map: both are represented as a flat 2*N element array, see
 * hotkv_reply_t and hotkv_reply_pair_at()). Returns the value reply, or
 * NULL if reply is not pair-shaped or the field is absent, so every
 * hkv_field_* accessor below defaults missing fields to zero/empty rather
 * than dereferencing NULL.
 */
static const hotkv_reply_t *hkv_field(const hotkv_reply_t *reply, const char *name) {
    size_t i, n;
    if (!reply) {
        return NULL;
    }
    n = hotkv_reply_pair_count(reply);
    for (i = 0; i < n; i++) {
        hotkv_reply_t *k, *v;
        if (hotkv_reply_pair_at(reply, i, &k, &v) && k && k->str && strcmp(k->str, name) == 0) {
            return v;
        }
    }
    return NULL;
}

static int64_t hkv_field_i64(const hotkv_reply_t *reply, const char *name) {
    const hotkv_reply_t *v = hkv_field(reply, name);
    if (!v) {
        return 0;
    }
    if (v->type == HOTKV_REPLY_INTEGER || v->type == HOTKV_REPLY_BOOL) {
        return v->integer;
    }
    if (v->type == HOTKV_REPLY_DOUBLE) {
        return (int64_t)v->dval;
    }
    return (v->str && v->len > 0) ? (int64_t)strtoll(v->str, NULL, 10) : 0;
}

static double hkv_field_f64(const hotkv_reply_t *reply, const char *name) {
    const hotkv_reply_t *v = hkv_field(reply, name);
    if (!v) {
        return 0.0;
    }
    if (v->type == HOTKV_REPLY_DOUBLE) {
        return v->dval;
    }
    if (v->type == HOTKV_REPLY_INTEGER || v->type == HOTKV_REPLY_BOOL) {
        return (double)v->integer;
    }
    return (v->str && v->len > 0) ? strtod(v->str, NULL) : 0.0;
}

/* Never NULL: "" for a missing field, so callers can strcmp()/print it
 * unconditionally. The returned pointer is owned by reply. */
static const char *hkv_field_s(const hotkv_reply_t *reply, const char *name) {
    const hotkv_reply_t *v = hkv_field(reply, name);
    return (v && v->str) ? v->str : "";
}

/* Whether reply is a shape hkv_field() can search: RESP2 flat array of
 * pairs or RESP3 map. Every hotkv_parse_* function below rejects anything
 * else (an ERR reply, NIL, ...) with HOTKV_ERR_OTHER. */
static int hkv_pairshaped(const hotkv_reply_t *reply) {
    return reply != NULL && (reply->type == HOTKV_REPLY_ARRAY || reply->type == HOTKV_REPLY_MAP);
}

static void hkv_parse_usage_savings(const hotkv_reply_t *reply, hotkv_usage_savings_t *out) {
    out->requests = hkv_field_i64(reply, "usage_saved_requests");
    out->input_tokens = hkv_field_i64(reply, "usage_saved_input_tokens");
    out->output_tokens = hkv_field_i64(reply, "usage_saved_output_tokens");
    out->cache_read_tokens = hkv_field_i64(reply, "usage_saved_cache_read_tokens");
    out->cache_write_tokens = hkv_field_i64(reply, "usage_saved_cache_write_tokens");
    out->cost_usd = hkv_field_f64(reply, "usage_saved_cost_usd");
}

/* =========================================================================
 * NOLICENSE detection helper (enterprise features under a legacy license)
 * ========================================================================= */

int hotkv_reply_is_nolicense(const hotkv_reply_t *reply) {
    return hotkv_reply_has_error_prefix(reply, "NOLICENSE");
}

/* =========================================================================
 * Generic typed-error-prefix helpers
 * ========================================================================= */

int hotkv_reply_has_error_prefix(const hotkv_reply_t *reply, const char *prefix) {
    size_t plen;
    if (!reply || reply->type != HOTKV_REPLY_ERROR || !reply->str || !prefix) {
        return 0;
    }
    plen = strlen(prefix);
    if (reply->len < plen) {
        return 0;
    }
    /* Prefix must be a whole word: followed by a space or end of string,
     * matching how the server always writes "PREFIX message...". */
    if (strncmp(reply->str, prefix, plen) != 0) {
        return 0;
    }
    return reply->len == plen || reply->str[plen] == ' ';
}

const char *hotkv_reply_error_prefix(const hotkv_reply_t *reply, char *buf, size_t buflen) {
    const char *sp;
    size_t plen;
    if (!reply || reply->type != HOTKV_REPLY_ERROR || !reply->str || !buf || buflen == 0) {
        return NULL;
    }
    sp = (const char *)memchr(reply->str, ' ', reply->len);
    plen = sp ? (size_t)(sp - reply->str) : reply->len;
    if (plen >= buflen) {
        plen = buflen - 1;
    }
    memcpy(buf, reply->str, plen);
    buf[plen] = '\0';
    return buf;
}

int hotkv_reply_is_moved(const hotkv_reply_t *reply) {
    return hotkv_reply_has_error_prefix(reply, "MOVED");
}

int hotkv_reply_is_ask(const hotkv_reply_t *reply) {
    return hotkv_reply_has_error_prefix(reply, "ASK");
}

int hotkv_reply_is_wrongtype(const hotkv_reply_t *reply) {
    return hotkv_reply_has_error_prefix(reply, "WRONGTYPE");
}

int hotkv_reply_is_noauth(const hotkv_reply_t *reply) {
    return hotkv_reply_has_error_prefix(reply, "NOAUTH");
}

int hotkv_reply_is_readonly(const hotkv_reply_t *reply) {
    return hotkv_reply_has_error_prefix(reply, "READONLY");
}

/* =========================================================================
 * GCRA.*
 * ========================================================================= */

hotkv_reply_t *hotkv_gcra_limit(hotkv_context_t *ctx, const char *key, size_t keylen,
                                 int64_t period_ms, int64_t burst, int64_t count) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "GCRA.LIMIT");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_addf(&c, "%" PRId64, period_ms);
    hkv_cmd_addf(&c, "%" PRId64, burst);
    if (count > 0) {
        hkv_cmd_addf(&c, "%" PRId64, count);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_gcra_reset(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                                 size_t nkeys) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "GCRA.RESET");
    for (i = 0; i < nkeys; i++) {
        hkv_cmd_add(&c, keys[i], keylens[i]);
    }
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * VSIM.* (core index) and VSIM persistence
 * ========================================================================= */

hotkv_reply_t *hotkv_vsim_createindex(hotkv_context_t *ctx, const char *name, size_t namelen,
                                       uint32_t dims, const char *metric, uint32_t m,
                                       uint32_t ef_construction, uint32_t ef_search) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.CREATEINDEX");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_addf(&c, "%" PRIu32, dims);
    if (metric && *metric) {
        hkv_cmd_adds(&c, "METRIC");
        hkv_cmd_adds(&c, metric);
    }
    if (m > 0) {
        hkv_cmd_adds(&c, "M");
        hkv_cmd_addf(&c, "%" PRIu32, m);
    }
    if (ef_construction > 0) {
        hkv_cmd_adds(&c, "EF_CONSTRUCTION");
        hkv_cmd_addf(&c, "%" PRIu32, ef_construction);
    }
    if (ef_search > 0) {
        hkv_cmd_adds(&c, "EF_SEARCH");
        hkv_cmd_addf(&c, "%" PRIu32, ef_search);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_vsim_dropindex(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.DROPINDEX");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_vsim_set(hotkv_context_t *ctx, const char *index, size_t indexlen,
                               const char *key, size_t keylen, const float *vec, size_t dims) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.SET");
    hkv_cmd_add(&c, index, indexlen);
    hkv_cmd_add(&c, key, keylen);
    for (i = 0; i < dims; i++) {
        hkv_cmd_add_float(&c, vec[i]);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_vsim_get(hotkv_context_t *ctx, const char *index, size_t indexlen,
                               const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.GET");
    hkv_cmd_add(&c, index, indexlen);
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_vsim_del(hotkv_context_t *ctx, const char *index, size_t indexlen,
                               const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.DEL");
    hkv_cmd_add(&c, index, indexlen);
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_vsim_search(hotkv_context_t *ctx, const char *index, size_t indexlen,
                                  uint32_t k, const float *vec, size_t dims,
                                  const char **filter_tags, const size_t *filter_taglens,
                                  size_t nfilter, uint32_t ef) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.SEARCH");
    hkv_cmd_add(&c, index, indexlen);
    hkv_cmd_addf(&c, "%" PRIu32, k);
    for (i = 0; i < dims; i++) {
        hkv_cmd_add_float(&c, vec[i]);
    }
    if (nfilter > 0) {
        hkv_cmd_adds(&c, "FILTER");
        for (i = 0; i < nfilter; i++) {
            hkv_cmd_add(&c, filter_tags[i], filter_taglens[i]);
        }
    }
    if (ef > 0) {
        hkv_cmd_adds(&c, "EF");
        hkv_cmd_addf(&c, "%" PRIu32, ef);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_vsim_info(hotkv_context_t *ctx, const char *index, size_t indexlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.INFO");
    if (index && indexlen > 0) {
        hkv_cmd_add(&c, index, indexlen);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_vsim_save(hotkv_context_t *ctx, const char *name, size_t namelen,
                                const char *blob, size_t bloblen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.SAVE");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_add(&c, blob, bloblen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_vsim_load(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.LOAD");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_vsim_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.LIST");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_vsim_drop(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "VSIM.DROP");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * SCACHE.*
 * ========================================================================= */

hotkv_reply_t *hotkv_scache_create(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                    uint32_t dims, const char *metric, const double *threshold,
                                    uint32_t ttl_seconds) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCACHE.CREATE");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_addf(&c, "%" PRIu32, dims);
    if (metric && *metric) {
        hkv_cmd_adds(&c, "METRIC");
        hkv_cmd_adds(&c, metric);
    }
    if (threshold) {
        hkv_cmd_adds(&c, "THRESHOLD");
        hkv_cmd_add_double(&c, *threshold, 1);
    }
    if (ttl_seconds > 0) {
        hkv_cmd_adds(&c, "TTL");
        hkv_cmd_addf(&c, "%" PRIu32, ttl_seconds);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_scache_set(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                 const char *query, size_t querylen, const char *response,
                                 size_t responselen, const float *vec, size_t dims,
                                 uint32_t ttl_seconds, const hotkv_token_usage_t *usage) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCACHE.SET");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, query, querylen);
    hkv_cmd_add(&c, response, responselen);
    for (i = 0; i < dims; i++) {
        hkv_cmd_add_float(&c, vec[i]);
    }
    if (ttl_seconds > 0) {
        hkv_cmd_adds(&c, "TTL");
        hkv_cmd_addf(&c, "%" PRIu32, ttl_seconds);
    }
    hkv_cmd_add_token_usage(&c, usage);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_scache_get(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                 const float *vec, size_t dims, const double *threshold_override) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCACHE.GET");
    hkv_cmd_add(&c, ns, nslen);
    for (i = 0; i < dims; i++) {
        hkv_cmd_add_float(&c, vec[i]);
    }
    if (threshold_override) {
        hkv_cmd_adds(&c, "THRESHOLD");
        hkv_cmd_add_double(&c, *threshold_override, 1);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_scache_stats(hotkv_context_t *ctx, const char *ns, size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCACHE.STATS");
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

int hotkv_parse_scache_stats(const hotkv_reply_t *reply, hotkv_scache_stats_t *out) {
    if (!hkv_pairshaped(reply) || !out) {
        return HOTKV_ERR_OTHER;
    }
    out->entries = hkv_field_i64(reply, "entries");
    out->hits = hkv_field_i64(reply, "hits");
    out->misses = hkv_field_i64(reply, "misses");
    out->hit_rate = hkv_field_f64(reply, "hit_rate");
    out->p50_us = hkv_field_i64(reply, "p50_us");
    hkv_parse_usage_savings(reply, &out->usage);
    return HOTKV_OK;
}

hotkv_reply_t *hotkv_scache_flush(hotkv_context_t *ctx, const char *ns, size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCACHE.FLUSH");
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_scache_drop(hotkv_context_t *ctx, const char *ns, size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCACHE.DROP");
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_scache_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCACHE.LIST");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_scache_threshold(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                       double value) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCACHE.THRESHOLD");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add_double(&c, value, 1);
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * PCACHE.*
 * ========================================================================= */

hotkv_reply_t *hotkv_pcache_create(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                    uint64_t max_entries, uint32_t ttl_seconds,
                                    double cost_per_hit) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PCACHE.CREATE");
    hkv_cmd_add(&c, ns, nslen);
    if (max_entries > 0) {
        hkv_cmd_adds(&c, "MAX_ENTRIES");
        hkv_cmd_addf(&c, "%" PRIu64, max_entries);
    }
    if (ttl_seconds > 0) {
        hkv_cmd_adds(&c, "TTL");
        hkv_cmd_addf(&c, "%" PRIu32, ttl_seconds);
    }
    if (cost_per_hit > 0.0) {
        hkv_cmd_adds(&c, "COST_PER_HIT");
        hkv_cmd_add_double(&c, cost_per_hit, 1);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_pcache_set(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                 const char *prompt, size_t promptlen, const char *response,
                                 size_t responselen, uint32_t ttl_seconds, const char *meta,
                                 size_t metalen, const hotkv_token_usage_t *usage) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PCACHE.SET");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, prompt, promptlen);
    hkv_cmd_add(&c, response, responselen);
    if (ttl_seconds > 0) {
        hkv_cmd_adds(&c, "TTL");
        hkv_cmd_addf(&c, "%" PRIu32, ttl_seconds);
    }
    if (meta && metalen > 0) {
        hkv_cmd_adds(&c, "META");
        hkv_cmd_add(&c, meta, metalen);
    }
    hkv_cmd_add_token_usage(&c, usage);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_pcache_get(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                 const char *prompt, size_t promptlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PCACHE.GET");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, prompt, promptlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_pcache_getex(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                   const char *prompt, size_t promptlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PCACHE.GETEX");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, prompt, promptlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_pcache_del(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                 const char *prompt, size_t promptlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PCACHE.DEL");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, prompt, promptlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_pcache_stats(hotkv_context_t *ctx, const char *ns, size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PCACHE.STATS");
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

int hotkv_parse_pcache_stats(const hotkv_reply_t *reply, hotkv_pcache_stats_t *out) {
    if (!hkv_pairshaped(reply) || !out) {
        return HOTKV_ERR_OTHER;
    }
    out->entries = hkv_field_i64(reply, "entries");
    out->hits = hkv_field_i64(reply, "hits");
    out->misses = hkv_field_i64(reply, "misses");
    out->evictions = hkv_field_i64(reply, "evictions");
    out->memory_bytes = hkv_field_i64(reply, "memory_bytes");
    out->hit_rate = hkv_field_f64(reply, "hit_rate");
    out->p50_ns = hkv_field_i64(reply, "p50_ns");
    out->p95_ns = hkv_field_i64(reply, "p95_ns");
    out->p99_ns = hkv_field_i64(reply, "p99_ns");
    out->cost_saved_usd = hkv_field_f64(reply, "cost_saved_usd");
    hkv_parse_usage_savings(reply, &out->usage);
    return HOTKV_OK;
}

hotkv_reply_t *hotkv_pcache_flush(hotkv_context_t *ctx, const char *ns, size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PCACHE.FLUSH");
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_pcache_drop(hotkv_context_t *ctx, const char *ns, size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PCACHE.DROP");
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_pcache_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PCACHE.LIST");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_pcache_config_set(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                        const char *option, const char *value, size_t valuelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PCACHE.CONFIG");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_adds(&c, "SET");
    hkv_cmd_adds(&c, option);
    hkv_cmd_add(&c, value, valuelen);
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * Keyed caches: TOOL.CACHE.* / NCACHE.* / GUARD.* / RERANK.*
 * One generic implementation per verb, driven by the family's command
 * prefix, plus thin public trampolines for each of the four families.
 * ========================================================================= */

static hotkv_reply_t *kc_create(hotkv_context_t *ctx, const char *prefix, const char *ns,
                                 size_t nslen, uint64_t max_entries, uint32_t ttl) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_addf(&c, "%s.CREATE", prefix);
    hkv_cmd_add(&c, ns, nslen);
    if (max_entries > 0) {
        hkv_cmd_adds(&c, "MAX_ENTRIES");
        hkv_cmd_addf(&c, "%" PRIu64, max_entries);
    }
    if (ttl > 0) {
        hkv_cmd_adds(&c, "TTL");
        hkv_cmd_addf(&c, "%" PRIu32, ttl);
    }
    return hkv_cmd_exec(ctx, &c);
}

static hotkv_reply_t *kc_set(hotkv_context_t *ctx, const char *prefix, const char *ns,
                              size_t nslen, const char *key, size_t keylen, const char *value,
                              size_t valuelen, uint32_t ttl, const char *meta, size_t metalen,
                              const hotkv_token_usage_t *usage) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_addf(&c, "%s.SET", prefix);
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_add(&c, value, valuelen);
    if (ttl > 0) {
        hkv_cmd_adds(&c, "TTL");
        hkv_cmd_addf(&c, "%" PRIu32, ttl);
    }
    if (meta && metalen > 0) {
        hkv_cmd_adds(&c, "META");
        hkv_cmd_add(&c, meta, metalen);
    }
    hkv_cmd_add_token_usage(&c, usage);
    return hkv_cmd_exec(ctx, &c);
}

static hotkv_reply_t *kc_get(hotkv_context_t *ctx, const char *prefix, const char *ns,
                              size_t nslen, const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_addf(&c, "%s.GET", prefix);
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

static hotkv_reply_t *kc_del(hotkv_context_t *ctx, const char *prefix, const char *ns,
                              size_t nslen, const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_addf(&c, "%s.DEL", prefix);
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

static hotkv_reply_t *kc_stats(hotkv_context_t *ctx, const char *prefix, const char *ns,
                                size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_addf(&c, "%s.STATS", prefix);
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

static hotkv_reply_t *kc_flush(hotkv_context_t *ctx, const char *prefix, const char *ns,
                                size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_addf(&c, "%s.FLUSH", prefix);
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

static hotkv_reply_t *kc_drop(hotkv_context_t *ctx, const char *prefix, const char *ns,
                               size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_addf(&c, "%s.DROP", prefix);
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

static hotkv_reply_t *kc_list(hotkv_context_t *ctx, const char *prefix) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_addf(&c, "%s.LIST", prefix);
    return hkv_cmd_exec(ctx, &c);
}

#define HKV_DEFINE_KEYED_CACHE_FAMILY(NAME, PREFIX)                                              \
    hotkv_reply_t *hotkv_##NAME##_create(hotkv_context_t *ctx, const char *ns, size_t nslen,     \
                                          uint64_t max_entries, uint32_t ttl) {                   \
        return kc_create(ctx, PREFIX, ns, nslen, max_entries, ttl);                              \
    }                                                                                            \
    hotkv_reply_t *hotkv_##NAME##_set(hotkv_context_t *ctx, const char *ns, size_t nslen,        \
                                       const char *key, size_t keylen, const char *value,         \
                                       size_t valuelen, uint32_t ttl, const char *meta,           \
                                       size_t metalen, const hotkv_token_usage_t *usage) {        \
        return kc_set(ctx, PREFIX, ns, nslen, key, keylen, value, valuelen, ttl, meta, metalen,  \
                       usage);                                                                    \
    }                                                                                            \
    hotkv_reply_t *hotkv_##NAME##_get(hotkv_context_t *ctx, const char *ns, size_t nslen,        \
                                       const char *key, size_t keylen) {                          \
        return kc_get(ctx, PREFIX, ns, nslen, key, keylen);                                      \
    }                                                                                            \
    hotkv_reply_t *hotkv_##NAME##_del(hotkv_context_t *ctx, const char *ns, size_t nslen,        \
                                       const char *key, size_t keylen) {                          \
        return kc_del(ctx, PREFIX, ns, nslen, key, keylen);                                      \
    }                                                                                            \
    hotkv_reply_t *hotkv_##NAME##_stats(hotkv_context_t *ctx, const char *ns, size_t nslen) {    \
        return kc_stats(ctx, PREFIX, ns, nslen);                                                 \
    }                                                                                            \
    hotkv_reply_t *hotkv_##NAME##_flush(hotkv_context_t *ctx, const char *ns, size_t nslen) {    \
        return kc_flush(ctx, PREFIX, ns, nslen);                                                 \
    }                                                                                            \
    hotkv_reply_t *hotkv_##NAME##_drop(hotkv_context_t *ctx, const char *ns, size_t nslen) {     \
        return kc_drop(ctx, PREFIX, ns, nslen);                                                  \
    }                                                                                            \
    hotkv_reply_t *hotkv_##NAME##_list(hotkv_context_t *ctx) { return kc_list(ctx, PREFIX); }

HKV_DEFINE_KEYED_CACHE_FAMILY(toolcache, "TOOL.CACHE")
HKV_DEFINE_KEYED_CACHE_FAMILY(ncache, "NCACHE")
HKV_DEFINE_KEYED_CACHE_FAMILY(guardcache, "GUARD")
HKV_DEFINE_KEYED_CACHE_FAMILY(rerankcache, "RERANK")

#undef HKV_DEFINE_KEYED_CACHE_FAMILY

int hotkv_parse_keyedcache_stats(const hotkv_reply_t *reply, hotkv_keyedcache_stats_t *out) {
    if (!hkv_pairshaped(reply) || !out) {
        return HOTKV_ERR_OTHER;
    }
    out->entries = hkv_field_i64(reply, "entries");
    out->hits = hkv_field_i64(reply, "hits");
    out->misses = hkv_field_i64(reply, "misses");
    out->evictions = hkv_field_i64(reply, "evictions");
    out->memory_bytes = hkv_field_i64(reply, "memory_bytes");
    hkv_parse_usage_savings(reply, &out->usage);
    return HOTKV_OK;
}

/* =========================================================================
 * LLMSTATS.*
 * ========================================================================= */

hotkv_reply_t *hotkv_llmstats_bucket(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMSTATS.BUCKET");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmstats_record(hotkv_context_t *ctx, const char *bucket, size_t bucketlen,
                                      uint64_t input_tokens, uint64_t output_tokens,
                                      uint64_t cached_tokens, double cost_usd, uint64_t latency_ms,
                                      const int *error_flag, const char *model,
                                      uint64_t cache_write_5m_tokens,
                                      uint64_t cache_write_1h_tokens) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMSTATS.RECORD");
    hkv_cmd_add(&c, bucket, bucketlen);
    hkv_cmd_addf(&c, "%" PRIu64, input_tokens);
    hkv_cmd_addf(&c, "%" PRIu64, output_tokens);
    hkv_cmd_addf(&c, "%" PRIu64, cached_tokens);
    hkv_cmd_add_usd(&c, cost_usd);
    hkv_cmd_addf(&c, "%" PRIu64, latency_ms);
    if (error_flag) {
        hkv_cmd_adds(&c, "ERROR");
        hkv_cmd_adds(&c, *error_flag ? "1" : "0");
    }
    if (model && *model) {
        hkv_cmd_adds(&c, "MODEL");
        hkv_cmd_adds(&c, model);
    }
    if (cache_write_5m_tokens > 0) {
        hkv_cmd_adds(&c, "CACHEWRITE5M");
        hkv_cmd_addf(&c, "%" PRIu64, cache_write_5m_tokens);
    }
    if (cache_write_1h_tokens > 0) {
        hkv_cmd_adds(&c, "CACHEWRITE1H");
        hkv_cmd_addf(&c, "%" PRIu64, cache_write_1h_tokens);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmstats_get(hotkv_context_t *ctx, const char *bucket, size_t bucketlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMSTATS.GET");
    hkv_cmd_add(&c, bucket, bucketlen);
    return hkv_cmd_exec(ctx, &c);
}

int hotkv_parse_llmstats_get(const hotkv_reply_t *reply, hotkv_llmstats_get_t *out) {
    if (!hkv_pairshaped(reply) || !out) {
        return HOTKV_ERR_OTHER;
    }
    out->requests = hkv_field_i64(reply, "requests");
    out->errors = hkv_field_i64(reply, "errors");
    out->input_tokens = hkv_field_i64(reply, "input_tokens");
    out->output_tokens = hkv_field_i64(reply, "output_tokens");
    out->cached_tokens = hkv_field_i64(reply, "cached_tokens");
    out->cost_usd = hkv_field_f64(reply, "cost_usd");
    out->avg_latency_ms = hkv_field_f64(reply, "avg_latency_ms");
    out->requests_1m = hkv_field_i64(reply, "requests_1m");
    out->tokens_1m = hkv_field_i64(reply, "tokens_1m");
    out->cost_usd_1m = hkv_field_f64(reply, "cost_usd_1m");
    out->cache_write_5m_tokens = hkv_field_i64(reply, "cache_write_5m_tokens");
    out->cache_write_1h_tokens = hkv_field_i64(reply, "cache_write_1h_tokens");
    out->unpriced_requests = hkv_field_i64(reply, "unpriced_requests");
    return HOTKV_OK;
}

hotkv_reply_t *hotkv_llmstats_reset(hotkv_context_t *ctx, const char *bucket, size_t bucketlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMSTATS.RESET");
    hkv_cmd_add(&c, bucket, bucketlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmstats_drop(hotkv_context_t *ctx, const char *bucket, size_t bucketlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMSTATS.DROP");
    hkv_cmd_add(&c, bucket, bucketlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmstats_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMSTATS.LIST");
    return hkv_cmd_exec(ctx, &c);
}

/* --- LLMSTATS.PRICE SET|GET|LIST|DEL: runtime-overridable per-model price
 * table, shared by LLMSTATS.RECORD's and LLMBUDGET.CONSUME's MODEL option. */

hotkv_reply_t *hotkv_llmstats_price_set(hotkv_context_t *ctx, const char *model, size_t modellen,
                                         double input_usd_per_mtok, double output_usd_per_mtok,
                                         const double *cache_read_usd_per_mtok,
                                         const double *cache_write_5m_usd_per_mtok,
                                         const double *cache_write_1h_usd_per_mtok) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMSTATS.PRICE");
    hkv_cmd_adds(&c, "SET");
    hkv_cmd_add(&c, model, modellen);
    hkv_cmd_adds(&c, "INPUT");
    hkv_cmd_add_usd_per_mtok(&c, input_usd_per_mtok);
    hkv_cmd_adds(&c, "OUTPUT");
    hkv_cmd_add_usd_per_mtok(&c, output_usd_per_mtok);
    if (cache_read_usd_per_mtok) {
        hkv_cmd_adds(&c, "CACHEREAD");
        hkv_cmd_add_usd_per_mtok(&c, *cache_read_usd_per_mtok);
    }
    if (cache_write_5m_usd_per_mtok) {
        hkv_cmd_adds(&c, "CACHEWRITE5M");
        hkv_cmd_add_usd_per_mtok(&c, *cache_write_5m_usd_per_mtok);
    }
    if (cache_write_1h_usd_per_mtok) {
        hkv_cmd_adds(&c, "CACHEWRITE1H");
        hkv_cmd_add_usd_per_mtok(&c, *cache_write_1h_usd_per_mtok);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmstats_price_get(hotkv_context_t *ctx, const char *model, size_t modellen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMSTATS.PRICE");
    hkv_cmd_adds(&c, "GET");
    hkv_cmd_add(&c, model, modellen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmstats_price_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMSTATS.PRICE");
    hkv_cmd_adds(&c, "LIST");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmstats_price_del(hotkv_context_t *ctx, const char *model, size_t modellen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMSTATS.PRICE");
    hkv_cmd_adds(&c, "DEL");
    hkv_cmd_add(&c, model, modellen);
    return hkv_cmd_exec(ctx, &c);
}

int hotkv_parse_llm_price(const hotkv_reply_t *reply, hotkv_llm_price_t *out) {
    if (!hkv_pairshaped(reply) || !out) {
        return HOTKV_ERR_OTHER;
    }
    out->model = hkv_field_s(reply, "model");
    out->source = hkv_field_s(reply, "source");
    out->priced = (int)hkv_field_i64(reply, "priced");
    out->retired = (int)hkv_field_i64(reply, "retired");
    out->input_usd_per_mtok = hkv_field_f64(reply, "input_usd_per_mtok");
    out->output_usd_per_mtok = hkv_field_f64(reply, "output_usd_per_mtok");
    out->cache_read_usd_per_mtok = hkv_field_f64(reply, "cache_read_usd_per_mtok");
    out->cache_write_5m_usd_per_mtok = hkv_field_f64(reply, "cache_write_5m_usd_per_mtok");
    out->cache_write_1h_usd_per_mtok = hkv_field_f64(reply, "cache_write_1h_usd_per_mtok");
    return HOTKV_OK;
}

/* =========================================================================
 * LLMBUDGET.*
 * ========================================================================= */

hotkv_reply_t *hotkv_llmbudget_set(hotkv_context_t *ctx, const char *name, size_t namelen,
                                    uint64_t token_limit, double cost_limit_usd,
                                    uint32_t window_seconds) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMBUDGET.SET");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_addf(&c, "%" PRIu64, token_limit);
    hkv_cmd_add_usd(&c, cost_limit_usd);
    hkv_cmd_addf(&c, "%" PRIu32, window_seconds);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmbudget_consume(hotkv_context_t *ctx, const char *name, size_t namelen,
                                        uint64_t tokens, double cost_usd,
                                        const hotkv_token_usage_t *usage) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMBUDGET.CONSUME");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_addf(&c, "%" PRIu64, tokens);
    hkv_cmd_add_usd(&c, cost_usd);
    hkv_cmd_add_token_usage(&c, usage);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmbudget_status(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMBUDGET.STATUS");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

int hotkv_parse_llmbudget_status(const hotkv_reply_t *reply, hotkv_llmbudget_status_t *out) {
    if (!hkv_pairshaped(reply) || !out) {
        return HOTKV_ERR_OTHER;
    }
    out->tokens_used = hkv_field_i64(reply, "tokens_used");
    out->token_limit = hkv_field_i64(reply, "token_limit");
    out->cost_used_usd = hkv_field_f64(reply, "cost_used_usd");
    out->cost_limit_usd = hkv_field_f64(reply, "cost_limit_usd");
    out->window_seconds = hkv_field_i64(reply, "window_seconds");
    out->window_remaining = hkv_field_i64(reply, "window_remaining");
    out->requests_allowed = hkv_field_i64(reply, "requests_allowed");
    out->requests_rejected = hkv_field_i64(reply, "requests_rejected");
    out->unpriced_requests = hkv_field_i64(reply, "unpriced_requests");
    return HOTKV_OK;
}

hotkv_reply_t *hotkv_llmbudget_reset(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMBUDGET.RESET");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmbudget_drop(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMBUDGET.DROP");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_llmbudget_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LLMBUDGET.LIST");
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * PROMPT.*
 * ========================================================================= */

hotkv_reply_t *hotkv_prompt_put(hotkv_context_t *ctx, const char *name, size_t namelen,
                                 const char *content, size_t contentlen, const char *meta,
                                 size_t metalen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PROMPT.PUT");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_add(&c, content, contentlen);
    if (meta && metalen > 0) {
        hkv_cmd_adds(&c, "META");
        hkv_cmd_add(&c, meta, metalen);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_prompt_get(hotkv_context_t *ctx, const char *name, size_t namelen,
                                 uint32_t version) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PROMPT.GET");
    hkv_cmd_add(&c, name, namelen);
    if (version > 0) {
        hkv_cmd_addf(&c, "%" PRIu32, version);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_prompt_latest(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PROMPT.LATEST");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_prompt_setactive(hotkv_context_t *ctx, const char *name, size_t namelen,
                                       uint32_t version) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PROMPT.SETACTIVE");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_addf(&c, "%" PRIu32, version);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_prompt_versions(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PROMPT.VERSIONS");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_prompt_drop(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PROMPT.DROP");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_prompt_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PROMPT.LIST");
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * EMBED.DEDUP.*
 * ========================================================================= */

hotkv_reply_t *hotkv_embed_dedup_put(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                      const char *hash, size_t hashlen, const char *model,
                                      size_t modellen, const char *embedding, size_t embeddinglen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "EMBED.DEDUP.PUT");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, hash, hashlen);
    hkv_cmd_add(&c, model, modellen);
    hkv_cmd_add(&c, embedding, embeddinglen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_embed_dedup_get(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                      const char *hash, size_t hashlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "EMBED.DEDUP.GET");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, hash, hashlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_embed_dedup_release(hotkv_context_t *ctx, const char *ns, size_t nslen,
                                          const char *hash, size_t hashlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "EMBED.DEDUP.RELEASE");
    hkv_cmd_add(&c, ns, nslen);
    hkv_cmd_add(&c, hash, hashlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_embed_dedup_stats(hotkv_context_t *ctx, const char *ns, size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "EMBED.DEDUP.STATS");
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_embed_dedup_drop(hotkv_context_t *ctx, const char *ns, size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "EMBED.DEDUP.DROP");
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_embed_dedup_purge(hotkv_context_t *ctx, const char *ns, size_t nslen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "EMBED.DEDUP.PURGE");
    hkv_cmd_add(&c, ns, nslen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_embed_dedup_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "EMBED.DEDUP.LIST");
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * AMEM.* (conversation / semantic / working / episodic / search)
 * ========================================================================= */

hotkv_reply_t *hotkv_amem_conv_create(hotkv_context_t *ctx, const char *session, size_t sessionlen,
                                       uint32_t window, uint32_t ttl) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.CONV.CREATE");
    hkv_cmd_add(&c, session, sessionlen);
    if (window > 0) {
        hkv_cmd_adds(&c, "WINDOW");
        hkv_cmd_addf(&c, "%" PRIu32, window);
    }
    if (ttl > 0) {
        hkv_cmd_adds(&c, "TTL");
        hkv_cmd_addf(&c, "%" PRIu32, ttl);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_conv_append(hotkv_context_t *ctx, const char *session, size_t sessionlen,
                                       const char *role, size_t rolelen, const char *content,
                                       size_t contentlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.CONV.APPEND");
    hkv_cmd_add(&c, session, sessionlen);
    hkv_cmd_add(&c, role, rolelen);
    hkv_cmd_add(&c, content, contentlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_conv_get(hotkv_context_t *ctx, const char *session, size_t sessionlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.CONV.GET");
    hkv_cmd_add(&c, session, sessionlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_conv_window(hotkv_context_t *ctx, const char *session, size_t sessionlen,
                                       uint32_t n) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.CONV.WINDOW");
    hkv_cmd_add(&c, session, sessionlen);
    hkv_cmd_addf(&c, "%" PRIu32, n);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_conv_clear(hotkv_context_t *ctx, const char *session, size_t sessionlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.CONV.CLEAR");
    hkv_cmd_add(&c, session, sessionlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_conv_drop(hotkv_context_t *ctx, const char *session, size_t sessionlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.CONV.DROP");
    hkv_cmd_add(&c, session, sessionlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_sem_create(hotkv_context_t *ctx, const char *agent, size_t agentlen,
                                      uint32_t dims) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SEM.CREATE");
    hkv_cmd_add(&c, agent, agentlen);
    hkv_cmd_addf(&c, "%" PRIu32, dims);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_sem_store(hotkv_context_t *ctx, const char *agent, size_t agentlen,
                                     const char *key, size_t keylen, const char *content,
                                     size_t contentlen, const float *vec, size_t dims,
                                     uint32_t ttl) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SEM.STORE");
    hkv_cmd_add(&c, agent, agentlen);
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_add(&c, content, contentlen);
    for (i = 0; i < dims; i++) {
        hkv_cmd_add_float(&c, vec[i]);
    }
    if (ttl > 0) {
        hkv_cmd_adds(&c, "TTL");
        hkv_cmd_addf(&c, "%" PRIu32, ttl);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_sem_recall(hotkv_context_t *ctx, const char *agent, size_t agentlen,
                                      uint32_t k, const float *vec, size_t dims) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SEM.RECALL");
    hkv_cmd_add(&c, agent, agentlen);
    hkv_cmd_addf(&c, "%" PRIu32, k);
    for (i = 0; i < dims; i++) {
        hkv_cmd_add_float(&c, vec[i]);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_sem_forget(hotkv_context_t *ctx, const char *agent, size_t agentlen,
                                      const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SEM.FORGET");
    hkv_cmd_add(&c, agent, agentlen);
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_sem_list(hotkv_context_t *ctx, const char *agent, size_t agentlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SEM.LIST");
    hkv_cmd_add(&c, agent, agentlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_work_set(hotkv_context_t *ctx, const char *session, size_t sessionlen,
                                    const char *key, size_t keylen, const char *value,
                                    size_t valuelen, uint32_t ttl) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.WORK.SET");
    hkv_cmd_add(&c, session, sessionlen);
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_add(&c, value, valuelen);
    if (ttl > 0) {
        hkv_cmd_adds(&c, "TTL");
        hkv_cmd_addf(&c, "%" PRIu32, ttl);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_work_get(hotkv_context_t *ctx, const char *session, size_t sessionlen,
                                    const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.WORK.GET");
    hkv_cmd_add(&c, session, sessionlen);
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_work_del(hotkv_context_t *ctx, const char *session, size_t sessionlen,
                                    const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.WORK.DEL");
    hkv_cmd_add(&c, session, sessionlen);
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_work_clear(hotkv_context_t *ctx, const char *session, size_t sessionlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.WORK.CLEAR");
    hkv_cmd_add(&c, session, sessionlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_work_list(hotkv_context_t *ctx, const char *session, size_t sessionlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.WORK.LIST");
    hkv_cmd_add(&c, session, sessionlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_epis_log(hotkv_context_t *ctx, const char *agent, size_t agentlen,
                                    const char *event_type, size_t event_typelen,
                                    const char *content, size_t contentlen, const char *metadata,
                                    size_t metadatalen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.EPIS.LOG");
    hkv_cmd_add(&c, agent, agentlen);
    hkv_cmd_add(&c, event_type, event_typelen);
    hkv_cmd_add(&c, content, contentlen);
    if (metadata && metadatalen > 0) {
        hkv_cmd_adds(&c, "METADATA");
        hkv_cmd_add(&c, metadata, metadatalen);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_epis_query(hotkv_context_t *ctx, const char *agent, size_t agentlen,
                                      const char *type, size_t typelen, const int64_t *from_ts,
                                      const int64_t *to_ts, const uint32_t *limit) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.EPIS.QUERY");
    hkv_cmd_add(&c, agent, agentlen);
    if (type && typelen > 0) {
        hkv_cmd_adds(&c, "TYPE");
        hkv_cmd_add(&c, type, typelen);
    }
    if (from_ts) {
        hkv_cmd_adds(&c, "FROM");
        hkv_cmd_addf(&c, "%" PRId64, *from_ts);
    }
    if (to_ts) {
        hkv_cmd_adds(&c, "TO");
        hkv_cmd_addf(&c, "%" PRId64, *to_ts);
    }
    if (limit) {
        hkv_cmd_adds(&c, "LIMIT");
        hkv_cmd_addf(&c, "%" PRIu32, *limit);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_epis_clear(hotkv_context_t *ctx, const char *agent, size_t agentlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.EPIS.CLEAR");
    hkv_cmd_add(&c, agent, agentlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_epis_stats(hotkv_context_t *ctx, const char *agent, size_t agentlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.EPIS.STATS");
    hkv_cmd_add(&c, agent, agentlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_search(hotkv_context_t *ctx, const char *agent, size_t agentlen,
                                  uint32_t k, const float *vec, size_t dims) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SEARCH");
    hkv_cmd_add(&c, agent, agentlen);
    hkv_cmd_addf(&c, "%" PRIu32, k);
    for (i = 0; i < dims; i++) {
        hkv_cmd_add_float(&c, vec[i]);
    }
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * AMEM.SUMMARY.*
 * ========================================================================= */

hotkv_reply_t *hotkv_amem_summary_set(hotkv_context_t *ctx, const char *agent, size_t agentlen,
                                       const char *summary, size_t summarylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SUMMARY.SET");
    hkv_cmd_add(&c, agent, agentlen);
    hkv_cmd_add(&c, summary, summarylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_summary_append(hotkv_context_t *ctx, const char *agent, size_t agentlen,
                                          const char *extra, size_t extralen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SUMMARY.APPEND");
    hkv_cmd_add(&c, agent, agentlen);
    hkv_cmd_add(&c, extra, extralen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_summary_get(hotkv_context_t *ctx, const char *agent, size_t agentlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SUMMARY.GET");
    hkv_cmd_add(&c, agent, agentlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_summary_drop(hotkv_context_t *ctx, const char *agent, size_t agentlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SUMMARY.DROP");
    hkv_cmd_add(&c, agent, agentlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_amem_summary_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "AMEM.SUMMARY.LIST");
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * RAG.*
 * ========================================================================= */

hotkv_reply_t *hotkv_rag_create(hotkv_context_t *ctx, const char *name, size_t namelen,
                                 uint32_t dims, const char *strategy, uint32_t chunk_size,
                                 uint32_t chunk_overlap) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RAG.CREATE");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_addf(&c, "%" PRIu32, dims);
    if (strategy && *strategy) {
        hkv_cmd_adds(&c, "STRATEGY");
        hkv_cmd_adds(&c, strategy);
    }
    if (chunk_size > 0) {
        hkv_cmd_adds(&c, "CHUNK_SIZE");
        hkv_cmd_addf(&c, "%" PRIu32, chunk_size);
    }
    if (chunk_overlap > 0) {
        hkv_cmd_adds(&c, "CHUNK_OVERLAP");
        hkv_cmd_addf(&c, "%" PRIu32, chunk_overlap);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_rag_ingest(hotkv_context_t *ctx, const char *pipeline, size_t pipelinelen,
                                 const char *doc_id, size_t doc_idlen, const char *text,
                                 size_t textlen, const char **meta_keys, const size_t *meta_keylens,
                                 const char **meta_vals, const size_t *meta_vallens, size_t nmeta,
                                 const float *const *vectors, const size_t *vector_lens,
                                 size_t nvectors) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RAG.INGEST");
    hkv_cmd_add(&c, pipeline, pipelinelen);
    hkv_cmd_add(&c, doc_id, doc_idlen);
    hkv_cmd_add(&c, text, textlen);
    if (nmeta > 0) {
        hkv_cmd_adds(&c, "META");
        for (i = 0; i < nmeta; i++) {
            hkv_cmd_add(&c, meta_keys[i], meta_keylens[i]);
            hkv_cmd_add(&c, meta_vals[i], meta_vallens[i]);
        }
    }
    if (nvectors > 0) {
        hkv_cmd_adds(&c, "VECTORS");
        for (i = 0; i < nvectors; i++) {
            hkv_cmd_add_csv_floats(&c, vectors[i], vector_lens[i]);
        }
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_rag_search(hotkv_context_t *ctx, const char *pipeline, size_t pipelinelen,
                                 const char *query_text, size_t query_textlen, uint32_t k,
                                 const double *alpha, const float *vector, size_t vector_len,
                                 const char **filter_keys, const size_t *filter_keylens,
                                 const char **filter_vals, const size_t *filter_vallens,
                                 size_t nfilter) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RAG.SEARCH");
    hkv_cmd_add(&c, pipeline, pipelinelen);
    hkv_cmd_add(&c, query_text, query_textlen);
    if (k > 0) {
        hkv_cmd_adds(&c, "K");
        hkv_cmd_addf(&c, "%" PRIu32, k);
    }
    if (alpha) {
        hkv_cmd_adds(&c, "ALPHA");
        hkv_cmd_add_double(&c, *alpha, 1);
    }
    if (vector && vector_len > 0) {
        hkv_cmd_adds(&c, "VECTOR");
        hkv_cmd_add_csv_floats(&c, vector, vector_len);
    }
    if (nfilter > 0) {
        hkv_cmd_adds(&c, "FILTER");
        for (i = 0; i < nfilter; i++) {
            hkv_cmd_add(&c, filter_keys[i], filter_keylens[i]);
            hkv_cmd_add(&c, filter_vals[i], filter_vallens[i]);
        }
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_rag_stats(hotkv_context_t *ctx, const char *pipeline, size_t pipelinelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RAG.STATS");
    hkv_cmd_add(&c, pipeline, pipelinelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_rag_drop(hotkv_context_t *ctx, const char *pipeline, size_t pipelinelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RAG.DROP");
    hkv_cmd_add(&c, pipeline, pipelinelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_rag_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RAG.LIST");
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * RATELIMIT.*
 * ========================================================================= */

hotkv_reply_t *hotkv_ratelimit_create(hotkv_context_t *ctx, const char *name, size_t namelen,
                                       uint64_t limit, uint64_t window_ms, const char *algo,
                                       double refill_rate, double drain_rate) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RATELIMIT.CREATE");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_addf(&c, "%" PRIu64, limit);
    hkv_cmd_addf(&c, "%" PRIu64, window_ms);
    if (algo && *algo) {
        hkv_cmd_adds(&c, "ALGO");
        hkv_cmd_adds(&c, algo);
    }
    if (refill_rate > 0.0) {
        hkv_cmd_adds(&c, "REFILL");
        hkv_cmd_add_double(&c, refill_rate, 1);
    }
    if (drain_rate > 0.0) {
        hkv_cmd_adds(&c, "DRAIN");
        hkv_cmd_add_double(&c, drain_rate, 1);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ratelimit_check(hotkv_context_t *ctx, const char *name, size_t namelen,
                                      const char *key, size_t keylen, uint64_t cost) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RATELIMIT.CHECK");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_add(&c, key, keylen);
    if (cost > 0) {
        hkv_cmd_adds(&c, "COST");
        hkv_cmd_addf(&c, "%" PRIu64, cost);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ratelimit_peek(hotkv_context_t *ctx, const char *name, size_t namelen,
                                     const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RATELIMIT.PEEK");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ratelimit_reset(hotkv_context_t *ctx, const char *name, size_t namelen,
                                      const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RATELIMIT.RESET");
    hkv_cmd_add(&c, name, namelen);
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ratelimit_stats(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RATELIMIT.STATS");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ratelimit_drop(hotkv_context_t *ctx, const char *name, size_t namelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RATELIMIT.DROP");
    hkv_cmd_add(&c, name, namelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ratelimit_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RATELIMIT.LIST");
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * TS.*
 * ========================================================================= */

hotkv_reply_t *hotkv_ts_create(hotkv_context_t *ctx, const char *key, size_t keylen,
                                uint64_t retention_ms, const char **label_keys,
                                const size_t *label_keylens, const char **label_vals,
                                const size_t *label_vallens, size_t nlabels) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "TS.CREATE");
    hkv_cmd_add(&c, key, keylen);
    if (retention_ms > 0) {
        hkv_cmd_adds(&c, "RETENTION");
        hkv_cmd_addf(&c, "%" PRIu64, retention_ms);
    }
    if (nlabels > 0) {
        hkv_cmd_adds(&c, "LABELS");
        for (i = 0; i < nlabels; i++) {
            hkv_cmd_add(&c, label_keys[i], label_keylens[i]);
            hkv_cmd_add(&c, label_vals[i], label_vallens[i]);
        }
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ts_add(hotkv_context_t *ctx, const char *key, size_t keylen,
                             const int64_t *timestamp_ms, double value) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "TS.ADD");
    hkv_cmd_add(&c, key, keylen);
    if (timestamp_ms) {
        hkv_cmd_addf(&c, "%" PRId64, *timestamp_ms);
    } else {
        hkv_cmd_adds(&c, "*");
    }
    hkv_cmd_add_double(&c, value, 1);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ts_range(hotkv_context_t *ctx, const char *key, size_t keylen, int64_t from_ms,
                               int64_t to_ms, uint64_t count) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "TS.RANGE");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_addf(&c, "%" PRId64, from_ms);
    hkv_cmd_addf(&c, "%" PRId64, to_ms);
    if (count > 0) {
        hkv_cmd_adds(&c, "COUNT");
        hkv_cmd_addf(&c, "%" PRIu64, count);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ts_agg(hotkv_context_t *ctx, const char *key, size_t keylen, int64_t from_ms,
                             int64_t to_ms, uint64_t bucket_ms, const char *agg_type) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "TS.AGG");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_addf(&c, "%" PRId64, from_ms);
    hkv_cmd_addf(&c, "%" PRId64, to_ms);
    hkv_cmd_addf(&c, "%" PRIu64, bucket_ms);
    hkv_cmd_adds(&c, agg_type);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ts_info(hotkv_context_t *ctx, const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "TS.INFO");
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ts_delete(hotkv_context_t *ctx, const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "TS.DELETE");
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ts_retention(hotkv_context_t *ctx, const char *key, size_t keylen,
                                   uint64_t retention_ms) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "TS.RETENTION");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_addf(&c, "%" PRIu64, retention_ms);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ts_downsample(hotkv_context_t *ctx, const char *src, size_t srclen,
                                    const char *dest, size_t destlen, const char *agg_type,
                                    uint64_t bucket_ms) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "TS.DOWNSAMPLE");
    hkv_cmd_add(&c, src, srclen);
    hkv_cmd_add(&c, dest, destlen);
    hkv_cmd_adds(&c, agg_type);
    hkv_cmd_addf(&c, "%" PRIu64, bucket_ms);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_ts_list(hotkv_context_t *ctx, const char *label_key, size_t label_keylen,
                              const char *label_val, size_t label_vallen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "TS.LIST");
    if (label_key && label_keylen > 0) {
        hkv_cmd_adds(&c, "LABEL");
        hkv_cmd_add(&c, label_key, label_keylen);
        hkv_cmd_add(&c, label_val, label_vallen);
    }
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * FEATURE.*
 * ========================================================================= */

hotkv_reply_t *hotkv_feature_create(hotkv_context_t *ctx, const char *group, size_t grouplen,
                                     const char *description, size_t descriptionlen,
                                     const char **names, const size_t *namelens,
                                     const char **types, const size_t *typelens,
                                     size_t nfeatures) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "FEATURE.CREATE");
    hkv_cmd_add(&c, group, grouplen);
    hkv_cmd_add(&c, description, descriptionlen);
    hkv_cmd_adds(&c, "SCHEMA");
    for (i = 0; i < nfeatures; i++) {
        hkv_cmd_add(&c, names[i], namelens[i]);
        hkv_cmd_add(&c, types[i], typelens[i]);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_feature_set(hotkv_context_t *ctx, const char *group, size_t grouplen,
                                  const char *entity, size_t entitylen, const char **names,
                                  const size_t *namelens, const char **values,
                                  const size_t *valuelens, size_t nfeatures) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "FEATURE.SET");
    hkv_cmd_add(&c, group, grouplen);
    hkv_cmd_add(&c, entity, entitylen);
    for (i = 0; i < nfeatures; i++) {
        hkv_cmd_add(&c, names[i], namelens[i]);
        hkv_cmd_add(&c, values[i], valuelens[i]);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_feature_get(hotkv_context_t *ctx, const char *group, size_t grouplen,
                                  const char *entity, size_t entitylen, const char **names,
                                  const size_t *namelens, size_t nnames) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "FEATURE.GET");
    hkv_cmd_add(&c, group, grouplen);
    hkv_cmd_add(&c, entity, entitylen);
    for (i = 0; i < nnames; i++) {
        hkv_cmd_add(&c, names[i], namelens[i]);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_feature_batch(hotkv_context_t *ctx, const char *group, size_t grouplen,
                                    const char **entities, const size_t *entitylens,
                                    size_t nentities, const char **names, const size_t *namelens,
                                    size_t nnames) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "FEATURE.BATCH");
    hkv_cmd_add(&c, group, grouplen);
    for (i = 0; i < nentities; i++) {
        hkv_cmd_add(&c, entities[i], entitylens[i]);
    }
    if (nnames > 0) {
        hkv_cmd_adds(&c, "FEATURES");
        for (i = 0; i < nnames; i++) {
            hkv_cmd_add(&c, names[i], namelens[i]);
        }
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_feature_schema(hotkv_context_t *ctx, const char *group, size_t grouplen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "FEATURE.SCHEMA");
    hkv_cmd_add(&c, group, grouplen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_feature_list(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "FEATURE.LIST");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_feature_drop(hotkv_context_t *ctx, const char *group, size_t grouplen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "FEATURE.DROP");
    hkv_cmd_add(&c, group, grouplen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_feature_stats(hotkv_context_t *ctx, const char *group, size_t grouplen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "FEATURE.STATS");
    hkv_cmd_add(&c, group, grouplen);
    return hkv_cmd_exec(ctx, &c);
}

/* =========================================================================
 * OBSERVE / ALERTS
 * ========================================================================= */

hotkv_reply_t *hotkv_observe_info(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "OBSERVE");
    hkv_cmd_adds(&c, "INFO");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_observe_metrics(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "OBSERVE");
    hkv_cmd_adds(&c, "METRICS");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_alerts_info(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "ALERTS");
    hkv_cmd_adds(&c, "INFO");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_alerts_test(hotkv_context_t *ctx, const char *severity, const char *message,
                                  size_t messagelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "ALERTS");
    hkv_cmd_adds(&c, "TEST");
    if (severity && *severity) {
        hkv_cmd_adds(&c, severity);
        if (message && messagelen > 0) {
            hkv_cmd_add(&c, message, messagelen);
        }
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_alerts_seturl(hotkv_context_t *ctx, const char *url, size_t urllen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "ALERTS");
    hkv_cmd_adds(&c, "SETURL");
    hkv_cmd_add(&c, url, urllen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_alerts_setseverity(hotkv_context_t *ctx, const char *severity) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "ALERTS");
    hkv_cmd_adds(&c, "SETSEVERITY");
    hkv_cmd_adds(&c, severity);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_alerts_set_enabled(hotkv_context_t *ctx, int enabled) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "ALERTS");
    hkv_cmd_adds(&c, enabled ? "ON" : "OFF");
    return hkv_cmd_exec(ctx, &c);
}
