/**
 * @file hotkv_ai.c
 * @brief AI helpers: PromptCache key builder and
 * get-or-call wrapper, and the SemanticCache get-or-call wrapper. Includes
 * a small self-contained SHA-256 (public-domain algorithm) so this stays
 * dependency-free like the rest of libhotkv's core build.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "hotkv.h"
#include "hotkv_private.h"

#include <stdio.h>
#include <stdlib.h>

/* =========================================================================
 * SHA-256 (public-domain reference algorithm; FIPS 180-4). File-local only.
 * ========================================================================= */

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} hkv_sha256_ctx_t;

#define HKV_ROTR(a, b) (((a) >> (b)) | ((a) << (32u - (b))))
#define HKV_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define HKV_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define HKV_EP0(x) (HKV_ROTR(x, 2) ^ HKV_ROTR(x, 13) ^ HKV_ROTR(x, 22))
#define HKV_EP1(x) (HKV_ROTR(x, 6) ^ HKV_ROTR(x, 11) ^ HKV_ROTR(x, 25))
#define HKV_SIG0(x) (HKV_ROTR(x, 7) ^ HKV_ROTR(x, 18) ^ ((x) >> 3))
#define HKV_SIG1(x) (HKV_ROTR(x, 17) ^ HKV_ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t hkv_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static void hkv_sha256_transform(hkv_sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    size_t i, j;

    for (i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | (uint32_t)data[j + 3];
    }
    for (; i < 64; ++i) {
        m[i] = HKV_SIG1(m[i - 2]) + m[i - 7] + HKV_SIG0(m[i - 15]) + m[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + HKV_EP1(e) + HKV_CH(e, f, g) + hkv_sha256_k[i] + m[i];
        t2 = HKV_EP0(a) + HKV_MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void hkv_sha256_init(hkv_sha256_ctx_t *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void hkv_sha256_update(hkv_sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            hkv_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void hkv_sha256_final(hkv_sha256_ctx_t *ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    size_t j;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) {
            ctx->data[i++] = 0x00;
        }
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) {
            ctx->data[i++] = 0x00;
        }
        hkv_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    hkv_sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 8; ++j) {
            hash[i + j * 4] = (uint8_t)((ctx->state[j] >> (24u - i * 8u)) & 0xffu);
        }
    }
}

static void hkv_sha256_hex(const char *data, size_t len, char out[65]) {
    static const char hexd[] = "0123456789abcdef";
    hkv_sha256_ctx_t ctx;
    uint8_t hash[32];
    size_t i;

    hkv_sha256_init(&ctx);
    hkv_sha256_update(&ctx, (const uint8_t *)data, len);
    hkv_sha256_final(&ctx, hash);

    for (i = 0; i < 32; i++) {
        out[i * 2] = hexd[(hash[i] >> 4) & 0xf];
        out[i * 2 + 1] = hexd[hash[i] & 0xf];
    }
    out[64] = '\0';
}

/* =========================================================================
 * Canonical JSON encoding for the PromptCache key.
 * ========================================================================= */

static int hkv_buf_append_json_string(hotkv_buf_t *b, const char *s) {
    static const char hexd[] = "0123456789abcdef";
    char esc[6];
    size_t i;

    if (hotkv_buf_append(b, "\"", 1) != HOTKV_OK) {
        return HOTKV_ERR_OOM;
    }
    if (!s) {
        return hotkv_buf_append(b, "\"", 1);
    }
    for (i = 0; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':
                if (hotkv_buf_append(b, "\\\"", 2) != HOTKV_OK) return HOTKV_ERR_OOM;
                break;
            case '\\':
                if (hotkv_buf_append(b, "\\\\", 2) != HOTKV_OK) return HOTKV_ERR_OOM;
                break;
            case '\n':
                if (hotkv_buf_append(b, "\\n", 2) != HOTKV_OK) return HOTKV_ERR_OOM;
                break;
            case '\r':
                if (hotkv_buf_append(b, "\\r", 2) != HOTKV_OK) return HOTKV_ERR_OOM;
                break;
            case '\t':
                if (hotkv_buf_append(b, "\\t", 2) != HOTKV_OK) return HOTKV_ERR_OOM;
                break;
            case '\b':
                if (hotkv_buf_append(b, "\\b", 2) != HOTKV_OK) return HOTKV_ERR_OOM;
                break;
            case '\f':
                if (hotkv_buf_append(b, "\\f", 2) != HOTKV_OK) return HOTKV_ERR_OOM;
                break;
            default:
                if (c < 0x20) {
                    esc[0] = '\\';
                    esc[1] = 'u';
                    esc[2] = '0';
                    esc[3] = '0';
                    esc[4] = hexd[(c >> 4) & 0xf];
                    esc[5] = hexd[c & 0xf];
                    if (hotkv_buf_append(b, esc, 6) != HOTKV_OK) return HOTKV_ERR_OOM;
                } else {
                    /* Non-ASCII bytes (UTF-8 continuation/lead bytes) pass
                     * through unchanged: they are always >= 0x80, outside
                     * every case above. */
                    if (hotkv_buf_append(b, (const char *)&c, 1) != HOTKV_OK) return HOTKV_ERR_OOM;
                }
        }
    }
    return hotkv_buf_append(b, "\"", 1);
}

typedef struct {
    const char *key;
    const char *val;
} hkv_kv_t;

static int hkv_kv_cmp(const void *a, const void *b) {
    return strcmp(((const hkv_kv_t *)a)->key, ((const hkv_kv_t *)b)->key);
}

char *hotkv_promptcache_key(const char *provider, const char *model, const char *system,
                             const char **msg_roles, const char **msg_contents, size_t nmessages,
                             const char **param_keys, const char **param_values, size_t nparams) {
    hotkv_buf_t buf;
    char digest[65];
    char *result;
    size_t i;
    hkv_kv_t *sorted = NULL;

    if (hotkv_buf_init(&buf, 512) != HOTKV_OK) {
        return NULL;
    }

#define HKV_APPEND_LIT(lit)                                                    \
    do {                                                                       \
        if (hotkv_buf_append(&buf, (lit), sizeof(lit) - 1) != HOTKV_OK) {      \
            hotkv_buf_destroy(&buf);                                          \
            hotkv_free(sorted);                                               \
            return NULL;                                                      \
        }                                                                      \
    } while (0)
#define HKV_APPEND_STR(s)                                                      \
    do {                                                                       \
        if (hkv_buf_append_json_string(&buf, (s)) != HOTKV_OK) {               \
            hotkv_buf_destroy(&buf);                                          \
            hotkv_free(sorted);                                               \
            return NULL;                                                      \
        }                                                                      \
    } while (0)

    HKV_APPEND_LIT("{\"provider\":");
    HKV_APPEND_STR(provider);
    HKV_APPEND_LIT(",\"model\":");
    HKV_APPEND_STR(model);
    HKV_APPEND_LIT(",\"system\":");
    HKV_APPEND_STR(system);
    HKV_APPEND_LIT(",\"messages\":[");
    for (i = 0; i < nmessages; i++) {
        if (i > 0) {
            HKV_APPEND_LIT(",");
        }
        HKV_APPEND_LIT("{\"role\":");
        HKV_APPEND_STR(msg_roles[i]);
        HKV_APPEND_LIT(",\"content\":");
        HKV_APPEND_STR(msg_contents[i]);
        HKV_APPEND_LIT("}");
    }
    HKV_APPEND_LIT("],\"params\":{");
    if (nparams > 0) {
        sorted = (hkv_kv_t *)hotkv_malloc(nparams * sizeof(hkv_kv_t));
        if (!sorted) {
            hotkv_buf_destroy(&buf);
            return NULL;
        }
        for (i = 0; i < nparams; i++) {
            sorted[i].key = param_keys[i];
            sorted[i].val = param_values[i];
        }
        qsort(sorted, nparams, sizeof(hkv_kv_t), hkv_kv_cmp);
        for (i = 0; i < nparams; i++) {
            if (i > 0) {
                HKV_APPEND_LIT(",");
            }
            HKV_APPEND_STR(sorted[i].key);
            HKV_APPEND_LIT(":");
            HKV_APPEND_STR(sorted[i].val);
        }
        hotkv_free(sorted);
        sorted = NULL;
    }
    HKV_APPEND_LIT("}}");

#undef HKV_APPEND_LIT
#undef HKV_APPEND_STR

    hkv_sha256_hex(buf.data, buf.len, digest);
    hotkv_buf_destroy(&buf);

    result = hotkv_strdup(digest);
    return result;
}

/* =========================================================================
 * PromptCache: get-or-call wrapper around PCACHE.* (needs PromptCache
 * license) plus optional LLMSTATS.RECORD accounting.
 * ========================================================================= */

int hotkv_promptcache_init(hotkv_promptcache_t *pc, hotkv_context_t *ctx, const char *ns,
                            uint32_t ttl_seconds) {
    if (!pc || !ctx || !ns) {
        return HOTKV_ERR_OTHER;
    }
    pc->ctx = ctx;
    pc->ns = hotkv_strdup(ns);
    pc->ttl_seconds = ttl_seconds;
    pc->stats_bucket = NULL;
    return pc->ns ? HOTKV_OK : HOTKV_ERR_OOM;
}

void hotkv_promptcache_set_stats_bucket(hotkv_promptcache_t *pc, const char *bucket) {
    if (!pc) {
        return;
    }
    hotkv_free(pc->stats_bucket);
    pc->stats_bucket = bucket ? hotkv_strdup(bucket) : NULL;
}

void hotkv_promptcache_destroy(hotkv_promptcache_t *pc) {
    if (!pc) {
        return;
    }
    hotkv_free(pc->ns);
    hotkv_free(pc->stats_bucket);
    pc->ns = NULL;
    pc->stats_bucket = NULL;
}

int hotkv_promptcache_create_namespace(hotkv_promptcache_t *pc, uint64_t max_entries,
                                        double cost_per_hit) {
    hotkv_reply_t *r;
    int ok;
    if (!pc) {
        return HOTKV_ERR_OTHER;
    }
    r = hotkv_pcache_create(pc->ctx, pc->ns, strlen(pc->ns), max_entries, pc->ttl_seconds,
                            cost_per_hit);
    if (!r) {
        return hotkv_context_err(pc->ctx);
    }
    ok = (r->type != HOTKV_REPLY_ERROR);
    hotkv_free_reply(r);
    return ok ? HOTKV_OK : HOTKV_ERR_OTHER;
}

static void hkv_record_stats(hotkv_promptcache_t *pc, const hotkv_llm_usage_t *usage, int hit) {
    uint64_t input_tokens, output_tokens, cached_tokens, cw5m, cw1h;
    double cost_usd;
    const char *model = NULL;

    if (!pc->stats_bucket) {
        return;
    }
    cw5m = 0;
    cw1h = 0;
    if (hit) {
        input_tokens = 0;
        output_tokens = 0;
        cached_tokens = usage ? usage->input_tokens + usage->output_tokens : 0;
        cost_usd = 0.0;
    } else {
        input_tokens = usage ? usage->input_tokens : 0;
        output_tokens = usage ? usage->output_tokens : 0;
        cached_tokens = 0;
        cost_usd = usage ? usage->cost_usd : 0.0;
        if (usage && usage->model && *usage->model) {
            model = usage->model;
            cw5m = usage->cache_write_5m_tokens;
            cw1h = usage->cache_write_1h_tokens;
        }
    }
    {
        hotkv_reply_t *r =
            hotkv_llmstats_record(pc->ctx, pc->stats_bucket, strlen(pc->stats_bucket),
                                  input_tokens, output_tokens, cached_tokens, cost_usd,
                                  usage ? usage->latency_ms : 0, NULL, model, cw5m, cw1h);
        hotkv_free_reply(r);
    }
}

/* Builds the PCACHE.SET/SCACHE.SET usage group from an hotkv_llm_usage_t,
 * or returns NULL when usage has no model (a usage group always requires
 * one). *out_group is only valid as long as usage outlives it (it just
 * borrows usage's pointers/values, nothing is copied). */
static const hotkv_token_usage_t *hkv_llm_usage_as_token_usage(const hotkv_llm_usage_t *usage,
                                                                hotkv_token_usage_t *out_group) {
    if (!usage || !usage->model || !*usage->model) {
        return NULL;
    }
    out_group->model = usage->model;
    out_group->input_tokens = usage->input_tokens;
    out_group->output_tokens = usage->output_tokens;
    out_group->cache_read_tokens = usage->cache_read_tokens;
    out_group->cache_write_5m_tokens = usage->cache_write_5m_tokens;
    out_group->cache_write_1h_tokens = usage->cache_write_1h_tokens;
    return out_group;
}

int hotkv_promptcache_get_or_call(hotkv_promptcache_t *pc, const char *provider,
                                   const char *model, const char *system,
                                   const char **msg_roles, const char **msg_contents,
                                   size_t nmessages, const char **param_keys,
                                   const char **param_values, size_t nparams,
                                   hotkv_llm_call_fn fn, void *userdata, char **out_response,
                                   int *out_hit) {
    char *key;
    hotkv_reply_t *get_reply;
    int rc;

    if (!pc || !fn || !out_response) {
        return HOTKV_ERR_OTHER;
    }
    *out_response = NULL;
    if (out_hit) {
        *out_hit = 0;
    }

    key = hotkv_promptcache_key(provider, model, system, msg_roles, msg_contents, nmessages,
                                param_keys, param_values, nparams);
    if (!key) {
        return HOTKV_ERR_OOM;
    }

    get_reply = hotkv_pcache_get(pc->ctx, pc->ns, strlen(pc->ns), key, strlen(key));
    if (get_reply && get_reply->type == HOTKV_REPLY_BULK) {
        *out_response = hotkv_strdup(get_reply->str ? get_reply->str : "");
        if (out_hit) {
            *out_hit = 1;
        }
        hotkv_free_reply(get_reply);
        hotkv_free(key);
        hkv_record_stats(pc, NULL, 1);
        return *out_response ? HOTKV_OK : HOTKV_ERR_OOM;
    }
    hotkv_free_reply(get_reply);

    {
        char *response = NULL;
        hotkv_llm_usage_t usage;
        memset(&usage, 0, sizeof(usage));

        rc = fn(userdata, &response, &usage);
        if (rc != HOTKV_OK || !response) {
            hotkv_free(key);
            hotkv_free(response);
            return rc != HOTKV_OK ? rc : HOTKV_ERR_OTHER;
        }

        {
            hotkv_token_usage_t group;
            hotkv_reply_t *set_reply =
                hotkv_pcache_set(pc->ctx, pc->ns, strlen(pc->ns), key, strlen(key), response,
                                 strlen(response), pc->ttl_seconds, NULL, 0,
                                 hkv_llm_usage_as_token_usage(&usage, &group));
            hotkv_free_reply(set_reply);
        }
        hkv_record_stats(pc, &usage, 0);

        hotkv_free(key);
        *out_response = response;
        return HOTKV_OK;
    }
}

int hotkv_promptcache_stats(hotkv_promptcache_t *pc, hotkv_pcache_stats_t *out) {
    hotkv_reply_t *r;
    int rc;
    if (!pc || !out) {
        return HOTKV_ERR_OTHER;
    }
    r = hotkv_pcache_stats(pc->ctx, pc->ns, strlen(pc->ns));
    if (!r) {
        return hotkv_context_err(pc->ctx);
    }
    rc = hotkv_parse_pcache_stats(r, out);
    hotkv_free_reply(r);
    return rc;
}

/* =========================================================================
 * SemanticCache: get-or-call wrapper around SCACHE.* (needs SemanticCache
 * license). Embeddings are always supplied by the caller.
 * ========================================================================= */

int hotkv_semanticcache_init(hotkv_semanticcache_t *sc, hotkv_context_t *ctx, const char *ns,
                              uint32_t ttl_seconds) {
    if (!sc || !ctx || !ns) {
        return HOTKV_ERR_OTHER;
    }
    sc->ctx = ctx;
    sc->ns = hotkv_strdup(ns);
    sc->ttl_seconds = ttl_seconds;
    return sc->ns ? HOTKV_OK : HOTKV_ERR_OOM;
}

void hotkv_semanticcache_destroy(hotkv_semanticcache_t *sc) {
    if (!sc) {
        return;
    }
    hotkv_free(sc->ns);
    sc->ns = NULL;
}

int hotkv_semanticcache_create_namespace(hotkv_semanticcache_t *sc, uint32_t dims,
                                          const char *metric, const double *threshold) {
    hotkv_reply_t *r;
    int ok;
    if (!sc) {
        return HOTKV_ERR_OTHER;
    }
    r = hotkv_scache_create(sc->ctx, sc->ns, strlen(sc->ns), dims, metric, threshold,
                            sc->ttl_seconds);
    if (!r) {
        return hotkv_context_err(sc->ctx);
    }
    ok = (r->type != HOTKV_REPLY_ERROR);
    hotkv_free_reply(r);
    return ok ? HOTKV_OK : HOTKV_ERR_OTHER;
}

int hotkv_semanticcache_get_or_call(hotkv_semanticcache_t *sc, const char *query, size_t querylen,
                                     const float *vector, size_t dims, hotkv_text_call_fn fn,
                                     void *userdata, char **out_response, int *out_hit) {
    hotkv_reply_t *get_reply;
    int rc;

    if (!sc || !fn || !out_response) {
        return HOTKV_ERR_OTHER;
    }
    *out_response = NULL;
    if (out_hit) {
        *out_hit = 0;
    }

    get_reply = hotkv_scache_get(sc->ctx, sc->ns, strlen(sc->ns), vector, dims, NULL);
    if (get_reply && get_reply->type == HOTKV_REPLY_ARRAY && get_reply->nelem >= 1 &&
        get_reply->elements[0]->str) {
        *out_response = hotkv_strdup(get_reply->elements[0]->str);
        if (out_hit) {
            *out_hit = 1;
        }
        hotkv_free_reply(get_reply);
        return *out_response ? HOTKV_OK : HOTKV_ERR_OOM;
    }
    hotkv_free_reply(get_reply);

    {
        char *response = NULL;
        hotkv_llm_usage_t usage;
        memset(&usage, 0, sizeof(usage));

        rc = fn(userdata, &response, &usage);
        if (rc != HOTKV_OK || !response) {
            hotkv_free(response);
            return rc != HOTKV_OK ? rc : HOTKV_ERR_OTHER;
        }

        {
            hotkv_token_usage_t group;
            hotkv_reply_t *set_reply = hotkv_scache_set(sc->ctx, sc->ns, strlen(sc->ns), query,
                                                        querylen, response, strlen(response),
                                                        vector, dims, sc->ttl_seconds,
                                                        hkv_llm_usage_as_token_usage(&usage, &group));
            hotkv_free_reply(set_reply);
        }

        *out_response = response;
        return HOTKV_OK;
    }
}

int hotkv_semanticcache_stats(hotkv_semanticcache_t *sc, hotkv_scache_stats_t *out) {
    hotkv_reply_t *r;
    int rc;
    if (!sc || !out) {
        return HOTKV_ERR_OTHER;
    }
    r = hotkv_scache_stats(sc->ctx, sc->ns, strlen(sc->ns));
    if (!r) {
        return hotkv_context_err(sc->ctx);
    }
    rc = hotkv_parse_scache_stats(r, out);
    hotkv_free_reply(r);
    return rc;
}
