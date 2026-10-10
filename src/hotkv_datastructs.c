/**
 * @file hotkv_datastructs.c
 * @brief Typed wrappers for the remaining core data-structure commands
 * (keys, bitmaps, lists, sets, hashes, sorted sets, geo, HyperLogLog,
 * streams, scripting, pub/sub subscription). Declarations and conventions:
 * see the "Extended data-structure commands" section of hotkv.h.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * Every wrapper builds its argv with the shared hkv_cmd_t builder, so there
 * is no per-command buffer arithmetic and numbers are formatted in one place.
 */

#include "hotkv.h"
#include "hotkv_private.h"
#include "hotkv_cmd.h"

#include <inttypes.h>

/* ------------------------------------------------------------------------- */
/* Local helpers                                                             */
/* ------------------------------------------------------------------------- */

static void add_i64(hkv_cmd_t *c, int64_t v) {
    hkv_cmd_addf(c, "%" PRId64, v);
}

static void add_u64(hkv_cmd_t *c, uint64_t v) {
    hkv_cmd_addf(c, "%" PRIu64, v);
}

static void add_count(hkv_cmd_t *c, size_t n) {
    hkv_cmd_addf(c, "%zu", n);
}

/* Appends n binary-safe items. */
static void add_items(hkv_cmd_t *c, const char **items, const size_t *lens, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        hkv_cmd_add(c, items[i], lens ? lens[i] : strlen(items[i]));
    }
}

/* Appends "FIELDS numfields field ..." (hash field-level commands). */
static void add_fields(hkv_cmd_t *c, const char **fields, const size_t *lens, size_t n) {
    hkv_cmd_adds(c, "FIELDS");
    add_count(c, n);
    add_items(c, fields, lens, n);
}

static void add_weights_aggregate(hkv_cmd_t *c, size_t nkeys, const double *weights,
                                  const char *aggregate) {
    size_t i;
    if (weights) {
        hkv_cmd_adds(c, "WEIGHTS");
        for (i = 0; i < nkeys; i++) hkv_cmd_add_double(c, weights[i], 0);
    }
    if (aggregate) {
        hkv_cmd_adds(c, "AGGREGATE");
        hkv_cmd_adds(c, aggregate);
    }
}

/* NAME key */
static hotkv_reply_t *cmd_key(hotkv_context_t *ctx, const char *name, const char *key,
                              size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

/* NAME key1 key2 ... */
static hotkv_reply_t *cmd_keys(hotkv_context_t *ctx, const char *name, const char **keys,
                               const size_t *keylens, size_t nkeys) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    add_items(&c, keys, keylens, nkeys);
    return hkv_cmd_exec(ctx, &c);
}

/* NAME key item item ... */
static hotkv_reply_t *cmd_key_items(hotkv_context_t *ctx, const char *name, const char *key,
                                    size_t keylen, const char **items, const size_t *lens,
                                    size_t n) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, key, keylen);
    add_items(&c, items, lens, n);
    return hkv_cmd_exec(ctx, &c);
}

/* NAME key int64 */
static hotkv_reply_t *cmd_key_i64(hotkv_context_t *ctx, const char *name, const char *key,
                                  size_t keylen, int64_t v) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, key, keylen);
    add_i64(&c, v);
    return hkv_cmd_exec(ctx, &c);
}

/* NAME key1 key2 */
static hotkv_reply_t *cmd_key_key(hotkv_context_t *ctx, const char *name, const char *a,
                                  size_t alen, const char *b, size_t blen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, a, alen);
    hkv_cmd_add(&c, b, blen);
    return hkv_cmd_exec(ctx, &c);
}

/* NAME dest key key ... */
static hotkv_reply_t *cmd_dest_keys(hotkv_context_t *ctx, const char *name, const char *dest,
                                    size_t destlen, const char **keys, const size_t *keylens,
                                    size_t nkeys) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, dest, destlen);
    add_items(&c, keys, keylens, nkeys);
    return hkv_cmd_exec(ctx, &c);
}

/* NAME numkeys key ... [LIMIT n] */
static hotkv_reply_t *cmd_numkeys_limit(hotkv_context_t *ctx, const char *name,
                                        const char **keys, const size_t *keylens, size_t nkeys,
                                        int64_t limit) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    add_count(&c, nkeys);
    add_items(&c, keys, keylens, nkeys);
    if (limit >= 0) {
        hkv_cmd_adds(&c, "LIMIT");
        add_i64(&c, limit);
    }
    return hkv_cmd_exec(ctx, &c);
}

/* NAME key cursor [MATCH pattern] [COUNT n] */
static hotkv_reply_t *cmd_scan_like(hotkv_context_t *ctx, const char *name, const char *key,
                                    size_t keylen, uint64_t cursor, const char *pattern,
                                    int64_t count) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, key, keylen);
    add_u64(&c, cursor);
    if (pattern) {
        hkv_cmd_adds(&c, "MATCH");
        hkv_cmd_adds(&c, pattern);
    }
    if (count > 0) {
        hkv_cmd_adds(&c, "COUNT");
        add_i64(&c, count);
    }
    return hkv_cmd_exec(ctx, &c);
}

/* NAME key FIELDS n field ... */
static hotkv_reply_t *cmd_key_fields(hotkv_context_t *ctx, const char *name, const char *key,
                                     size_t keylen, const char **fields, const size_t *lens,
                                     size_t n) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, key, keylen);
    add_fields(&c, fields, lens, n);
    return hkv_cmd_exec(ctx, &c);
}

/* ------------------------------------------------------------------------- */
/* Keys                                                                      */
/* ------------------------------------------------------------------------- */

hotkv_reply_t *hotkv_unlink(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                            size_t nkeys) {
    return cmd_keys(ctx, "UNLINK", keys, keylens, nkeys);
}

hotkv_reply_t *hotkv_touch(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                           size_t nkeys) {
    return cmd_keys(ctx, "TOUCH", keys, keylens, nkeys);
}

hotkv_reply_t *hotkv_expireat(hotkv_context_t *ctx, const char *key, size_t keylen,
                              int64_t unix_seconds) {
    return cmd_key_i64(ctx, "EXPIREAT", key, keylen, unix_seconds);
}

hotkv_reply_t *hotkv_pexpireat(hotkv_context_t *ctx, const char *key, size_t keylen,
                               int64_t unix_milliseconds) {
    return cmd_key_i64(ctx, "PEXPIREAT", key, keylen, unix_milliseconds);
}

hotkv_reply_t *hotkv_expiretime(hotkv_context_t *ctx, const char *key, size_t keylen) {
    return cmd_key(ctx, "EXPIRETIME", key, keylen);
}

hotkv_reply_t *hotkv_pexpiretime(hotkv_context_t *ctx, const char *key, size_t keylen) {
    return cmd_key(ctx, "PEXPIRETIME", key, keylen);
}

hotkv_reply_t *hotkv_renamenx(hotkv_context_t *ctx, const char *key, size_t keylen,
                              const char *newkey, size_t newkeylen) {
    return cmd_key_key(ctx, "RENAMENX", key, keylen, newkey, newkeylen);
}

hotkv_reply_t *hotkv_copy(hotkv_context_t *ctx, const char *src, size_t srclen, const char *dst,
                          size_t dstlen, int replace) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "COPY");
    hkv_cmd_add(&c, src, srclen);
    hkv_cmd_add(&c, dst, dstlen);
    if (replace) hkv_cmd_adds(&c, "REPLACE");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_move(hotkv_context_t *ctx, const char *key, size_t keylen, int db) {
    return cmd_key_i64(ctx, "MOVE", key, keylen, db);
}

hotkv_reply_t *hotkv_randomkey(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RANDOMKEY");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_dump(hotkv_context_t *ctx, const char *key, size_t keylen) {
    return cmd_key(ctx, "DUMP", key, keylen);
}

hotkv_reply_t *hotkv_restore(hotkv_context_t *ctx, const char *key, size_t keylen, int64_t ttl_ms,
                             const char *payload, size_t payloadlen, int replace) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "RESTORE");
    hkv_cmd_add(&c, key, keylen);
    add_i64(&c, ttl_ms);
    hkv_cmd_add(&c, payload, payloadlen);
    if (replace) hkv_cmd_adds(&c, "REPLACE");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_watch(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                           size_t nkeys) {
    return cmd_keys(ctx, "WATCH", keys, keylens, nkeys);
}

hotkv_reply_t *hotkv_unwatch(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "UNWATCH");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_time(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "TIME");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_lastsave(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LASTSAVE");
    return hkv_cmd_exec(ctx, &c);
}

/* ------------------------------------------------------------------------- */
/* Bitmaps                                                                   */
/* ------------------------------------------------------------------------- */

hotkv_reply_t *hotkv_setbit(hotkv_context_t *ctx, const char *key, size_t keylen, int64_t offset,
                            int bit) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SETBIT");
    hkv_cmd_add(&c, key, keylen);
    add_i64(&c, offset);
    hkv_cmd_adds(&c, bit ? "1" : "0");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_getbit(hotkv_context_t *ctx, const char *key, size_t keylen,
                            int64_t offset) {
    return cmd_key_i64(ctx, "GETBIT", key, keylen, offset);
}

hotkv_reply_t *hotkv_bitcount(hotkv_context_t *ctx, const char *key, size_t keylen, int has_range,
                              int64_t start, int64_t end, const char *mode) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "BITCOUNT");
    hkv_cmd_add(&c, key, keylen);
    if (has_range) {
        add_i64(&c, start);
        add_i64(&c, end);
        if (mode) hkv_cmd_adds(&c, mode);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_bitpos(hotkv_context_t *ctx, const char *key, size_t keylen, int bit,
                            int has_start, int64_t start, int has_end, int64_t end,
                            const char *mode) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "BITPOS");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, bit ? "1" : "0");
    if (has_start) {
        add_i64(&c, start);
        if (has_end) {
            add_i64(&c, end);
            if (mode) hkv_cmd_adds(&c, mode);
        }
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_bitop(hotkv_context_t *ctx, const char *operation, const char *dest,
                           size_t destlen, const char **keys, const size_t *keylens,
                           size_t nkeys) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "BITOP");
    hkv_cmd_adds(&c, operation);
    hkv_cmd_add(&c, dest, destlen);
    add_items(&c, keys, keylens, nkeys);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_lcs(hotkv_context_t *ctx, const char *key1, size_t key1len, const char *key2,
                         size_t key2len, int len_only) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LCS");
    hkv_cmd_add(&c, key1, key1len);
    hkv_cmd_add(&c, key2, key2len);
    if (len_only) hkv_cmd_adds(&c, "LEN");
    return hkv_cmd_exec(ctx, &c);
}

/* ------------------------------------------------------------------------- */
/* Lists                                                                     */
/* ------------------------------------------------------------------------- */

hotkv_reply_t *hotkv_ltrim(hotkv_context_t *ctx, const char *key, size_t keylen, int64_t start,
                           int64_t stop) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LTRIM");
    hkv_cmd_add(&c, key, keylen);
    add_i64(&c, start);
    add_i64(&c, stop);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_linsert(hotkv_context_t *ctx, const char *key, size_t keylen, int before,
                             const char *pivot, size_t pivotlen, const char *value,
                             size_t valuelen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LINSERT");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, before ? "BEFORE" : "AFTER");
    hkv_cmd_add(&c, pivot, pivotlen);
    hkv_cmd_add(&c, value, valuelen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_lpos(hotkv_context_t *ctx, const char *key, size_t keylen,
                          const char *element, size_t elementlen, int64_t rank, int64_t count,
                          int64_t maxlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LPOS");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_add(&c, element, elementlen);
    if (rank != 0) {
        hkv_cmd_adds(&c, "RANK");
        add_i64(&c, rank);
    }
    if (count >= 0) {
        hkv_cmd_adds(&c, "COUNT");
        add_i64(&c, count);
    }
    if (maxlen >= 0) {
        hkv_cmd_adds(&c, "MAXLEN");
        add_i64(&c, maxlen);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_lmove(hotkv_context_t *ctx, const char *src, size_t srclen, const char *dst,
                           size_t dstlen, const char *wherefrom, const char *whereto) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "LMOVE");
    hkv_cmd_add(&c, src, srclen);
    hkv_cmd_add(&c, dst, dstlen);
    hkv_cmd_adds(&c, wherefrom);
    hkv_cmd_adds(&c, whereto);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_rpoplpush(hotkv_context_t *ctx, const char *src, size_t srclen,
                               const char *dst, size_t dstlen) {
    return cmd_key_key(ctx, "RPOPLPUSH", src, srclen, dst, dstlen);
}

hotkv_reply_t *hotkv_lpushx(hotkv_context_t *ctx, const char *key, size_t keylen,
                            const char **vals, const size_t *vallens, size_t nvals) {
    return cmd_key_items(ctx, "LPUSHX", key, keylen, vals, vallens, nvals);
}

hotkv_reply_t *hotkv_rpushx(hotkv_context_t *ctx, const char *key, size_t keylen,
                            const char **vals, const size_t *vallens, size_t nvals) {
    return cmd_key_items(ctx, "RPUSHX", key, keylen, vals, vallens, nvals);
}

hotkv_reply_t *hotkv_lpop_count(hotkv_context_t *ctx, const char *key, size_t keylen,
                                int64_t count) {
    return cmd_key_i64(ctx, "LPOP", key, keylen, count);
}

hotkv_reply_t *hotkv_rpop_count(hotkv_context_t *ctx, const char *key, size_t keylen,
                                int64_t count) {
    return cmd_key_i64(ctx, "RPOP", key, keylen, count);
}

/* ------------------------------------------------------------------------- */
/* Sets                                                                      */
/* ------------------------------------------------------------------------- */

hotkv_reply_t *hotkv_sdiff(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                           size_t nkeys) {
    return cmd_keys(ctx, "SDIFF", keys, keylens, nkeys);
}

hotkv_reply_t *hotkv_sinter(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                            size_t nkeys) {
    return cmd_keys(ctx, "SINTER", keys, keylens, nkeys);
}

hotkv_reply_t *hotkv_sunion(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                            size_t nkeys) {
    return cmd_keys(ctx, "SUNION", keys, keylens, nkeys);
}

hotkv_reply_t *hotkv_sdiffstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                const char **keys, const size_t *keylens, size_t nkeys) {
    return cmd_dest_keys(ctx, "SDIFFSTORE", dest, destlen, keys, keylens, nkeys);
}

hotkv_reply_t *hotkv_sinterstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                 const char **keys, const size_t *keylens, size_t nkeys) {
    return cmd_dest_keys(ctx, "SINTERSTORE", dest, destlen, keys, keylens, nkeys);
}

hotkv_reply_t *hotkv_sunionstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                 const char **keys, const size_t *keylens, size_t nkeys) {
    return cmd_dest_keys(ctx, "SUNIONSTORE", dest, destlen, keys, keylens, nkeys);
}

hotkv_reply_t *hotkv_sintercard(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                                size_t nkeys, int64_t limit) {
    return cmd_numkeys_limit(ctx, "SINTERCARD", keys, keylens, nkeys, limit);
}

hotkv_reply_t *hotkv_smismember(hotkv_context_t *ctx, const char *key, size_t keylen,
                                const char **members, const size_t *memlens, size_t nmembers) {
    return cmd_key_items(ctx, "SMISMEMBER", key, keylen, members, memlens, nmembers);
}

hotkv_reply_t *hotkv_smove(hotkv_context_t *ctx, const char *src, size_t srclen, const char *dst,
                           size_t dstlen, const char *member, size_t memlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SMOVE");
    hkv_cmd_add(&c, src, srclen);
    hkv_cmd_add(&c, dst, dstlen);
    hkv_cmd_add(&c, member, memlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_spop(hotkv_context_t *ctx, const char *key, size_t keylen) {
    return cmd_key(ctx, "SPOP", key, keylen);
}

hotkv_reply_t *hotkv_spop_count(hotkv_context_t *ctx, const char *key, size_t keylen,
                                int64_t count) {
    return cmd_key_i64(ctx, "SPOP", key, keylen, count);
}

hotkv_reply_t *hotkv_srandmember(hotkv_context_t *ctx, const char *key, size_t keylen) {
    return cmd_key(ctx, "SRANDMEMBER", key, keylen);
}

hotkv_reply_t *hotkv_srandmember_count(hotkv_context_t *ctx, const char *key, size_t keylen,
                                       int64_t count) {
    return cmd_key_i64(ctx, "SRANDMEMBER", key, keylen, count);
}

hotkv_reply_t *hotkv_sscan(hotkv_context_t *ctx, const char *key, size_t keylen, uint64_t cursor,
                           const char *pattern, int64_t count) {
    return cmd_scan_like(ctx, "SSCAN", key, keylen, cursor, pattern, count);
}

/* ------------------------------------------------------------------------- */
/* Hashes                                                                    */
/* ------------------------------------------------------------------------- */

hotkv_reply_t *hotkv_hsetnx(hotkv_context_t *ctx, const char *key, size_t keylen,
                            const char *field, size_t fieldlen, const char *val, size_t vallen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "HSETNX");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_add(&c, field, fieldlen);
    hkv_cmd_add(&c, val, vallen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_hmset(hotkv_context_t *ctx, const char *key, size_t keylen,
                           const char **fields, const size_t *fieldlens, const char **vals,
                           const size_t *vallens, size_t npairs) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "HMSET");
    hkv_cmd_add(&c, key, keylen);
    for (i = 0; i < npairs; i++) {
        hkv_cmd_add(&c, fields[i], fieldlens[i]);
        hkv_cmd_add(&c, vals[i], vallens[i]);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_hkeys(hotkv_context_t *ctx, const char *key, size_t keylen) {
    return cmd_key(ctx, "HKEYS", key, keylen);
}

hotkv_reply_t *hotkv_hvals(hotkv_context_t *ctx, const char *key, size_t keylen) {
    return cmd_key(ctx, "HVALS", key, keylen);
}

hotkv_reply_t *hotkv_hstrlen(hotkv_context_t *ctx, const char *key, size_t keylen,
                             const char *field, size_t fieldlen) {
    return cmd_key_key(ctx, "HSTRLEN", key, keylen, field, fieldlen);
}

hotkv_reply_t *hotkv_hscan(hotkv_context_t *ctx, const char *key, size_t keylen, uint64_t cursor,
                           const char *pattern, int64_t count) {
    return cmd_scan_like(ctx, "HSCAN", key, keylen, cursor, pattern, count);
}

hotkv_reply_t *hotkv_hgetdel(hotkv_context_t *ctx, const char *key, size_t keylen,
                             const char **fields, const size_t *fieldlens, size_t nfields) {
    return cmd_key_fields(ctx, "HGETDEL", key, keylen, fields, fieldlens, nfields);
}

static hotkv_reply_t *hexpire_impl(hotkv_context_t *ctx, const char *name, const char *key,
                                   size_t keylen, int64_t amount, const char *condition,
                                   const char **fields, const size_t *fieldlens,
                                   size_t nfields) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, key, keylen);
    add_i64(&c, amount);
    if (condition) hkv_cmd_adds(&c, condition);
    add_fields(&c, fields, fieldlens, nfields);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_hexpire(hotkv_context_t *ctx, const char *key, size_t keylen,
                             int64_t seconds, const char *condition, const char **fields,
                             const size_t *fieldlens, size_t nfields) {
    return hexpire_impl(ctx, "HEXPIRE", key, keylen, seconds, condition, fields, fieldlens,
                        nfields);
}

hotkv_reply_t *hotkv_hpexpire(hotkv_context_t *ctx, const char *key, size_t keylen,
                              int64_t milliseconds, const char *condition, const char **fields,
                              const size_t *fieldlens, size_t nfields) {
    return hexpire_impl(ctx, "HPEXPIRE", key, keylen, milliseconds, condition, fields,
                        fieldlens, nfields);
}

hotkv_reply_t *hotkv_httl(hotkv_context_t *ctx, const char *key, size_t keylen,
                          const char **fields, const size_t *fieldlens, size_t nfields) {
    return cmd_key_fields(ctx, "HTTL", key, keylen, fields, fieldlens, nfields);
}

hotkv_reply_t *hotkv_hpttl(hotkv_context_t *ctx, const char *key, size_t keylen,
                           const char **fields, const size_t *fieldlens, size_t nfields) {
    return cmd_key_fields(ctx, "HPTTL", key, keylen, fields, fieldlens, nfields);
}

hotkv_reply_t *hotkv_hpersist(hotkv_context_t *ctx, const char *key, size_t keylen,
                              const char **fields, const size_t *fieldlens, size_t nfields) {
    return cmd_key_fields(ctx, "HPERSIST", key, keylen, fields, fieldlens, nfields);
}

hotkv_reply_t *hotkv_hexpiretime(hotkv_context_t *ctx, const char *key, size_t keylen,
                                 const char **fields, const size_t *fieldlens, size_t nfields) {
    return cmd_key_fields(ctx, "HEXPIRETIME", key, keylen, fields, fieldlens, nfields);
}

hotkv_reply_t *hotkv_hpexpiretime(hotkv_context_t *ctx, const char *key, size_t keylen,
                                  const char **fields, const size_t *fieldlens,
                                  size_t nfields) {
    return cmd_key_fields(ctx, "HPEXPIRETIME", key, keylen, fields, fieldlens, nfields);
}

/* ------------------------------------------------------------------------- */
/* Sorted sets                                                               */
/* ------------------------------------------------------------------------- */

hotkv_reply_t *hotkv_zincrby(hotkv_context_t *ctx, const char *key, size_t keylen,
                             double increment, const char *member, size_t memlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "ZINCRBY");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_add_double(&c, increment, 0);
    hkv_cmd_add(&c, member, memlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_zmscore(hotkv_context_t *ctx, const char *key, size_t keylen,
                             const char **members, const size_t *memlens, size_t nmembers) {
    return cmd_key_items(ctx, "ZMSCORE", key, keylen, members, memlens, nmembers);
}

hotkv_reply_t *hotkv_zrevrank(hotkv_context_t *ctx, const char *key, size_t keylen,
                              const char *member, size_t memlen) {
    return cmd_key_key(ctx, "ZREVRANK", key, keylen, member, memlen);
}

hotkv_reply_t *hotkv_zrevrange(hotkv_context_t *ctx, const char *key, size_t keylen,
                               int64_t start, int64_t stop, int withscores) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "ZREVRANGE");
    hkv_cmd_add(&c, key, keylen);
    add_i64(&c, start);
    add_i64(&c, stop);
    if (withscores) hkv_cmd_adds(&c, "WITHSCORES");
    return hkv_cmd_exec(ctx, &c);
}

static hotkv_reply_t *range_by_score(hotkv_context_t *ctx, const char *name, const char *key,
                                     size_t keylen, const char *first, const char *second,
                                     int withscores, int64_t limit_offset, int64_t limit_count) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, first);
    hkv_cmd_adds(&c, second);
    if (withscores) hkv_cmd_adds(&c, "WITHSCORES");
    if (limit_count >= 0) {
        hkv_cmd_adds(&c, "LIMIT");
        add_i64(&c, limit_offset);
        add_i64(&c, limit_count);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_zrangebyscore(hotkv_context_t *ctx, const char *key, size_t keylen,
                                   const char *min, const char *max, int withscores,
                                   int64_t limit_offset, int64_t limit_count) {
    return range_by_score(ctx, "ZRANGEBYSCORE", key, keylen, min, max, withscores, limit_offset,
                          limit_count);
}

hotkv_reply_t *hotkv_zrevrangebyscore(hotkv_context_t *ctx, const char *key, size_t keylen,
                                      const char *max, const char *min, int withscores,
                                      int64_t limit_offset, int64_t limit_count) {
    return range_by_score(ctx, "ZREVRANGEBYSCORE", key, keylen, max, min, withscores,
                          limit_offset, limit_count);
}

hotkv_reply_t *hotkv_zrangebylex(hotkv_context_t *ctx, const char *key, size_t keylen,
                                 const char *min, const char *max, int64_t limit_offset,
                                 int64_t limit_count) {
    return range_by_score(ctx, "ZRANGEBYLEX", key, keylen, min, max, 0, limit_offset,
                          limit_count);
}

hotkv_reply_t *hotkv_zrevrangebylex(hotkv_context_t *ctx, const char *key, size_t keylen,
                                    const char *max, const char *min, int64_t limit_offset,
                                    int64_t limit_count) {
    return range_by_score(ctx, "ZREVRANGEBYLEX", key, keylen, max, min, 0, limit_offset,
                          limit_count);
}

static hotkv_reply_t *key_two_strings(hotkv_context_t *ctx, const char *name, const char *key,
                                      size_t keylen, const char *a, const char *b) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, a);
    hkv_cmd_adds(&c, b);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_zlexcount(hotkv_context_t *ctx, const char *key, size_t keylen,
                               const char *min, const char *max) {
    return key_two_strings(ctx, "ZLEXCOUNT", key, keylen, min, max);
}

hotkv_reply_t *hotkv_zremrangebyscore(hotkv_context_t *ctx, const char *key, size_t keylen,
                                      const char *min, const char *max) {
    return key_two_strings(ctx, "ZREMRANGEBYSCORE", key, keylen, min, max);
}

hotkv_reply_t *hotkv_zremrangebylex(hotkv_context_t *ctx, const char *key, size_t keylen,
                                    const char *min, const char *max) {
    return key_two_strings(ctx, "ZREMRANGEBYLEX", key, keylen, min, max);
}

hotkv_reply_t *hotkv_zremrangebyrank(hotkv_context_t *ctx, const char *key, size_t keylen,
                                     int64_t start, int64_t stop) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "ZREMRANGEBYRANK");
    hkv_cmd_add(&c, key, keylen);
    add_i64(&c, start);
    add_i64(&c, stop);
    return hkv_cmd_exec(ctx, &c);
}

static hotkv_reply_t *zstore_impl(hotkv_context_t *ctx, const char *name, const char *dest,
                                  size_t destlen, const char **keys, const size_t *keylens,
                                  size_t nkeys, const double *weights, const char *aggregate) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, dest, destlen);
    add_count(&c, nkeys);
    add_items(&c, keys, keylens, nkeys);
    add_weights_aggregate(&c, nkeys, weights, aggregate);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_zunionstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                 const char **keys, const size_t *keylens, size_t nkeys,
                                 const double *weights, const char *aggregate) {
    return zstore_impl(ctx, "ZUNIONSTORE", dest, destlen, keys, keylens, nkeys, weights,
                       aggregate);
}

hotkv_reply_t *hotkv_zinterstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                 const char **keys, const size_t *keylens, size_t nkeys,
                                 const double *weights, const char *aggregate) {
    return zstore_impl(ctx, "ZINTERSTORE", dest, destlen, keys, keylens, nkeys, weights,
                       aggregate);
}

static hotkv_reply_t *zsetop_impl(hotkv_context_t *ctx, const char *name, const char **keys,
                                  const size_t *keylens, size_t nkeys, const double *weights,
                                  const char *aggregate, int withscores) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    add_count(&c, nkeys);
    add_items(&c, keys, keylens, nkeys);
    add_weights_aggregate(&c, nkeys, weights, aggregate);
    if (withscores) hkv_cmd_adds(&c, "WITHSCORES");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_zunion(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                            size_t nkeys, const double *weights, const char *aggregate,
                            int withscores) {
    return zsetop_impl(ctx, "ZUNION", keys, keylens, nkeys, weights, aggregate, withscores);
}

hotkv_reply_t *hotkv_zinter(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                            size_t nkeys, const double *weights, const char *aggregate,
                            int withscores) {
    return zsetop_impl(ctx, "ZINTER", keys, keylens, nkeys, weights, aggregate, withscores);
}

hotkv_reply_t *hotkv_zdiff(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                           size_t nkeys, int withscores) {
    return zsetop_impl(ctx, "ZDIFF", keys, keylens, nkeys, NULL, NULL, withscores);
}

hotkv_reply_t *hotkv_zdiffstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                const char **keys, const size_t *keylens, size_t nkeys) {
    return zstore_impl(ctx, "ZDIFFSTORE", dest, destlen, keys, keylens, nkeys, NULL, NULL);
}

hotkv_reply_t *hotkv_zintercard(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                                size_t nkeys, int64_t limit) {
    return cmd_numkeys_limit(ctx, "ZINTERCARD", keys, keylens, nkeys, limit);
}

hotkv_reply_t *hotkv_zscan(hotkv_context_t *ctx, const char *key, size_t keylen, uint64_t cursor,
                           const char *pattern, int64_t count) {
    return cmd_scan_like(ctx, "ZSCAN", key, keylen, cursor, pattern, count);
}

/* ------------------------------------------------------------------------- */
/* Geo                                                                       */
/* ------------------------------------------------------------------------- */

hotkv_reply_t *hotkv_geoadd(hotkv_context_t *ctx, const char *key, size_t keylen,
                            const double *lons, const double *lats, const char **members,
                            const size_t *memlens, size_t npoints) {
    hkv_cmd_t c;
    size_t i;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "GEOADD");
    hkv_cmd_add(&c, key, keylen);
    for (i = 0; i < npoints; i++) {
        hkv_cmd_add_double(&c, lons[i], 1);
        hkv_cmd_add_double(&c, lats[i], 1);
        hkv_cmd_add(&c, members[i], memlens[i]);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_geodist(hotkv_context_t *ctx, const char *key, size_t keylen,
                             const char *member1, size_t member1len, const char *member2,
                             size_t member2len, const char *unit) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "GEODIST");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_add(&c, member1, member1len);
    hkv_cmd_add(&c, member2, member2len);
    if (unit) hkv_cmd_adds(&c, unit);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_geopos(hotkv_context_t *ctx, const char *key, size_t keylen,
                            const char **members, const size_t *memlens, size_t nmembers) {
    return cmd_key_items(ctx, "GEOPOS", key, keylen, members, memlens, nmembers);
}

hotkv_reply_t *hotkv_geohash(hotkv_context_t *ctx, const char *key, size_t keylen,
                             const char **members, const size_t *memlens, size_t nmembers) {
    return cmd_key_items(ctx, "GEOHASH", key, keylen, members, memlens, nmembers);
}

hotkv_reply_t *hotkv_geosearch(hotkv_context_t *ctx, const char *key, size_t keylen,
                               const hotkv_geosearch_opts_t *o) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    if (!o) {
        hotkv_set_error(ctx, HOTKV_ERR_OTHER, "geosearch options are required");
        return NULL;
    }
    hkv_cmd_adds(&c, "GEOSEARCH");
    hkv_cmd_add(&c, key, keylen);
    if (o->from_member) {
        hkv_cmd_adds(&c, "FROMMEMBER");
        hkv_cmd_adds(&c, o->from_member);
    }
    if (o->from_lonlat) {
        hkv_cmd_adds(&c, "FROMLONLAT");
        hkv_cmd_add_double(&c, o->lon, 1);
        hkv_cmd_add_double(&c, o->lat, 1);
    }
    if (o->by_radius) {
        hkv_cmd_adds(&c, "BYRADIUS");
        hkv_cmd_add_double(&c, o->radius, 1);
        hkv_cmd_adds(&c, o->unit ? o->unit : "m");
    }
    if (o->by_box) {
        hkv_cmd_adds(&c, "BYBOX");
        hkv_cmd_add_double(&c, o->width, 1);
        hkv_cmd_add_double(&c, o->height, 1);
        hkv_cmd_adds(&c, o->unit ? o->unit : "m");
    }
    if (o->order == 1) hkv_cmd_adds(&c, "ASC");
    if (o->order == 2) hkv_cmd_adds(&c, "DESC");
    if (o->count > 0) {
        hkv_cmd_adds(&c, "COUNT");
        add_i64(&c, o->count);
        if (o->any) hkv_cmd_adds(&c, "ANY");
    }
    if (o->withcoord) hkv_cmd_adds(&c, "WITHCOORD");
    if (o->withdist) hkv_cmd_adds(&c, "WITHDIST");
    if (o->withhash) hkv_cmd_adds(&c, "WITHHASH");
    return hkv_cmd_exec(ctx, &c);
}

/* ------------------------------------------------------------------------- */
/* HyperLogLog                                                               */
/* ------------------------------------------------------------------------- */

hotkv_reply_t *hotkv_pfadd(hotkv_context_t *ctx, const char *key, size_t keylen,
                           const char **elements, const size_t *elemlens, size_t nelements) {
    return cmd_key_items(ctx, "PFADD", key, keylen, elements, elemlens, nelements);
}

hotkv_reply_t *hotkv_pfcount(hotkv_context_t *ctx, const char **keys, const size_t *keylens,
                             size_t nkeys) {
    return cmd_keys(ctx, "PFCOUNT", keys, keylens, nkeys);
}

hotkv_reply_t *hotkv_pfmerge(hotkv_context_t *ctx, const char *dest, size_t destlen,
                             const char **keys, const size_t *keylens, size_t nkeys) {
    return cmd_dest_keys(ctx, "PFMERGE", dest, destlen, keys, keylens, nkeys);
}

/* ------------------------------------------------------------------------- */
/* Streams                                                                   */
/* ------------------------------------------------------------------------- */

hotkv_reply_t *hotkv_xdel(hotkv_context_t *ctx, const char *key, size_t keylen, const char **ids,
                          const size_t *idlens, size_t nids) {
    return cmd_key_items(ctx, "XDEL", key, keylen, ids, idlens, nids);
}

hotkv_reply_t *hotkv_xtrim(hotkv_context_t *ctx, const char *key, size_t keylen,
                           const char *strategy, int approximate, const char *threshold) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XTRIM");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, strategy);
    if (approximate) hkv_cmd_adds(&c, "~");
    hkv_cmd_adds(&c, threshold);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_xrevrange(hotkv_context_t *ctx, const char *key, size_t keylen,
                               const char *end, const char *start, int64_t count) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XREVRANGE");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, end);
    hkv_cmd_adds(&c, start);
    if (count > 0) {
        hkv_cmd_adds(&c, "COUNT");
        add_i64(&c, count);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_xgroup_create(hotkv_context_t *ctx, const char *key, size_t keylen,
                                   const char *group, const char *id, int mkstream) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XGROUP");
    hkv_cmd_adds(&c, "CREATE");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, group);
    hkv_cmd_adds(&c, id);
    if (mkstream) hkv_cmd_adds(&c, "MKSTREAM");
    return hkv_cmd_exec(ctx, &c);
}

static hotkv_reply_t *xgroup_sub(hotkv_context_t *ctx, const char *sub, const char *key,
                                 size_t keylen, const char *group, const char *extra) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XGROUP");
    hkv_cmd_adds(&c, sub);
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, group);
    if (extra) hkv_cmd_adds(&c, extra);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_xgroup_destroy(hotkv_context_t *ctx, const char *key, size_t keylen,
                                    const char *group) {
    return xgroup_sub(ctx, "DESTROY", key, keylen, group, NULL);
}

hotkv_reply_t *hotkv_xgroup_createconsumer(hotkv_context_t *ctx, const char *key, size_t keylen,
                                           const char *group, const char *consumer) {
    return xgroup_sub(ctx, "CREATECONSUMER", key, keylen, group, consumer);
}

hotkv_reply_t *hotkv_xgroup_delconsumer(hotkv_context_t *ctx, const char *key, size_t keylen,
                                        const char *group, const char *consumer) {
    return xgroup_sub(ctx, "DELCONSUMER", key, keylen, group, consumer);
}

hotkv_reply_t *hotkv_xgroup_setid(hotkv_context_t *ctx, const char *key, size_t keylen,
                                  const char *group, const char *id) {
    return xgroup_sub(ctx, "SETID", key, keylen, group, id);
}

hotkv_reply_t *hotkv_xack(hotkv_context_t *ctx, const char *key, size_t keylen, const char *group,
                          const char **ids, const size_t *idlens, size_t nids) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XACK");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, group);
    add_items(&c, ids, idlens, nids);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_xreadgroup(hotkv_context_t *ctx, const char *group, const char *consumer,
                                const char **keys, const size_t *keylens, const char **ids,
                                const size_t *idlens, size_t nstreams, int64_t count,
                                int noack) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XREADGROUP");
    hkv_cmd_adds(&c, "GROUP");
    hkv_cmd_adds(&c, group);
    hkv_cmd_adds(&c, consumer);
    if (count > 0) {
        hkv_cmd_adds(&c, "COUNT");
        add_i64(&c, count);
    }
    if (noack) hkv_cmd_adds(&c, "NOACK");
    hkv_cmd_adds(&c, "STREAMS");
    add_items(&c, keys, keylens, nstreams);
    add_items(&c, ids, idlens, nstreams);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_xpending(hotkv_context_t *ctx, const char *key, size_t keylen,
                              const char *group) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XPENDING");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, group);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_xpending_range(hotkv_context_t *ctx, const char *key, size_t keylen,
                                    const char *group, const char *start, const char *end,
                                    int64_t count, const char *consumer) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XPENDING");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, group);
    hkv_cmd_adds(&c, start);
    hkv_cmd_adds(&c, end);
    add_i64(&c, count);
    if (consumer) hkv_cmd_adds(&c, consumer);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_xclaim(hotkv_context_t *ctx, const char *key, size_t keylen,
                            const char *group, const char *consumer, int64_t min_idle_ms,
                            const char **ids, const size_t *idlens, size_t nids) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XCLAIM");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, group);
    hkv_cmd_adds(&c, consumer);
    add_i64(&c, min_idle_ms);
    add_items(&c, ids, idlens, nids);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_xautoclaim(hotkv_context_t *ctx, const char *key, size_t keylen,
                                const char *group, const char *consumer, int64_t min_idle_ms,
                                const char *start, int64_t count) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XAUTOCLAIM");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, group);
    hkv_cmd_adds(&c, consumer);
    add_i64(&c, min_idle_ms);
    hkv_cmd_adds(&c, start);
    if (count > 0) {
        hkv_cmd_adds(&c, "COUNT");
        add_i64(&c, count);
    }
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_xinfo_groups(hotkv_context_t *ctx, const char *key, size_t keylen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XINFO");
    hkv_cmd_adds(&c, "GROUPS");
    hkv_cmd_add(&c, key, keylen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_xinfo_consumers(hotkv_context_t *ctx, const char *key, size_t keylen,
                                     const char *group) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "XINFO");
    hkv_cmd_adds(&c, "CONSUMERS");
    hkv_cmd_add(&c, key, keylen);
    hkv_cmd_adds(&c, group);
    return hkv_cmd_exec(ctx, &c);
}

/* ------------------------------------------------------------------------- */
/* Scripting                                                                 */
/* ------------------------------------------------------------------------- */

/* NAME <script-or-sha-or-function> numkeys key ... arg ... */
static hotkv_reply_t *script_call(hotkv_context_t *ctx, const char *name, const char *body,
                                  size_t bodylen, const char **keys, const size_t *keylens,
                                  size_t nkeys, const char **args, const size_t *arglens,
                                  size_t nargs) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    hkv_cmd_add(&c, body, bodylen);
    add_count(&c, nkeys);
    add_items(&c, keys, keylens, nkeys);
    add_items(&c, args, arglens, nargs);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_eval(hotkv_context_t *ctx, const char *script, size_t scriptlen,
                          const char **keys, const size_t *keylens, size_t nkeys,
                          const char **args, const size_t *arglens, size_t nargs) {
    return script_call(ctx, "EVAL", script, scriptlen, keys, keylens, nkeys, args, arglens,
                       nargs);
}

hotkv_reply_t *hotkv_eval_ro(hotkv_context_t *ctx, const char *script, size_t scriptlen,
                             const char **keys, const size_t *keylens, size_t nkeys,
                             const char **args, const size_t *arglens, size_t nargs) {
    return script_call(ctx, "EVAL_RO", script, scriptlen, keys, keylens, nkeys, args, arglens,
                       nargs);
}

hotkv_reply_t *hotkv_evalsha(hotkv_context_t *ctx, const char *sha1, const char **keys,
                             const size_t *keylens, size_t nkeys, const char **args,
                             const size_t *arglens, size_t nargs) {
    return script_call(ctx, "EVALSHA", sha1, strlen(sha1), keys, keylens, nkeys, args, arglens,
                       nargs);
}

hotkv_reply_t *hotkv_evalsha_ro(hotkv_context_t *ctx, const char *sha1, const char **keys,
                                const size_t *keylens, size_t nkeys, const char **args,
                                const size_t *arglens, size_t nargs) {
    return script_call(ctx, "EVALSHA_RO", sha1, strlen(sha1), keys, keylens, nkeys, args,
                       arglens, nargs);
}

hotkv_reply_t *hotkv_fcall(hotkv_context_t *ctx, const char *function, const char **keys,
                           const size_t *keylens, size_t nkeys, const char **args,
                           const size_t *arglens, size_t nargs) {
    return script_call(ctx, "FCALL", function, strlen(function), keys, keylens, nkeys, args,
                       arglens, nargs);
}

hotkv_reply_t *hotkv_fcall_ro(hotkv_context_t *ctx, const char *function, const char **keys,
                              const size_t *keylens, size_t nkeys, const char **args,
                              const size_t *arglens, size_t nargs) {
    return script_call(ctx, "FCALL_RO", function, strlen(function), keys, keylens, nkeys, args,
                       arglens, nargs);
}

hotkv_reply_t *hotkv_script_load(hotkv_context_t *ctx, const char *script, size_t scriptlen) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCRIPT");
    hkv_cmd_adds(&c, "LOAD");
    hkv_cmd_add(&c, script, scriptlen);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_script_exists(hotkv_context_t *ctx, const char **sha1s, size_t nsha1s) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCRIPT");
    hkv_cmd_adds(&c, "EXISTS");
    add_items(&c, sha1s, NULL, nsha1s);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_script_flush(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "SCRIPT");
    hkv_cmd_adds(&c, "FLUSH");
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_function_load(hotkv_context_t *ctx, const char *code, size_t codelen,
                                   int replace) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "FUNCTION");
    hkv_cmd_adds(&c, "LOAD");
    if (replace) hkv_cmd_adds(&c, "REPLACE");
    hkv_cmd_add(&c, code, codelen);
    return hkv_cmd_exec(ctx, &c);
}

/* ------------------------------------------------------------------------- */
/* Pub/Sub                                                                   */
/* ------------------------------------------------------------------------- */

/* Sends NAME item... and consumes one confirmation per item. An empty
 * UNSUBSCRIBE / PUNSUBSCRIBE (n == 0) cancels everything and the server
 * answers once per active subscription, which the caller reads with
 * hotkv_get_reply(); nothing is consumed here in that case. */
static int sub_impl(hotkv_context_t *ctx, const char *name, const char **items,
                    const size_t *lens, size_t n) {
    hkv_cmd_t c;
    size_t i;
    int rc;

    if (!ctx || ctx->err) return HOTKV_ERR_OTHER;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, name);
    add_items(&c, items, lens, n);
    if (c.oom || c.bad) {
        hkv_cmd_destroy(&c);
        return HOTKV_ERR_OOM;
    }
    rc = hotkv_append_command_argv(ctx, (int)c.count, c.argv, c.lens);
    hkv_cmd_destroy(&c);
    if (rc != HOTKV_OK) return rc;

    for (i = 0; i < n; i++) {
        hotkv_reply_t *r = NULL;
        rc = hotkv_get_reply(ctx, &r);
        if (rc != HOTKV_OK) return rc;
        if (r && r->type == HOTKV_REPLY_ERROR) {
            hotkv_free_reply(r);
            return HOTKV_ERR_OTHER;
        }
        hotkv_free_reply(r);
    }
    return HOTKV_OK;
}

int hotkv_subscribe(hotkv_context_t *ctx, const char **channels, const size_t *chanlens,
                    size_t nchannels) {
    if (nchannels == 0) return HOTKV_ERR_OTHER;
    return sub_impl(ctx, "SUBSCRIBE", channels, chanlens, nchannels);
}

int hotkv_psubscribe(hotkv_context_t *ctx, const char **patterns, const size_t *patlens,
                     size_t npatterns) {
    if (npatterns == 0) return HOTKV_ERR_OTHER;
    return sub_impl(ctx, "PSUBSCRIBE", patterns, patlens, npatterns);
}

int hotkv_unsubscribe(hotkv_context_t *ctx, const char **channels, const size_t *chanlens,
                      size_t nchannels) {
    return sub_impl(ctx, "UNSUBSCRIBE", channels, chanlens, nchannels);
}

int hotkv_punsubscribe(hotkv_context_t *ctx, const char **patterns, const size_t *patlens,
                       size_t npatterns) {
    return sub_impl(ctx, "PUNSUBSCRIBE", patterns, patlens, npatterns);
}

hotkv_reply_t *hotkv_pubsub_channels(hotkv_context_t *ctx, const char *pattern) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PUBSUB");
    hkv_cmd_adds(&c, "CHANNELS");
    if (pattern) hkv_cmd_adds(&c, pattern);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_pubsub_numsub(hotkv_context_t *ctx, const char **channels,
                                   const size_t *chanlens, size_t nchannels) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PUBSUB");
    hkv_cmd_adds(&c, "NUMSUB");
    add_items(&c, channels, chanlens, nchannels);
    return hkv_cmd_exec(ctx, &c);
}

hotkv_reply_t *hotkv_pubsub_numpat(hotkv_context_t *ctx) {
    hkv_cmd_t c;
    hkv_cmd_init(&c);
    hkv_cmd_adds(&c, "PUBSUB");
    hkv_cmd_adds(&c, "NUMPAT");
    return hkv_cmd_exec(ctx, &c);
}
