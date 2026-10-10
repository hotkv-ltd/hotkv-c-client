/**
 * @file hotkv_commands.c
 * @brief Typed command implementations for all HotKV commands.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * Each function builds a RESP command via format_command_argv and
 * dispatches it through the blocking command path.
 */

#include "hotkv_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Helper: build and execute an argv-based command
 * ========================================================================= */

/* Portable PRId64: MSVC doesn't always have inttypes.h in C11 mode. */
#ifdef _MSC_VER
    #define PRId64_COMPAT "lld"
    #define PRIu64_COMPAT "llu"
#else
    #include <inttypes.h>
    #define PRId64_COMPAT PRId64
    #define PRIu64_COMPAT PRIu64
#endif

/**
 * Small inline helper to format an int64_t into a caller-provided buffer
 * and return the string length. Buffer must be >= 21 bytes.
 */
static size_t i64_to_str(char *buf, size_t bufsz, int64_t val) {
    int n = snprintf(buf, bufsz, "%" PRId64_COMPAT, val);
    return (n > 0) ? (size_t)n : 0;
}

static size_t u64_to_str(char *buf, size_t bufsz, uint64_t val) {
    int n = snprintf(buf, bufsz, "%" PRIu64_COMPAT, val);
    return (n > 0) ? (size_t)n : 0;
}

/* Plain decimal, locale independent, shortest text that reads back as the
 * same double (see hotkv_num.c). Returns 0 for NaN. */
static size_t dbl_to_str(char *buf, size_t bufsz, double val) {
    return hotkv_fmt_double(buf, bufsz, val);
}

static hotkv_reply_t *bad_number(hotkv_context_t *ctx) {
    (void)ctx;
    return hotkv_local_error_reply("ERR invalid numeric argument, nothing was sent");
}

/* =========================================================================
 * String commands
 * ========================================================================= */

hotkv_reply_t *hotkv_set(hotkv_context_t *ctx,
                          const char *key, size_t keylen,
                          const char *val, size_t vallen) {
    const char *argv[] = {"SET", key, val};
    size_t lens[] = {3, keylen, vallen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_get(hotkv_context_t *ctx,
                          const char *key, size_t keylen) {
    const char *argv[] = {"GET", key};
    size_t lens[] = {3, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_del(hotkv_context_t *ctx,
                          const char **keys, const size_t *keylens,
                          size_t nkeys) {
    /* DEL key [key ...] */
    size_t argc = 1 + nkeys;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "DEL"; lens[0] = 3;
    for (size_t i = 0; i < nkeys; i++) {
        argv[1 + i] = keys[i];
        lens[1 + i] = keylens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_mset(hotkv_context_t *ctx,
                           const char **keys, const size_t *keylens,
                           const char **vals, const size_t *vallens,
                           size_t npairs) {
    /* MSET key value [key value ...] */
    size_t argc = 1 + npairs * 2;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "MSET"; lens[0] = 4;
    for (size_t i = 0; i < npairs; i++) {
        argv[1 + i * 2]     = keys[i];  lens[1 + i * 2]     = keylens[i];
        argv[1 + i * 2 + 1] = vals[i];  lens[1 + i * 2 + 1] = vallens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_mget(hotkv_context_t *ctx,
                           const char **keys, const size_t *keylens,
                           size_t nkeys) {
    size_t argc = 1 + nkeys;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "MGET"; lens[0] = 4;
    for (size_t i = 0; i < nkeys; i++) {
        argv[1 + i] = keys[i]; lens[1 + i] = keylens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_incr(hotkv_context_t *ctx,
                           const char *key, size_t keylen) {
    const char *argv[] = {"INCR", key};
    size_t lens[] = {4, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_decr(hotkv_context_t *ctx,
                           const char *key, size_t keylen) {
    const char *argv[] = {"DECR", key};
    size_t lens[] = {4, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_incrby(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             int64_t increment) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), increment);
    const char *argv[] = {"INCRBY", key, buf};
    size_t lens[] = {6, keylen, blen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_decrby(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             int64_t decrement) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), decrement);
    const char *argv[] = {"DECRBY", key, buf};
    size_t lens[] = {6, keylen, blen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_incrbyfloat(hotkv_context_t *ctx,
                                  const char *key, size_t keylen,
                                  double increment) {
    char buf[HOTKV_FMT_NUM_MAX];
    size_t blen = dbl_to_str(buf, sizeof(buf), increment);
    if (blen == 0) return bad_number(ctx);
    const char *argv[] = {"INCRBYFLOAT", key, buf};
    size_t lens[] = {11, keylen, blen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_append(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             const char *val, size_t vallen) {
    const char *argv[] = {"APPEND", key, val};
    size_t lens[] = {6, keylen, vallen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_strlen(hotkv_context_t *ctx,
                             const char *key, size_t keylen) {
    const char *argv[] = {"STRLEN", key};
    size_t lens[] = {6, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_setnx(hotkv_context_t *ctx,
                            const char *key, size_t keylen,
                            const char *val, size_t vallen) {
    const char *argv[] = {"SETNX", key, val};
    size_t lens[] = {5, keylen, vallen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_setex(hotkv_context_t *ctx,
                            const char *key, size_t keylen,
                            int64_t seconds,
                            const char *val, size_t vallen) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), seconds);
    const char *argv[] = {"SETEX", key, buf, val};
    size_t lens[] = {5, keylen, blen, vallen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

hotkv_reply_t *hotkv_psetex(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             int64_t milliseconds,
                             const char *val, size_t vallen) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), milliseconds);
    const char *argv[] = {"PSETEX", key, buf, val};
    size_t lens[] = {6, keylen, blen, vallen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

hotkv_reply_t *hotkv_getdel(hotkv_context_t *ctx,
                             const char *key, size_t keylen) {
    const char *argv[] = {"GETDEL", key};
    size_t lens[] = {6, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_getrange(hotkv_context_t *ctx,
                               const char *key, size_t keylen,
                               int64_t start, int64_t end) {
    char sbuf[24], ebuf[24];
    size_t slen = i64_to_str(sbuf, sizeof(sbuf), start);
    size_t elen = i64_to_str(ebuf, sizeof(ebuf), end);
    const char *argv[] = {"GETRANGE", key, sbuf, ebuf};
    size_t lens[] = {8, keylen, slen, elen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

hotkv_reply_t *hotkv_setrange(hotkv_context_t *ctx,
                               const char *key, size_t keylen,
                               int64_t offset,
                               const char *val, size_t vallen) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), offset);
    const char *argv[] = {"SETRANGE", key, buf, val};
    size_t lens[] = {8, keylen, blen, vallen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

hotkv_reply_t *hotkv_getset(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             const char *val, size_t vallen) {
    const char *argv[] = {"GETSET", key, val};
    size_t lens[] = {6, keylen, vallen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_getex(hotkv_context_t *ctx,
                            const char *key, size_t keylen,
                            const char *expire_type,
                            int64_t expire_value) {
    /* GETEX key [EX s | PX ms | EXAT ts | PXAT ts | PERSIST]. PERSIST takes no
     * argument, so expire_value is ignored for it. */
    const char *argv[4] = {"GETEX", key, NULL, NULL};
    size_t lens[4] = {5, keylen, 0, 0};
    char buf[24];
    int argc = 2;

    if (expire_type && expire_type[0] != '\0') {
        argv[argc] = expire_type;
        lens[argc++] = strlen(expire_type);
        if (strcmp(expire_type, "PERSIST") != 0 && strcmp(expire_type, "persist") != 0) {
            argv[argc] = buf;
            lens[argc++] = i64_to_str(buf, sizeof(buf), expire_value);
        }
    }
    return hotkv_command_argv(ctx, argc, argv, lens);
}

hotkv_reply_t *hotkv_msetnx(hotkv_context_t *ctx,
                             const char **keys, const size_t *keylens,
                             const char **vals, const size_t *vallens,
                             size_t npairs) {
    size_t argc = 1 + npairs * 2;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "MSETNX"; lens[0] = 6;
    for (size_t i = 0; i < npairs; i++) {
        argv[1 + i * 2]     = keys[i];  lens[1 + i * 2]     = keylens[i];
        argv[1 + i * 2 + 1] = vals[i];  lens[1 + i * 2 + 1] = vallens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

/* =========================================================================
 * List commands
 * ========================================================================= */

hotkv_reply_t *hotkv_lpush(hotkv_context_t *ctx,
                            const char *key, size_t keylen,
                            const char **vals, const size_t *vallens,
                            size_t nvals) {
    size_t argc = 2 + nvals;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "LPUSH"; lens[0] = 5;
    argv[1] = key;      lens[1] = keylen;
    for (size_t i = 0; i < nvals; i++) {
        argv[2 + i] = vals[i]; lens[2 + i] = vallens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_rpush(hotkv_context_t *ctx,
                            const char *key, size_t keylen,
                            const char **vals, const size_t *vallens,
                            size_t nvals) {
    size_t argc = 2 + nvals;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "RPUSH"; lens[0] = 5;
    argv[1] = key;      lens[1] = keylen;
    for (size_t i = 0; i < nvals; i++) {
        argv[2 + i] = vals[i]; lens[2 + i] = vallens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_lpop(hotkv_context_t *ctx,
                           const char *key, size_t keylen) {
    const char *argv[] = {"LPOP", key};
    size_t lens[] = {4, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_rpop(hotkv_context_t *ctx,
                           const char *key, size_t keylen) {
    const char *argv[] = {"RPOP", key};
    size_t lens[] = {4, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_llen(hotkv_context_t *ctx,
                           const char *key, size_t keylen) {
    const char *argv[] = {"LLEN", key};
    size_t lens[] = {4, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_lrange(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             int64_t start, int64_t stop) {
    char sbuf[24], ebuf[24];
    size_t slen = i64_to_str(sbuf, sizeof(sbuf), start);
    size_t elen = i64_to_str(ebuf, sizeof(ebuf), stop);
    const char *argv[] = {"LRANGE", key, sbuf, ebuf};
    size_t lens[] = {6, keylen, slen, elen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

hotkv_reply_t *hotkv_lindex(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             int64_t index) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), index);
    const char *argv[] = {"LINDEX", key, buf};
    size_t lens[] = {6, keylen, blen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_lset(hotkv_context_t *ctx,
                           const char *key, size_t keylen,
                           int64_t index,
                           const char *val, size_t vallen) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), index);
    const char *argv[] = {"LSET", key, buf, val};
    size_t lens[] = {4, keylen, blen, vallen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

hotkv_reply_t *hotkv_lrem(hotkv_context_t *ctx,
                           const char *key, size_t keylen,
                           int64_t count,
                           const char *val, size_t vallen) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), count);
    const char *argv[] = {"LREM", key, buf, val};
    size_t lens[] = {4, keylen, blen, vallen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

/* =========================================================================
 * Set commands
 * ========================================================================= */

hotkv_reply_t *hotkv_sadd(hotkv_context_t *ctx,
                           const char *key, size_t keylen,
                           const char **members, const size_t *memlens,
                           size_t nmembers) {
    size_t argc = 2 + nmembers;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "SADD"; lens[0] = 4;
    argv[1] = key;     lens[1] = keylen;
    for (size_t i = 0; i < nmembers; i++) {
        argv[2 + i] = members[i]; lens[2 + i] = memlens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_srem(hotkv_context_t *ctx,
                           const char *key, size_t keylen,
                           const char **members, const size_t *memlens,
                           size_t nmembers) {
    size_t argc = 2 + nmembers;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "SREM"; lens[0] = 4;
    argv[1] = key;     lens[1] = keylen;
    for (size_t i = 0; i < nmembers; i++) {
        argv[2 + i] = members[i]; lens[2 + i] = memlens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_smembers(hotkv_context_t *ctx,
                               const char *key, size_t keylen) {
    const char *argv[] = {"SMEMBERS", key};
    size_t lens[] = {8, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_sismember(hotkv_context_t *ctx,
                                const char *key, size_t keylen,
                                const char *member, size_t memlen) {
    const char *argv[] = {"SISMEMBER", key, member};
    size_t lens[] = {9, keylen, memlen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_scard(hotkv_context_t *ctx,
                            const char *key, size_t keylen) {
    const char *argv[] = {"SCARD", key};
    size_t lens[] = {5, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

/* =========================================================================
 * Hash commands
 * ========================================================================= */

hotkv_reply_t *hotkv_hset(hotkv_context_t *ctx,
                           const char *key, size_t keylen,
                           const char **fields, const size_t *fieldlens,
                           const char **vals, const size_t *vallens,
                           size_t npairs) {
    size_t argc = 2 + npairs * 2;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "HSET"; lens[0] = 4;
    argv[1] = key;     lens[1] = keylen;
    for (size_t i = 0; i < npairs; i++) {
        argv[2 + i * 2]     = fields[i]; lens[2 + i * 2]     = fieldlens[i];
        argv[2 + i * 2 + 1] = vals[i];   lens[2 + i * 2 + 1] = vallens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_hget(hotkv_context_t *ctx,
                           const char *key, size_t keylen,
                           const char *field, size_t fieldlen) {
    const char *argv[] = {"HGET", key, field};
    size_t lens[] = {4, keylen, fieldlen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_hdel(hotkv_context_t *ctx,
                           const char *key, size_t keylen,
                           const char **fields, const size_t *fieldlens,
                           size_t nfields) {
    size_t argc = 2 + nfields;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "HDEL"; lens[0] = 4;
    argv[1] = key;     lens[1] = keylen;
    for (size_t i = 0; i < nfields; i++) {
        argv[2 + i] = fields[i]; lens[2 + i] = fieldlens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_hexists(hotkv_context_t *ctx,
                              const char *key, size_t keylen,
                              const char *field, size_t fieldlen) {
    const char *argv[] = {"HEXISTS", key, field};
    size_t lens[] = {7, keylen, fieldlen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_hlen(hotkv_context_t *ctx,
                           const char *key, size_t keylen) {
    const char *argv[] = {"HLEN", key};
    size_t lens[] = {4, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_hgetall(hotkv_context_t *ctx,
                              const char *key, size_t keylen) {
    const char *argv[] = {"HGETALL", key};
    size_t lens[] = {7, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_hmget(hotkv_context_t *ctx,
                            const char *key, size_t keylen,
                            const char **fields, const size_t *fieldlens,
                            size_t nfields) {
    size_t argc = 2 + nfields;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "HMGET"; lens[0] = 5;
    argv[1] = key;      lens[1] = keylen;
    for (size_t i = 0; i < nfields; i++) {
        argv[2 + i] = fields[i]; lens[2 + i] = fieldlens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_hincrby(hotkv_context_t *ctx,
                              const char *key, size_t keylen,
                              const char *field, size_t fieldlen,
                              int64_t increment) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), increment);
    const char *argv[] = {"HINCRBY", key, field, buf};
    size_t lens[] = {7, keylen, fieldlen, blen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

hotkv_reply_t *hotkv_hincrbyfloat(hotkv_context_t *ctx,
                                    const char *key, size_t keylen,
                                    const char *field, size_t fieldlen,
                                    double increment) {
    char buf[HOTKV_FMT_NUM_MAX];
    size_t blen = dbl_to_str(buf, sizeof(buf), increment);
    if (blen == 0) return bad_number(ctx);
    const char *argv[] = {"HINCRBYFLOAT", key, field, buf};
    size_t lens[] = {12, keylen, fieldlen, blen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

hotkv_reply_t *hotkv_hrandfield(hotkv_context_t *ctx,
                                 const char *key, size_t keylen) {
    const char *argv[] = {"HRANDFIELD", key};
    size_t lens[] = {10, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_hrandfield_count(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       int64_t count, int withvalues) {
    char cbuf[24];
    size_t clen = i64_to_str(cbuf, sizeof(cbuf), count);
    if (withvalues) {
        const char *argv[] = {"HRANDFIELD", key, cbuf, "WITHVALUES"};
        size_t lens[] = {10, keylen, clen, 10};
        return hotkv_command_argv(ctx, 4, argv, lens);
    }
    const char *argv[] = {"HRANDFIELD", key, cbuf};
    size_t lens[] = {10, keylen, clen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

/* =========================================================================
 * Sorted set commands
 * ========================================================================= */

hotkv_reply_t *hotkv_zadd(hotkv_context_t *ctx,
                           const char *key, size_t keylen,
                           const double *scores,
                           const char **members, const size_t *memlens,
                           size_t nmembers) {
    /* ZADD key score member [score member ...] */
    size_t argc = 2 + nmembers * 2;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    char **sbufs = (char **)hotkv_calloc(nmembers ? nmembers : 1, sizeof(char *));
    if (!argv || !lens || !sbufs) {
        hotkv_free(argv); hotkv_free(lens); hotkv_free(sbufs);
        return NULL;
    }

    argv[0] = "ZADD"; lens[0] = 4;
    argv[1] = key;     lens[1] = keylen;
    for (size_t i = 0; i < nmembers; i++) {
        char sb[HOTKV_FMT_NUM_MAX];
        size_t slen = dbl_to_str(sb, sizeof(sb), scores[i]);
        sbufs[i] = slen ? hotkv_strdup(sb) : NULL;
        if (!sbufs[i]) {
            for (size_t k = 0; k < i; k++) hotkv_free(sbufs[k]);
            hotkv_free(argv); hotkv_free(lens); hotkv_free(sbufs);
            return slen ? NULL : bad_number(ctx);
        }
        argv[2 + i * 2]     = sbufs[i];    lens[2 + i * 2]     = slen;
        argv[2 + i * 2 + 1] = members[i];  lens[2 + i * 2 + 1] = memlens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    for (size_t i = 0; i < nmembers; i++) hotkv_free(sbufs[i]);
    hotkv_free(argv);
    hotkv_free(lens);
    hotkv_free(sbufs);
    return reply;
}

hotkv_reply_t *hotkv_zrem(hotkv_context_t *ctx,
                           const char *key, size_t keylen,
                           const char **members, const size_t *memlens,
                           size_t nmembers) {
    size_t argc = 2 + nmembers;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "ZREM"; lens[0] = 4;
    argv[1] = key;     lens[1] = keylen;
    for (size_t i = 0; i < nmembers; i++) {
        argv[2 + i] = members[i]; lens[2 + i] = memlens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_zscore(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             const char *member, size_t memlen) {
    const char *argv[] = {"ZSCORE", key, member};
    size_t lens[] = {6, keylen, memlen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_zrank(hotkv_context_t *ctx,
                            const char *key, size_t keylen,
                            const char *member, size_t memlen) {
    const char *argv[] = {"ZRANK", key, member};
    size_t lens[] = {5, keylen, memlen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_zcard(hotkv_context_t *ctx,
                            const char *key, size_t keylen) {
    const char *argv[] = {"ZCARD", key};
    size_t lens[] = {5, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_zrange(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             int64_t start, int64_t stop,
                             int withscores) {
    char sbuf[24], ebuf[24];
    size_t slen = i64_to_str(sbuf, sizeof(sbuf), start);
    size_t elen = i64_to_str(ebuf, sizeof(ebuf), stop);

    if (withscores) {
        const char *argv[] = {"ZRANGE", key, sbuf, ebuf, "WITHSCORES"};
        size_t lens[] = {6, keylen, slen, elen, 10};
        return hotkv_command_argv(ctx, 5, argv, lens);
    }
    const char *argv[] = {"ZRANGE", key, sbuf, ebuf};
    size_t lens[] = {6, keylen, slen, elen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

hotkv_reply_t *hotkv_zcount(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             const char *min, const char *max) {
    const char *argv[] = {"ZCOUNT", key, min, max};
    size_t lens[] = {6, keylen, strlen(min), strlen(max)};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

static hotkv_reply_t *zpop(hotkv_context_t *ctx, const char *cmd, size_t cmdlen,
                            const char *key, size_t keylen, int64_t count) {
    if (count < 0) {
        const char *argv[] = {cmd, key};
        size_t lens[] = {cmdlen, keylen};
        return hotkv_command_argv(ctx, 2, argv, lens);
    }
    char cbuf[24];
    size_t clen = i64_to_str(cbuf, sizeof(cbuf), count);
    const char *argv[] = {cmd, key, cbuf};
    size_t lens[] = {cmdlen, keylen, clen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_zpopmin(hotkv_context_t *ctx,
                              const char *key, size_t keylen, int64_t count) {
    return zpop(ctx, "ZPOPMIN", 7, key, keylen, count);
}

hotkv_reply_t *hotkv_zpopmax(hotkv_context_t *ctx,
                              const char *key, size_t keylen, int64_t count) {
    return zpop(ctx, "ZPOPMAX", 7, key, keylen, count);
}

hotkv_reply_t *hotkv_zrandmember(hotkv_context_t *ctx,
                                  const char *key, size_t keylen) {
    const char *argv[] = {"ZRANDMEMBER", key};
    size_t lens[] = {11, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_zrandmember_count(hotkv_context_t *ctx,
                                        const char *key, size_t keylen,
                                        int64_t count, int withscores) {
    char cbuf[24];
    size_t clen = i64_to_str(cbuf, sizeof(cbuf), count);
    if (withscores) {
        const char *argv[] = {"ZRANDMEMBER", key, cbuf, "WITHSCORES"};
        size_t lens[] = {11, keylen, clen, 10};
        return hotkv_command_argv(ctx, 4, argv, lens);
    }
    const char *argv[] = {"ZRANDMEMBER", key, cbuf};
    size_t lens[] = {11, keylen, clen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

/* =========================================================================
 * Key commands
 * ========================================================================= */

hotkv_reply_t *hotkv_exists(hotkv_context_t *ctx,
                             const char **keys, const size_t *keylens,
                             size_t nkeys) {
    size_t argc = 1 + nkeys;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "EXISTS"; lens[0] = 6;
    for (size_t i = 0; i < nkeys; i++) {
        argv[1 + i] = keys[i]; lens[1 + i] = keylens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_expire(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             int64_t seconds) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), seconds);
    const char *argv[] = {"EXPIRE", key, buf};
    size_t lens[] = {6, keylen, blen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_pexpire(hotkv_context_t *ctx,
                              const char *key, size_t keylen,
                              int64_t milliseconds) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), milliseconds);
    const char *argv[] = {"PEXPIRE", key, buf};
    size_t lens[] = {7, keylen, blen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_ttl(hotkv_context_t *ctx,
                          const char *key, size_t keylen) {
    const char *argv[] = {"TTL", key};
    size_t lens[] = {3, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_pttl(hotkv_context_t *ctx,
                           const char *key, size_t keylen) {
    const char *argv[] = {"PTTL", key};
    size_t lens[] = {4, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_persist(hotkv_context_t *ctx,
                              const char *key, size_t keylen) {
    const char *argv[] = {"PERSIST", key};
    size_t lens[] = {7, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_type(hotkv_context_t *ctx,
                           const char *key, size_t keylen) {
    const char *argv[] = {"TYPE", key};
    size_t lens[] = {4, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_rename(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             const char *newkey, size_t newkeylen) {
    const char *argv[] = {"RENAME", key, newkey};
    size_t lens[] = {6, keylen, newkeylen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_keys(hotkv_context_t *ctx,
                           const char *pattern, size_t patternlen) {
    const char *argv[] = {"KEYS", pattern};
    size_t lens[] = {4, patternlen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_scan(hotkv_context_t *ctx,
                           uint64_t cursor,
                           const char *pattern, size_t patternlen,
                           int64_t count) {
    char cbuf[24];
    size_t clen = u64_to_str(cbuf, sizeof(cbuf), cursor);

    if (pattern && patternlen > 0 && count > 0) {
        char nbuf[24];
        size_t nlen = i64_to_str(nbuf, sizeof(nbuf), count);
        const char *argv[] = {"SCAN", cbuf, "MATCH", pattern, "COUNT", nbuf};
        size_t lens[] = {4, clen, 5, patternlen, 5, nlen};
        return hotkv_command_argv(ctx, 6, argv, lens);
    } else if (pattern && patternlen > 0) {
        const char *argv[] = {"SCAN", cbuf, "MATCH", pattern};
        size_t lens[] = {4, clen, 5, patternlen};
        return hotkv_command_argv(ctx, 4, argv, lens);
    } else if (count > 0) {
        char nbuf[24];
        size_t nlen = i64_to_str(nbuf, sizeof(nbuf), count);
        const char *argv[] = {"SCAN", cbuf, "COUNT", nbuf};
        size_t lens[] = {4, clen, 5, nlen};
        return hotkv_command_argv(ctx, 4, argv, lens);
    }

    const char *argv[] = {"SCAN", cbuf};
    size_t lens[] = {4, clen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

/* =========================================================================
 * Server commands
 * ========================================================================= */

hotkv_reply_t *hotkv_ping(hotkv_context_t *ctx) {
    const char *argv[] = {"PING"};
    size_t lens[] = {4};
    return hotkv_command_argv(ctx, 1, argv, lens);
}

hotkv_reply_t *hotkv_echo(hotkv_context_t *ctx,
                           const char *msg, size_t msglen) {
    const char *argv[] = {"ECHO", msg};
    size_t lens[] = {4, msglen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_dbsize(hotkv_context_t *ctx) {
    const char *argv[] = {"DBSIZE"};
    size_t lens[] = {6};
    return hotkv_command_argv(ctx, 1, argv, lens);
}

hotkv_reply_t *hotkv_flushdb(hotkv_context_t *ctx) {
    const char *argv[] = {"FLUSHDB"};
    size_t lens[] = {7};
    return hotkv_command_argv(ctx, 1, argv, lens);
}

hotkv_reply_t *hotkv_flushall(hotkv_context_t *ctx) {
    const char *argv[] = {"FLUSHALL"};
    size_t lens[] = {8};
    return hotkv_command_argv(ctx, 1, argv, lens);
}

hotkv_reply_t *hotkv_info(hotkv_context_t *ctx, const char *section) {
    if (section) {
        const char *argv[] = {"INFO", section};
        size_t lens[] = {4, strlen(section)};
        return hotkv_command_argv(ctx, 2, argv, lens);
    }
    const char *argv[] = {"INFO"};
    size_t lens[] = {4};
    return hotkv_command_argv(ctx, 1, argv, lens);
}

hotkv_reply_t *hotkv_select(hotkv_context_t *ctx, int db) {
    char buf[24];
    size_t blen = i64_to_str(buf, sizeof(buf), (int64_t)db);
    const char *argv[] = {"SELECT", buf};
    size_t lens[] = {6, blen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_config_get(hotkv_context_t *ctx,
                                 const char *parameter, size_t paramlen) {
    const char *argv[] = {"CONFIG", "GET", parameter};
    size_t lens[] = {6, 3, paramlen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

/* =========================================================================
 * Transaction commands
 * ========================================================================= */

hotkv_reply_t *hotkv_multi(hotkv_context_t *ctx) {
    const char *argv[] = {"MULTI"};
    size_t lens[] = {5};
    return hotkv_command_argv(ctx, 1, argv, lens);
}

hotkv_reply_t *hotkv_exec(hotkv_context_t *ctx) {
    const char *argv[] = {"EXEC"};
    size_t lens[] = {4};
    return hotkv_command_argv(ctx, 1, argv, lens);
}

hotkv_reply_t *hotkv_discard(hotkv_context_t *ctx) {
    const char *argv[] = {"DISCARD"};
    size_t lens[] = {7};
    return hotkv_command_argv(ctx, 1, argv, lens);
}

/* =========================================================================
 * Pub/Sub
 * ========================================================================= */

hotkv_reply_t *hotkv_publish(hotkv_context_t *ctx,
                              const char *channel, size_t channellen,
                              const char *message, size_t messagelen) {
    const char *argv[] = {"PUBLISH", channel, message};
    size_t lens[] = {7, channellen, messagelen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

/* =========================================================================
 * Stream commands
 * ========================================================================= */

hotkv_reply_t *hotkv_xadd(hotkv_context_t *ctx,
                           const char *key, size_t keylen,
                           const char *id, size_t idlen,
                           const char **fields, const size_t *fieldlens,
                           const char **vals, const size_t *vallens,
                           size_t npairs) {
    size_t argc = 3 + npairs * 2;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "XADD"; lens[0] = 4;
    argv[1] = key;    lens[1] = keylen;
    argv[2] = id;     lens[2] = idlen;
    for (size_t i = 0; i < npairs; i++) {
        argv[3 + i * 2]     = fields[i]; lens[3 + i * 2]     = fieldlens[i];
        argv[3 + i * 2 + 1] = vals[i];   lens[3 + i * 2 + 1] = vallens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_xlen(hotkv_context_t *ctx,
                           const char *key, size_t keylen) {
    const char *argv[] = {"XLEN", key};
    size_t lens[] = {4, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_xrange(hotkv_context_t *ctx,
                             const char *key, size_t keylen,
                             const char *start, size_t startlen,
                             const char *end, size_t endlen,
                             int64_t count) {
    if (count > 0) {
        char cbuf[24];
        size_t clen = i64_to_str(cbuf, sizeof(cbuf), count);
        const char *argv[] = {"XRANGE", key, start, end, "COUNT", cbuf};
        size_t lens[] = {6, keylen, startlen, endlen, 5, clen};
        return hotkv_command_argv(ctx, 6, argv, lens);
    }
    const char *argv[] = {"XRANGE", key, start, end};
    size_t lens[] = {6, keylen, startlen, endlen};
    return hotkv_command_argv(ctx, 4, argv, lens);
}

hotkv_reply_t *hotkv_xread(hotkv_context_t *ctx,
                            const char **keys, const size_t *keylens,
                            const char **ids, const size_t *idlens,
                            size_t nstreams, int64_t count) {
    /* [XREAD] [COUNT n] STREAMS key... id... */
    size_t argc = 1 + (count > 0 ? (size_t)2 : (size_t)0) + 1 + nstreams * 2;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    char cbuf[24];
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    size_t i = 0;
    argv[i] = "XREAD"; lens[i] = 5; i++;
    if (count > 0) {
        size_t clen = i64_to_str(cbuf, sizeof(cbuf), count);
        argv[i] = "COUNT"; lens[i] = 5; i++;
        argv[i] = cbuf;    lens[i] = clen; i++;
    }
    argv[i] = "STREAMS"; lens[i] = 7; i++;
    for (size_t s = 0; s < nstreams; s++) { argv[i] = keys[s]; lens[i] = keylens[s]; i++; }
    for (size_t s = 0; s < nstreams; s++) { argv[i] = ids[s]; lens[i] = idlens[s]; i++; }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_xinfo_stream(hotkv_context_t *ctx,
                                   const char *key, size_t keylen) {
    const char *argv[] = {"XINFO", "STREAM", key};
    size_t lens[] = {5, 6, keylen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

/* =========================================================================
 * HotKV-exclusive: Nanosecond expiration
 * ========================================================================= */

hotkv_reply_t *hotkv_nexpire(hotkv_context_t *ctx,
                              const char *key, size_t keylen,
                              uint64_t nanoseconds) {
    char buf[24];
    size_t blen = u64_to_str(buf, sizeof(buf), nanoseconds);
    const char *argv[] = {"NEXPIRE", key, buf};
    size_t lens[] = {7, keylen, blen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_nttl(hotkv_context_t *ctx,
                           const char *key, size_t keylen) {
    const char *argv[] = {"NTTL", key};
    size_t lens[] = {4, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_nexpireat(hotkv_context_t *ctx,
                                const char *key, size_t keylen,
                                uint64_t ns_timestamp) {
    char buf[24];
    size_t blen = u64_to_str(buf, sizeof(buf), ns_timestamp);
    const char *argv[] = {"NEXPIREAT", key, buf};
    size_t lens[] = {9, keylen, blen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

/* =========================================================================
 * HotKV-exclusive: Tag commands
 * ========================================================================= */

hotkv_reply_t *hotkv_tag_set(hotkv_context_t *ctx,
                              const char *key, size_t keylen,
                              const char **tags, const size_t *taglens,
                              size_t ntags) {
    /* TAG.SET key tag [tag ...] */
    size_t argc = 2 + ntags;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "TAG.SET"; lens[0] = 7;
    argv[1] = key;        lens[1] = keylen;
    for (size_t i = 0; i < ntags; i++) {
        argv[2 + i] = tags[i]; lens[2 + i] = taglens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_tag_rem(hotkv_context_t *ctx,
                              const char *key, size_t keylen,
                              const char **tags, const size_t *taglens,
                              size_t ntags) {
    /* TAG.REM key tag [tag ...] */
    size_t argc = 2 + ntags;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "TAG.REM"; lens[0] = 7;
    argv[1] = key;        lens[1] = keylen;
    for (size_t i = 0; i < ntags; i++) {
        argv[2 + i] = tags[i]; lens[2 + i] = taglens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_tag_get(hotkv_context_t *ctx,
                              const char *key, size_t keylen) {
    const char *argv[] = {"TAG.GET", key};
    size_t lens[] = {7, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_tag_members(hotkv_context_t *ctx,
                                  const char *tag, size_t taglen) {
    const char *argv[] = {"TAG.MEMBERS", tag};
    size_t lens[] = {11, taglen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_tag_inter(hotkv_context_t *ctx,
                                const char **tags, const size_t *taglens,
                                size_t ntags) {
    /* TAG.INTER tag [tag ...] */
    size_t argc = 1 + ntags;
    const char **argv = (const char **)hotkv_malloc(argc * sizeof(const char *));
    size_t *lens = (size_t *)hotkv_malloc(argc * sizeof(size_t));
    if (!argv || !lens) { hotkv_free(argv); hotkv_free(lens); return NULL; }

    argv[0] = "TAG.INTER"; lens[0] = 9;
    for (size_t i = 0; i < ntags; i++) {
        argv[1 + i] = tags[i]; lens[1 + i] = taglens[i];
    }

    hotkv_reply_t *reply = hotkv_command_argv(ctx, (int)argc, argv, lens);
    hotkv_free(argv);
    hotkv_free(lens);
    return reply;
}

hotkv_reply_t *hotkv_tag_count(hotkv_context_t *ctx,
                                const char *key, size_t keylen) {
    const char *argv[] = {"TAG.COUNT", key};
    size_t lens[] = {9, keylen};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_tag_exists(hotkv_context_t *ctx,
                                 const char *key, size_t keylen,
                                 const char *tag, size_t taglen) {
    const char *argv[] = {"TAG.EXISTS", key, tag};
    size_t lens[] = {10, keylen, taglen};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

/* =========================================================================
 * HotKV-exclusive: Monitoring commands
 * ========================================================================= */

hotkv_reply_t *hotkv_hotkeys_top(hotkv_context_t *ctx, int64_t count) {
    if (count > 0) {
        char buf[24];
        size_t blen = i64_to_str(buf, sizeof(buf), count);
        const char *argv[] = {"HOTKEYS", "TOP", buf};
        size_t lens[] = {7, 3, blen};
        return hotkv_command_argv(ctx, 3, argv, lens);
    }
    const char *argv[] = {"HOTKEYS", "TOP"};
    size_t lens[] = {7, 3};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_hotkeys_reset(hotkv_context_t *ctx) {
    const char *argv[] = {"HOTKEYS", "RESET"};
    size_t lens[] = {7, 5};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_latency_latest(hotkv_context_t *ctx) {
    const char *argv[] = {"LATENCY", "LATEST"};
    size_t lens[] = {7, 6};
    return hotkv_command_argv(ctx, 2, argv, lens);
}

hotkv_reply_t *hotkv_latency_history(hotkv_context_t *ctx, const char *cmd) {
    const char *argv[] = {"LATENCY", "HISTORY", cmd};
    size_t lens[] = {7, 7, strlen(cmd)};
    return hotkv_command_argv(ctx, 3, argv, lens);
}

hotkv_reply_t *hotkv_latency_reset(hotkv_context_t *ctx) {
    const char *argv[] = {"LATENCY", "RESET"};
    size_t lens[] = {7, 5};
    return hotkv_command_argv(ctx, 2, argv, lens);
}
