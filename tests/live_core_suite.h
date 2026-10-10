/**
 * @file live_core_suite.h
 * @brief Value-checked live tests for every typed core command wrapper, as a
 * header so test_live_core.c and test_remote.c share one copy. Needs
 * live_util.h included first. Entry point: live_core_run(url, protocol).
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#ifndef HKV_LIVE_CORE_SUITE_H
#define HKV_LIVE_CORE_SUITE_H

/* ------------------------------------------------------------------------- */
static void test_strings(void) {
    char *k = key("s"), *k2 = key("s2"), *k3 = key("s3"), *ctr = key("ctr"), *fl = key("fl");
    char *bin = key("bin");
    const char binval[] = {'a', '\0', 'b', '\r', '\n', 'c'};
    printf("-- strings --\n");

    expect_ok(hotkv_set(ctx, KL(k), "hello", 5), "SET");
    expect_text(hotkv_get(ctx, KL(k)), "hello", "GET");
    expect_nil(hotkv_get(ctx, KL(k2)), "GET missing");
    expect_int(hotkv_append(ctx, KL(k), " world", 6), 11, "APPEND");
    expect_int(hotkv_strlen(ctx, KL(k)), 11, "STRLEN");
    expect_text(hotkv_getrange(ctx, KL(k), 0, 4), "hello", "GETRANGE");
    expect_text(hotkv_getrange(ctx, KL(k), -5, -1), "world", "GETRANGE negative");
    expect_int(hotkv_setrange(ctx, KL(k), 6, "WORLD", 5), 11, "SETRANGE");
    expect_text(hotkv_get(ctx, KL(k)), "hello WORLD", "GET after SETRANGE");
    expect_text(hotkv_getset(ctx, KL(k), "new", 3), "hello WORLD", "GETSET returns old");
    expect_text(hotkv_get(ctx, KL(k)), "new", "GET after GETSET");

    expect_int(hotkv_setnx(ctx, KL(k), "x", 1), 0, "SETNX existing");
    expect_int(hotkv_setnx(ctx, KL(k2), "x", 1), 1, "SETNX new");
    expect_text(hotkv_getdel(ctx, KL(k2)), "x", "GETDEL");
    expect_nil(hotkv_get(ctx, KL(k2)), "GET after GETDEL");

    expect_ok(hotkv_setex(ctx, KL(k3), 100, "ttl", 3), "SETEX");
    {
        hotkv_reply_t *r = hotkv_ttl(ctx, KL(k3));
        CHECK(r && r->type == HOTKV_REPLY_INTEGER && r->integer > 90 && r->integer <= 100,
              "TTL after SETEX");
        hotkv_free_reply(r);
    }
    expect_ok(hotkv_psetex(ctx, KL(k3), 50000, "pttl", 4), "PSETEX");
    {
        hotkv_reply_t *r = hotkv_pttl(ctx, KL(k3));
        CHECK(r && r->type == HOTKV_REPLY_INTEGER && r->integer > 40000 && r->integer <= 50000,
              "PTTL after PSETEX");
        hotkv_free_reply(r);
    }
    expect_text(hotkv_getex(ctx, KL(k3), "EX", 500), "pttl", "GETEX EX returns value");
    {
        hotkv_reply_t *r = hotkv_ttl(ctx, KL(k3));
        CHECK(r && r->type == HOTKV_REPLY_INTEGER && r->integer > 400 && r->integer <= 500,
              "TTL after GETEX EX");
        hotkv_free_reply(r);
    }
    expect_text(hotkv_getex(ctx, KL(k3), "PERSIST", 0), "pttl", "GETEX PERSIST");
    expect_int(hotkv_ttl(ctx, KL(k3)), -1, "TTL after GETEX PERSIST");

    expect_int(hotkv_incr(ctx, KL(ctr)), 1, "INCR");
    expect_int(hotkv_incr(ctx, KL(ctr)), 2, "INCR again");
    expect_int(hotkv_decr(ctx, KL(ctr)), 1, "DECR");
    expect_int(hotkv_incrby(ctx, KL(ctr), 41), 42, "INCRBY");
    expect_int(hotkv_decrby(ctx, KL(ctr), 50), -8, "DECRBY");
    expect_double(hotkv_incrbyfloat(ctx, KL(fl), 1.5), 1.5, 1e-9, "INCRBYFLOAT");
    expect_double(hotkv_incrbyfloat(ctx, KL(fl), 0.25), 1.75, 1e-9, "INCRBYFLOAT again");
    expect_double(hotkv_incrbyfloat(ctx, KL(fl), -1e-5), 1.74999, 1e-9, "INCRBYFLOAT tiny negative");
    expect_double(hotkv_incrbyfloat(ctx, KL(fl), 1e15), 1e15 + 1.74999, 1e-3, "INCRBYFLOAT huge");

    {
        const char *ks[] = {k, key("m2"), key("m3")};
        size_t kl[3];
        const char *vs[] = {"v1", "v2", "v3"};
        size_t vl[] = {2, 2, 2};
        hotkv_reply_t *r;
        lens_of(ks, 3, kl);
        expect_ok(hotkv_mset(ctx, ks, kl, vs, vl, 3), "MSET");
        r = hotkv_mget(ctx, ks, kl, 3);
        CHECK(is_agg(r) && r->nelem == 3 && strcmp(rtext(elem(r, 0)), "v1") == 0 &&
                  strcmp(rtext(elem(r, 2)), "v3") == 0,
              "MGET values");
        hotkv_free_reply(r);
        {
            const char *mk[] = {key("m2"), key("nx1")};
            size_t mkl[2];
            const char *mv[] = {"a", "b"};
            size_t mvl[] = {1, 1};
            lens_of(mk, 2, mkl);
            expect_int(hotkv_msetnx(ctx, mk, mkl, mv, mvl, 2), 0, "MSETNX with an existing key");
            mk[0] = key("nx0");
            lens_of(mk, 2, mkl);
            expect_int(hotkv_msetnx(ctx, mk, mkl, mv, mvl, 2), 1, "MSETNX all new");
        }
        {
            const char *dk[] = {key("m2"), key("m3")};
            size_t dkl[2];
            lens_of(dk, 2, dkl);
            expect_int(hotkv_del(ctx, dk, dkl, 2), 2, "DEL two keys");
            expect_int(hotkv_del(ctx, dk, dkl, 2), 0, "DEL again");
        }
    }

    expect_ok(hotkv_set(ctx, KL(bin), binval, sizeof(binval)), "SET binary");
    expect_bin(hotkv_get(ctx, KL(bin)), binval, sizeof(binval), "GET binary round trip");
}

/* ------------------------------------------------------------------------- */
static void test_keys(void) {
    char *a = key("k-a"), *b = key("k-b"), *c = key("k-c"), *d = key("k-d"), *e = key("k-e");
    printf("-- keys --\n");

    expect_ok(hotkv_set(ctx, KL(a), "1", 1), "SET a");
    {
        const char *ks[] = {a, b};
        size_t kl[2];
        lens_of(ks, 2, kl);
        expect_int(hotkv_exists(ctx, ks, kl, 2), 1, "EXISTS one of two");
        expect_int(hotkv_touch(ctx, ks, kl, 2), 1, "TOUCH");
    }
    expect_text(hotkv_type(ctx, KL(a)), "string", "TYPE string");
    expect_text(hotkv_type(ctx, KL(b)), "none", "TYPE none");
    expect_int(hotkv_ttl(ctx, KL(a)), -1, "TTL no expiry");
    expect_int(hotkv_ttl(ctx, KL(b)), -2, "TTL missing key");
    expect_int(hotkv_expire(ctx, KL(a), 1000), 1, "EXPIRE");
    expect_int(hotkv_expire(ctx, KL(b), 1000), 0, "EXPIRE missing");
    {
        hotkv_reply_t *r = hotkv_ttl(ctx, KL(a));
        CHECK(r && r->integer > 990 && r->integer <= 1000, "TTL after EXPIRE");
        hotkv_free_reply(r);
    }
    expect_int(hotkv_persist(ctx, KL(a)), 1, "PERSIST");
    expect_int(hotkv_persist(ctx, KL(a)), 0, "PERSIST again");
    expect_int(hotkv_pexpire(ctx, KL(a), 90000), 1, "PEXPIRE");
    {
        hotkv_reply_t *r = hotkv_pttl(ctx, KL(a));
        CHECK(r && r->integer > 80000 && r->integer <= 90000, "PTTL after PEXPIRE");
        hotkv_free_reply(r);
    }
    {
        int64_t at = (int64_t)time(NULL) + 3000;
        expect_int(hotkv_expireat(ctx, KL(a), at), 1, "EXPIREAT");
        expect_int(hotkv_expiretime(ctx, KL(a)), at, "EXPIRETIME");
        expect_int(hotkv_pexpireat(ctx, KL(a), at * 1000 + 500), 1, "PEXPIREAT");
        expect_int(hotkv_pexpiretime(ctx, KL(a)), at * 1000 + 500, "PEXPIRETIME");
        expect_int(hotkv_persist(ctx, KL(a)), 1, "PERSIST after PEXPIREAT");
        expect_int(hotkv_expiretime(ctx, KL(a)), -1, "EXPIRETIME no expiry");
        expect_int(hotkv_expiretime(ctx, KL(b)), -2, "EXPIRETIME missing");
    }

    expect_ok(hotkv_rename(ctx, KL(a), KL(b)), "RENAME");
    expect_text(hotkv_get(ctx, KL(b)), "1", "GET renamed");
    expect_nil(hotkv_get(ctx, KL(a)), "GET old name");
    expect_ok(hotkv_set(ctx, KL(c), "c", 1), "SET c");
    expect_int(hotkv_renamenx(ctx, KL(b), KL(c)), 0, "RENAMENX onto existing");
    expect_int(hotkv_renamenx(ctx, KL(b), KL(d)), 1, "RENAMENX");
    expect_int(hotkv_copy(ctx, KL(d), KL(e), 0), 1, "COPY");
    expect_text(hotkv_get(ctx, KL(e)), "1", "GET copy");
    expect_int(hotkv_copy(ctx, KL(d), KL(c), 0), 0, "COPY onto existing without REPLACE");
    expect_int(hotkv_copy(ctx, KL(d), KL(c), 1), 1, "COPY REPLACE");
    expect_text(hotkv_get(ctx, KL(c)), "1", "GET after COPY REPLACE");

    {
        hotkv_reply_t *dump = hotkv_dump(ctx, KL(d));
        char *copy = NULL;
        size_t n = 0;
        CHECK(dump && dump->type == HOTKV_REPLY_BULK && dump->len > 0, "DUMP");
        if (dump && dump->type == HOTKV_REPLY_BULK) {
            char *r2 = key("restored");
            copy = (char *)malloc(dump->len);
            memcpy(copy, dump->str, dump->len);
            n = dump->len;
            expect_ok(hotkv_restore(ctx, KL(r2), 0, copy, n, 0), "RESTORE");
            expect_text(hotkv_get(ctx, KL(r2)), "1", "GET restored");
            expect_err(hotkv_restore(ctx, KL(r2), 0, copy, n, 0), "BUSYKEY", "RESTORE onto existing");
            expect_ok(hotkv_restore(ctx, KL(r2), 0, copy, n, 1), "RESTORE REPLACE");
            free(copy);
        }
        hotkv_free_reply(dump);
    }

    {
        char *sc = key("scanme");
        size_t cursors = 0;
        uint64_t cur = 0;
        int found = 0;
        expect_ok(hotkv_set(ctx, KL(sc), "1", 1), "SET scan key");
        do {
            hotkv_reply_t *r = hotkv_scan(ctx, cur, sc, strlen(sc), 1000);
            const hotkv_reply_t *keys;
            if (!is_agg(r) || r->nelem != 2) {
                fail("SCAN", "unexpected reply shape");
                hotkv_free_reply(r);
                break;
            }
            cur = (uint64_t)strtoull(rtext(elem(r, 0)) ? rtext(elem(r, 0)) : "0", NULL, 10);
            keys = elem(r, 1);
            for (size_t i = 0; keys && i < keys->nelem; i++) {
                if (rtext(keys->elements[i]) && strcmp(rtext(keys->elements[i]), sc) == 0) found = 1;
            }
            hotkv_free_reply(r);
        } while (cur != 0 && ++cursors < 10000);
        CHECK(found, "SCAN MATCH finds the key");
    }
    expect_strs(hotkv_keys(ctx, KL(e)), 1, "KEYS exact", e);
    {
        hotkv_reply_t *r = hotkv_randomkey(ctx);
        CHECK(r && r->type == HOTKV_REPLY_BULK, "RANDOMKEY returns a key");
        hotkv_free_reply(r);
    }
    {
        const char *ks[] = {c, d, e};
        size_t kl[3];
        lens_of(ks, 3, kl);
        expect_int(hotkv_unlink(ctx, ks, kl, 3), 3, "UNLINK");
        expect_int(hotkv_exists(ctx, ks, kl, 3), 0, "EXISTS after UNLINK");
    }
    {
        char *mv = key("mv");
        expect_ok(hotkv_set(ctx, KL(mv), "x", 1), "SET mv");
        expect_int(hotkv_move(ctx, KL(mv), 9), 1, "MOVE to db 9");
        expect_int(hotkv_exists(ctx, ARR(mv), (size_t[]){strlen(mv)}, 1), 0, "gone from this db");
        expect_ok(hotkv_select(ctx, 9), "SELECT 9");
        expect_text(hotkv_get(ctx, KL(mv)), "x", "GET in db 9");
        expect_int(hotkv_del(ctx, ARR(mv), (size_t[]){strlen(mv)}, 1), 1, "DEL in db 9");
        expect_ok(hotkv_select(ctx, 0), "SELECT 0");
    }
}

/* ------------------------------------------------------------------------- */
static void test_lists(void) {
    char *l = key("l"), *l2 = key("l2"), *nl = key("nolist");
    const char *v3[] = {"a", "b", "c"};
    size_t v3l[] = {1, 1, 1};
    printf("-- lists --\n");

    expect_int(hotkv_rpush(ctx, KL(l), v3, v3l, 3), 3, "RPUSH");
    expect_int(hotkv_lpush(ctx, KL(l), ARR("z"), (size_t[]){1}, 1), 4, "LPUSH");
    expect_int(hotkv_llen(ctx, KL(l)), 4, "LLEN");
    expect_strs(hotkv_lrange(ctx, KL(l), 0, -1), 4, "LRANGE", "z", "a", "b", "c");
    expect_text(hotkv_lindex(ctx, KL(l), 1), "a", "LINDEX");
    expect_text(hotkv_lindex(ctx, KL(l), -1), "c", "LINDEX -1");
    expect_nil(hotkv_lindex(ctx, KL(l), 99), "LINDEX out of range");
    expect_ok(hotkv_lset(ctx, KL(l), 0, "Z", 1), "LSET");
    expect_int(hotkv_linsert(ctx, KL(l), 1, "a", 1, "pre", 3), 5, "LINSERT BEFORE");
    expect_int(hotkv_linsert(ctx, KL(l), 0, "a", 1, "post", 4), 6, "LINSERT AFTER");
    expect_int(hotkv_linsert(ctx, KL(l), 1, "nope", 4, "x", 1), -1, "LINSERT missing pivot");
    expect_strs(hotkv_lrange(ctx, KL(l), 0, -1), 6, "LRANGE after LINSERT", "Z", "pre", "a", "post",
                "b", "c");
    expect_int(hotkv_lpos(ctx, KL(l), "a", 1, 0, -1, -1), 2, "LPOS");
    expect_nil(hotkv_lpos(ctx, KL(l), "nope", 4, 0, -1, -1), "LPOS missing");
    expect_ints(hotkv_lpos(ctx, KL(l), "a", 1, 0, 0, -1), 1, "LPOS COUNT 0", 2LL);
    expect_int(hotkv_lrem(ctx, KL(l), 0, "pre", 3), 1, "LREM");
    expect_ok(hotkv_ltrim(ctx, KL(l), 1, -1), "LTRIM");
    expect_strs(hotkv_lrange(ctx, KL(l), 0, -1), 4, "LRANGE after LTRIM", "a", "post", "b", "c");
    expect_text(hotkv_lpop(ctx, KL(l)), "a", "LPOP");
    expect_text(hotkv_rpop(ctx, KL(l)), "c", "RPOP");
    expect_strs(hotkv_lpop_count(ctx, KL(l), 2), 2, "LPOP count", "post", "b");
    expect_nil(hotkv_lpop(ctx, KL(l)), "LPOP empty");
    expect_int(hotkv_lpushx(ctx, KL(nl), v3, v3l, 3), 0, "LPUSHX missing key");
    expect_int(hotkv_rpushx(ctx, KL(nl), v3, v3l, 3), 0, "RPUSHX missing key");
    expect_int(hotkv_rpush(ctx, KL(l), v3, v3l, 3), 3, "RPUSH again");
    expect_int(hotkv_lpushx(ctx, KL(l), ARR("0"), (size_t[]){1}, 1), 4, "LPUSHX existing");
    expect_int(hotkv_rpushx(ctx, KL(l), ARR("d"), (size_t[]){1}, 1), 5, "RPUSHX existing");
    expect_text(hotkv_lmove(ctx, KL(l), KL(l2), "LEFT", "RIGHT"), "0", "LMOVE");
    expect_text(hotkv_rpoplpush(ctx, KL(l), KL(l2)), "d", "RPOPLPUSH");
    expect_strs(hotkv_lrange(ctx, KL(l2), 0, -1), 2, "LRANGE destination", "d", "0");
    expect_strs(hotkv_rpop_count(ctx, KL(l2), 2), 2, "RPOP count", "0", "d");
}

/* ------------------------------------------------------------------------- */
static void test_sets(void) {
    char *a = key("set-a"), *b = key("set-b"), *dst = key("set-dst"), *big = key("set-big");
    const char *am[] = {"1", "2", "3", "4"};
    const char *bm[] = {"3", "4", "5"};
    size_t al[] = {1, 1, 1, 1}, bl[] = {1, 1, 1};
    const char *ks[2];
    size_t kl[2];
    printf("-- sets --\n");

    expect_int(hotkv_sadd(ctx, KL(a), am, al, 4), 4, "SADD");
    expect_int(hotkv_sadd(ctx, KL(a), am, al, 4), 0, "SADD duplicates");
    expect_int(hotkv_sadd(ctx, KL(b), bm, bl, 3), 3, "SADD b");
    expect_int(hotkv_scard(ctx, KL(a)), 4, "SCARD");
    expect_int(hotkv_sismember(ctx, KL(a), "2", 1), 1, "SISMEMBER yes");
    expect_int(hotkv_sismember(ctx, KL(a), "9", 1), 0, "SISMEMBER no");
    expect_ints(hotkv_smismember(ctx, KL(a), ARR("1", "9", "4"), (size_t[]){1, 1, 1}, 3), 3,
                "SMISMEMBER", 1LL, 0LL, 1LL);
    expect_strset(hotkv_smembers(ctx, KL(a)), 4, "SMEMBERS", "1", "2", "3", "4");
    ks[0] = a; ks[1] = b; lens_of(ks, 2, kl);
    expect_strset(hotkv_sdiff(ctx, ks, kl, 2), 2, "SDIFF", "1", "2");
    expect_strset(hotkv_sinter(ctx, ks, kl, 2), 2, "SINTER", "3", "4");
    expect_strset(hotkv_sunion(ctx, ks, kl, 2), 5, "SUNION", "1", "2", "3", "4", "5");
    expect_int(hotkv_sdiffstore(ctx, KL(dst), ks, kl, 2), 2, "SDIFFSTORE");
    expect_int(hotkv_sinterstore(ctx, KL(dst), ks, kl, 2), 2, "SINTERSTORE");
    expect_strset(hotkv_smembers(ctx, KL(dst)), 2, "SMEMBERS stored inter", "3", "4");
    expect_int(hotkv_sunionstore(ctx, KL(dst), ks, kl, 2), 5, "SUNIONSTORE");
    expect_int(hotkv_sintercard(ctx, ks, kl, 2, -1), 2, "SINTERCARD");
    expect_int(hotkv_sintercard(ctx, ks, kl, 2, 1), 1, "SINTERCARD LIMIT 1");
    expect_int(hotkv_smove(ctx, KL(a), KL(b), "1", 1), 1, "SMOVE");
    expect_int(hotkv_smove(ctx, KL(a), KL(b), "99", 2), 0, "SMOVE missing member");
    expect_int(hotkv_sismember(ctx, KL(b), "1", 1), 1, "moved member is in b");
    expect_int(hotkv_srem(ctx, KL(a), ARR("2", "nope"), (size_t[]){1, 4}, 2), 1, "SREM");
    {
        hotkv_reply_t *r = hotkv_spop(ctx, KL(a));
        CHECK(r && r->type == HOTKV_REPLY_BULK, "SPOP one");
        hotkv_free_reply(r);
        expect_int(hotkv_scard(ctx, KL(a)), 1, "SCARD after SPOP");
        r = hotkv_srandmember(ctx, KL(a));
        CHECK(r && r->type == HOTKV_REPLY_BULK, "SRANDMEMBER one");
        hotkv_free_reply(r);
        r = hotkv_srandmember_count(ctx, KL(b), 3);
        CHECK(is_agg(r) && r->nelem == 3, "SRANDMEMBER count 3");
        hotkv_free_reply(r);
        r = hotkv_srandmember_count(ctx, KL(b), -7);
        CHECK(is_agg(r) && r->nelem == 7, "SRANDMEMBER negative count repeats");
        hotkv_free_reply(r);
        r = hotkv_spop_count(ctx, KL(b), 2);
        CHECK(is_agg(r) && r->nelem == 2, "SPOP count");
        hotkv_free_reply(r);
        expect_int(hotkv_scard(ctx, KL(b)), 2, "SCARD after SPOP count");
    }
    {
        char m[16];
        uint64_t cur = 0;
        int seen = 0, rounds = 0;
        for (int i = 0; i < 50; i++) {
            snprintf(m, sizeof(m), "m%d", i);
            hotkv_free_reply(hotkv_sadd(ctx, KL(big), ARR(m), (size_t[]){strlen(m)}, 1));
        }
        do {
            hotkv_reply_t *r = hotkv_sscan(ctx, KL(big), cur, "m*", 20);
            if (!is_agg(r) || r->nelem != 2) { fail("SSCAN", "bad shape"); hotkv_free_reply(r); break; }
            cur = strtoull(rtext(elem(r, 0)), NULL, 10);
            seen += (int)(elem(r, 1) ? elem(r, 1)->nelem : 0);
            hotkv_free_reply(r);
        } while (cur != 0 && ++rounds < 1000);
        CHECK(seen >= 50, "SSCAN visits every member");
    }
}

/* ------------------------------------------------------------------------- */
static void test_hashes(void) {
    char *h = key("h"), *h2 = key("h2");
    const char *f[] = {"f1", "f2", "f3"};
    size_t fl[] = {2, 2, 2};
    const char *v[] = {"v1", "v2", "3"};
    size_t vl[] = {2, 2, 1};
    printf("-- hashes --\n");

    expect_int(hotkv_hset(ctx, KL(h), f, fl, v, vl, 3), 3, "HSET");
    expect_int(hotkv_hset(ctx, KL(h), f, fl, v, vl, 3), 0, "HSET existing fields");
    expect_text(hotkv_hget(ctx, KL(h), "f1", 2), "v1", "HGET");
    expect_nil(hotkv_hget(ctx, KL(h), "nope", 4), "HGET missing");
    expect_int(hotkv_hexists(ctx, KL(h), "f2", 2), 1, "HEXISTS");
    expect_int(hotkv_hexists(ctx, KL(h), "zz", 2), 0, "HEXISTS no");
    expect_int(hotkv_hlen(ctx, KL(h)), 3, "HLEN");
    expect_int(hotkv_hstrlen(ctx, KL(h), "f1", 2), 2, "HSTRLEN");
    expect_strset(hotkv_hkeys(ctx, KL(h)), 3, "HKEYS", "f1", "f2", "f3");
    expect_strset(hotkv_hvals(ctx, KL(h)), 3, "HVALS", "v1", "v2", "3");
    expect_strset(hotkv_hgetall(ctx, KL(h)), 6, "HGETALL", "f1", "v1", "f2", "v2", "f3", "3");
    {
        hotkv_reply_t *r = hotkv_hmget(ctx, KL(h), ARR("f2", "nope", "f1"), (size_t[]){2, 4, 2}, 3);
        CHECK(is_agg(r) && r->nelem == 3 && strcmp(rtext(elem(r, 0)), "v2") == 0 &&
                  elem(r, 1)->type == HOTKV_REPLY_NIL && strcmp(rtext(elem(r, 2)), "v1") == 0,
              "HMGET (nil for the missing field)");
        hotkv_free_reply(r);
    }
    expect_int(hotkv_hincrby(ctx, KL(h), "f3", 2, 10), 13, "HINCRBY");
    expect_double(hotkv_hincrbyfloat(ctx, KL(h), "f3", 2, 0.5), 13.5, 1e-9, "HINCRBYFLOAT");
    expect_double(hotkv_hincrbyfloat(ctx, KL(h), "f3", 2, 1e-5), 13.50001, 1e-9,
                  "HINCRBYFLOAT tiny");
    expect_int(hotkv_hsetnx(ctx, KL(h), "f1", 2, "x", 1), 0, "HSETNX existing");
    expect_int(hotkv_hsetnx(ctx, KL(h), "f4", 2, "x", 1), 1, "HSETNX new");
    expect_ok(hotkv_hmset(ctx, KL(h2), ARR("a", "b"), (size_t[]){1, 1}, ARR("1", "2"),
                          (size_t[]){1, 1}, 2), "HMSET");
    expect_int(hotkv_hlen(ctx, KL(h2)), 2, "HLEN after HMSET");
    {
        hotkv_reply_t *r = hotkv_hrandfield(ctx, KL(h2));
        CHECK(r && r->type == HOTKV_REPLY_BULK, "HRANDFIELD");
        hotkv_free_reply(r);
        r = hotkv_hrandfield_count(ctx, KL(h2), 2, 1);
        CHECK(hotkv_reply_pair_count(r) == 2, "HRANDFIELD WITHVALUES pairs");
        hotkv_free_reply(r);
        r = hotkv_hrandfield_count(ctx, KL(h2), -5, 0);
        CHECK(is_agg(r) && r->nelem == 5, "HRANDFIELD negative count");
        hotkv_free_reply(r);
    }
    {
        uint64_t cur = 0;
        int pairs = 0, rounds = 0;
        do {
            hotkv_reply_t *r = hotkv_hscan(ctx, KL(h), cur, NULL, 100);
            if (!is_agg(r) || r->nelem != 2) { fail("HSCAN", "bad shape"); hotkv_free_reply(r); break; }
            cur = strtoull(rtext(elem(r, 0)), NULL, 10);
            pairs += (int)(elem(r, 1) ? elem(r, 1)->nelem / 2 : 0);
            hotkv_free_reply(r);
        } while (cur != 0 && ++rounds < 100);
        CHECK(pairs == 4, "HSCAN returns the 4 pairs");
    }
    /* Field-level expiry (HotKV). */
    expect_ints(hotkv_hexpire(ctx, KL(h), 1000, NULL, ARR("f1", "nope"), (size_t[]){2, 4}, 2), 2,
                "HEXPIRE", 1LL, -2LL);
    {
        hotkv_reply_t *r = hotkv_httl(ctx, KL(h), ARR("f1", "f2", "nope"), (size_t[]){2, 2, 4}, 3);
        long long a0 = 0, a1 = 0, a2 = 0;
        CHECK(is_agg(r) && r->nelem == 3 && rint64(elem(r, 0), &a0) && a0 > 900 && a0 <= 1000 &&
                  rint64(elem(r, 1), &a1) && a1 == -1 && rint64(elem(r, 2), &a2) && a2 == -2,
              "HTTL");
        hotkv_free_reply(r);
    }
    expect_ints(hotkv_hpexpire(ctx, KL(h), 800000, "XX", ARR("f1"), (size_t[]){2}, 1), 1,
                "HPEXPIRE XX", 1LL);
    {
        hotkv_reply_t *r = hotkv_hpttl(ctx, KL(h), ARR("f1"), (size_t[]){2}, 1);
        long long t = 0;
        CHECK(is_agg(r) && rint64(elem(r, 0), &t) && t > 700000 && t <= 800000, "HPTTL");
        hotkv_free_reply(r);
        r = hotkv_hexpiretime(ctx, KL(h), ARR("f1", "f2"), (size_t[]){2, 2}, 2);
        t = 0;
        CHECK(is_agg(r) && rint64(elem(r, 0), &t) && t > (long long)time(NULL), "HEXPIRETIME");
        CHECK(is_agg(r) && rint64(elem(r, 1), &t) && t == -1, "HEXPIRETIME no ttl");
        hotkv_free_reply(r);
        r = hotkv_hpexpiretime(ctx, KL(h), ARR("f1"), (size_t[]){2}, 1);
        t = 0;
        CHECK(is_agg(r) && rint64(elem(r, 0), &t) && t > (long long)time(NULL) * 1000, "HPEXPIRETIME");
        hotkv_free_reply(r);
    }
    expect_ints(hotkv_hpersist(ctx, KL(h), ARR("f1", "f2"), (size_t[]){2, 2}, 2), 2, "HPERSIST",
                1LL, -1LL);
    {
        hotkv_reply_t *r = hotkv_hgetdel(ctx, KL(h), ARR("f1", "nope"), (size_t[]){2, 4}, 2);
        CHECK(is_agg(r) && r->nelem == 2 && strcmp(rtext(elem(r, 0)), "v1") == 0 &&
                  elem(r, 1)->type == HOTKV_REPLY_NIL,
              "HGETDEL (value, nil for the missing field)");
        hotkv_free_reply(r);
    }
    expect_int(hotkv_hexists(ctx, KL(h), "f1", 2), 0, "field gone after HGETDEL");
    expect_int(hotkv_hdel(ctx, KL(h), ARR("f2", "f3", "nope"), (size_t[]){2, 2, 4}, 3), 2, "HDEL");
}

/* ------------------------------------------------------------------------- */
static void test_zsets(void) {
    char *z = key("z"), *z2 = key("z2"), *zd = key("zdst"), *zl = key("zlex");
    const double sc[] = {1, 2, 3, 4};
    const char *mm[] = {"a", "b", "c", "d"};
    size_t ml[] = {1, 1, 1, 1};
    printf("-- sorted sets --\n");

    expect_int(hotkv_zadd(ctx, KL(z), sc, mm, ml, 4), 4, "ZADD");
    expect_int(hotkv_zadd(ctx, KL(z), sc, mm, ml, 4), 0, "ZADD existing");
    expect_int(hotkv_zcard(ctx, KL(z)), 4, "ZCARD");
    expect_double(hotkv_zscore(ctx, KL(z), "c", 1), 3.0, 1e-12, "ZSCORE");
    expect_nil(hotkv_zscore(ctx, KL(z), "nope", 4), "ZSCORE missing");
    expect_int(hotkv_zrank(ctx, KL(z), "c", 1), 2, "ZRANK");
    expect_int(hotkv_zrevrank(ctx, KL(z), "c", 1), 1, "ZREVRANK");
    expect_nil(hotkv_zrank(ctx, KL(z), "nope", 4), "ZRANK missing");
    expect_strs(hotkv_zrange(ctx, KL(z), 0, -1, 0), 4, "ZRANGE", "a", "b", "c", "d");
    expect_strs(hotkv_zrevrange(ctx, KL(z), 0, 1, 0), 2, "ZREVRANGE", "d", "c");
    {
        hotkv_reply_t *r = hotkv_zrange(ctx, KL(z), 0, 1, 1);
        hotkv_reply_t *k0 = NULL, *v0 = NULL;
        double d0 = 0;
        CHECK(hotkv_reply_pair_count(r) == 2, "ZRANGE WITHSCORES pairs");
        CHECK(hotkv_reply_pair_at(r, 1, &k0, &v0) && strcmp(rtext(k0), "b") == 0 &&
                  rdouble(v0, &d0) && d0 == 2.0,
              "ZRANGE WITHSCORES pair 1");
        hotkv_free_reply(r);
        r = hotkv_zrevrange(ctx, KL(z), 0, 0, 1);
        CHECK(hotkv_reply_pair_count(r) == 1 && hotkv_reply_pair_at(r, 0, &k0, &v0) &&
                  strcmp(rtext(k0), "d") == 0 && rdouble(v0, &d0) && d0 == 4.0,
              "ZREVRANGE WITHSCORES");
        hotkv_free_reply(r);
    }
    expect_int(hotkv_zcount(ctx, KL(z), "2", "3"), 2, "ZCOUNT");
    expect_int(hotkv_zcount(ctx, KL(z), "(2", "+inf"), 2, "ZCOUNT exclusive");
    expect_strs(hotkv_zrangebyscore(ctx, KL(z), "2", "3", 0, 0, -1), 2, "ZRANGEBYSCORE", "b", "c");
    expect_strs(hotkv_zrangebyscore(ctx, KL(z), "-inf", "+inf", 0, 1, 2), 2,
                "ZRANGEBYSCORE LIMIT", "b", "c");
    expect_strs(hotkv_zrevrangebyscore(ctx, KL(z), "3", "2", 0, 0, -1), 2, "ZREVRANGEBYSCORE", "c",
                "b");
    expect_double(hotkv_zincrby(ctx, KL(z), 2.5, "a", 1), 3.5, 1e-12, "ZINCRBY");
    expect_double(hotkv_zincrby(ctx, KL(z), -1e-5, "a", 1), 3.49999, 1e-9, "ZINCRBY tiny");
    {
        hotkv_reply_t *r = hotkv_zmscore(ctx, KL(z), ARR("b", "nope"), (size_t[]){1, 4}, 2);
        double d = 0;
        CHECK(is_agg(r) && r->nelem == 2 && rdouble(elem(r, 0), &d) && d == 2.0 &&
                  elem(r, 1)->type == HOTKV_REPLY_NIL,
              "ZMSCORE");
        hotkv_free_reply(r);
    }
    /* set operations */
    expect_int(hotkv_zadd(ctx, KL(z2), (double[]){10, 20, 30}, ARR("b", "c", "x"), (size_t[]){1, 1, 1}, 3), 3,
               "ZADD z2");
    {
        const char *ks[] = {z, z2};
        size_t kl[2];
        double w[] = {1, 2};
        lens_of(ks, 2, kl);
        expect_int(hotkv_zunionstore(ctx, KL(zd), ks, kl, 2, NULL, NULL), 5, "ZUNIONSTORE");
        expect_double(hotkv_zscore(ctx, KL(zd), "b", 1), 12.0, 1e-9, "ZUNIONSTORE sums scores");
        expect_int(hotkv_zunionstore(ctx, KL(zd), ks, kl, 2, w, "MAX"), 5, "ZUNIONSTORE WEIGHTS MAX");
        expect_double(hotkv_zscore(ctx, KL(zd), "b", 1), 20.0, 1e-9, "weighted max");
        expect_int(hotkv_zinterstore(ctx, KL(zd), ks, kl, 2, NULL, "MIN"), 2, "ZINTERSTORE MIN");
        expect_double(hotkv_zscore(ctx, KL(zd), "c", 1), 3.0, 1e-9, "inter min");
        expect_strs(hotkv_zinter(ctx, ks, kl, 2, NULL, NULL, 0), 2, "ZINTER", "b", "c");
        expect_strset(hotkv_zunion(ctx, ks, kl, 2, NULL, NULL, 0), 5, "ZUNION", "a", "b", "c", "d", "x");
        {
            hotkv_reply_t *r = hotkv_zunion(ctx, ks, kl, 2, NULL, NULL, 1);
            CHECK(hotkv_reply_pair_count(r) == 5, "ZUNION WITHSCORES");
            hotkv_free_reply(r);
        }
        expect_strset(hotkv_zdiff(ctx, ks, kl, 2, 0), 2, "ZDIFF", "a", "d");
        expect_int(hotkv_zdiffstore(ctx, KL(zd), ks, kl, 2), 2, "ZDIFFSTORE");
        expect_int(hotkv_zintercard(ctx, ks, kl, 2, -1), 2, "ZINTERCARD");
        expect_int(hotkv_zintercard(ctx, ks, kl, 2, 1), 1, "ZINTERCARD LIMIT");
    }
    /* lexicographic: all score 0 */
    expect_int(hotkv_zadd(ctx, KL(zl), (double[]){0, 0, 0, 0}, ARR("a", "b", "c", "d"),
                          (size_t[]){1, 1, 1, 1}, 4), 4, "ZADD lex");
    expect_strs(hotkv_zrangebylex(ctx, KL(zl), "[b", "[c", -1, -1), 2, "ZRANGEBYLEX", "b", "c");
    expect_strs(hotkv_zrevrangebylex(ctx, KL(zl), "[c", "[b", -1, -1), 2, "ZREVRANGEBYLEX", "c", "b");
    expect_int(hotkv_zlexcount(ctx, KL(zl), "-", "(c"), 2, "ZLEXCOUNT");
    expect_int(hotkv_zremrangebylex(ctx, KL(zl), "[a", "[a"), 1, "ZREMRANGEBYLEX");
    expect_int(hotkv_zremrangebyrank(ctx, KL(zl), 0, 0), 1, "ZREMRANGEBYRANK");
    expect_int(hotkv_zcard(ctx, KL(zl)), 2, "ZCARD after removes");
    expect_int(hotkv_zremrangebyscore(ctx, KL(z), "1", "2"), 1, "ZREMRANGEBYSCORE");
    expect_int(hotkv_zrem(ctx, KL(z), ARR("c", "nope"), (size_t[]){1, 4}, 2), 1, "ZREM");
    {
        uint64_t cur = 0;
        int pairs = 0, rounds = 0;
        do {
            hotkv_reply_t *r = hotkv_zscan(ctx, KL(z), cur, NULL, 100);
            if (!is_agg(r) || r->nelem != 2) { fail("ZSCAN", "bad shape"); hotkv_free_reply(r); break; }
            cur = strtoull(rtext(elem(r, 0)), NULL, 10);
            pairs += (int)(elem(r, 1) ? elem(r, 1)->nelem / 2 : 0);
            hotkv_free_reply(r);
        } while (cur != 0 && ++rounds < 100);
        CHECK(pairs == 2, "ZSCAN returns the remaining 2 pairs");
    }
    {
        hotkv_reply_t *r = hotkv_zpopmin(ctx, KL(z), -1);
        hotkv_reply_t *kk = NULL, *vv = NULL;
        CHECK(hotkv_reply_pair_count(r) >= 1 || (is_agg(r) && r->nelem == 2), "ZPOPMIN one");
        (void)kk; (void)vv;
        hotkv_free_reply(r);
        r = hotkv_zpopmax(ctx, KL(z), 5);
        CHECK(is_agg(r), "ZPOPMAX count");
        hotkv_free_reply(r);
        expect_int(hotkv_zcard(ctx, KL(z)), 0, "ZCARD after pops");
        expect_int(hotkv_zadd(ctx, KL(z), sc, mm, ml, 4), 4, "ZADD again");
        r = hotkv_zrandmember(ctx, KL(z));
        CHECK(r && r->type == HOTKV_REPLY_BULK, "ZRANDMEMBER");
        hotkv_free_reply(r);
        r = hotkv_zrandmember_count(ctx, KL(z), 3, 1);
        CHECK(hotkv_reply_pair_count(r) == 3, "ZRANDMEMBER count WITHSCORES");
        hotkv_free_reply(r);
        r = hotkv_zrandmember_count(ctx, KL(z), -6, 0);
        CHECK(is_agg(r) && r->nelem == 6, "ZRANDMEMBER negative count");
        hotkv_free_reply(r);
    }
}

/* ------------------------------------------------------------------------- */
static void test_bitmaps_hll_geo(void) {
    char *b = key("bits"), *b2 = key("bits2"), *bd = key("bitsd"), *s1 = key("lcs1"), *s2 = key("lcs2");
    char *p1 = key("hll1"), *p2 = key("hll2"), *pm = key("hllm"), *g = key("geo");
    printf("-- bitmaps, HyperLogLog, geo --\n");

    expect_int(hotkv_setbit(ctx, KL(b), 7, 1), 0, "SETBIT");
    expect_int(hotkv_setbit(ctx, KL(b), 7, 1), 1, "SETBIT returns old bit");
    expect_int(hotkv_setbit(ctx, KL(b), 15, 1), 0, "SETBIT 15");
    expect_int(hotkv_getbit(ctx, KL(b), 7), 1, "GETBIT 7");
    expect_int(hotkv_getbit(ctx, KL(b), 8), 0, "GETBIT 8");
    expect_int(hotkv_bitcount(ctx, KL(b), 0, 0, 0, NULL), 2, "BITCOUNT");
    expect_int(hotkv_bitcount(ctx, KL(b), 1, 0, 0, NULL), 1, "BITCOUNT byte range");
    expect_int(hotkv_bitcount(ctx, KL(b), 1, 0, 7, "BIT"), 1, "BITCOUNT BIT range");
    expect_int(hotkv_bitpos(ctx, KL(b), 1, 0, 0, 0, 0, NULL), 7, "BITPOS");
    expect_int(hotkv_bitpos(ctx, KL(b), 1, 1, 1, 0, 0, NULL), 15, "BITPOS from byte 1");
    expect_int(hotkv_setbit(ctx, KL(b2), 7, 1), 0, "SETBIT b2");
    expect_int(hotkv_setbit(ctx, KL(b2), 6, 1), 0, "SETBIT b2 6");
    {
        const char *ks[] = {b, b2};
        size_t kl[2];
        lens_of(ks, 2, kl);
        expect_int(hotkv_bitop(ctx, "AND", KL(bd), ks, kl, 2), 2, "BITOP AND (length)");
        expect_int(hotkv_bitcount(ctx, KL(bd), 0, 0, 0, NULL), 1, "BITOP AND result");
        expect_int(hotkv_bitop(ctx, "OR", KL(bd), ks, kl, 2), 2, "BITOP OR");
        expect_int(hotkv_bitcount(ctx, KL(bd), 0, 0, 0, NULL), 3, "BITOP OR result");
        expect_int(hotkv_bitop(ctx, "XOR", KL(bd), ks, kl, 2), 2, "BITOP XOR");
        expect_int(hotkv_bitop(ctx, "NOT", KL(bd), ks, kl, 1), 2, "BITOP NOT");
    }
    expect_ok(hotkv_set(ctx, KL(s1), "ohmytext", 8), "SET lcs1");
    expect_ok(hotkv_set(ctx, KL(s2), "mynewtext", 9), "SET lcs2");
    expect_text(hotkv_lcs(ctx, KL(s1), KL(s2), 0), "mytext", "LCS");
    expect_int(hotkv_lcs(ctx, KL(s1), KL(s2), 1), 6, "LCS LEN");

    expect_int(hotkv_pfadd(ctx, KL(p1), ARR("a", "b", "c"), (size_t[]){1, 1, 1}, 3), 1, "PFADD");
    expect_int(hotkv_pfadd(ctx, KL(p1), ARR("a"), (size_t[]){1}, 1), 0, "PFADD no change");
    expect_int(hotkv_pfadd(ctx, KL(p2), ARR("c", "d"), (size_t[]){1, 1}, 2), 1, "PFADD p2");
    expect_int(hotkv_pfcount(ctx, ARR(p1), (size_t[]){strlen(p1)}, 1), 3, "PFCOUNT");
    {
        const char *ks[] = {p1, p2};
        size_t kl[2];
        lens_of(ks, 2, kl);
        expect_int(hotkv_pfcount(ctx, ks, kl, 2), 4, "PFCOUNT union");
        expect_ok(hotkv_pfmerge(ctx, KL(pm), ks, kl, 2), "PFMERGE");
        expect_int(hotkv_pfcount(ctx, ARR(pm), (size_t[]){strlen(pm)}, 1), 4, "PFCOUNT merged");
    }

    {
        double lon[] = {13.361389, 15.087269, 2.349014};
        double lat[] = {38.115556, 37.502669, 48.864716};
        const char *mem[] = {"palermo", "catania", "paris"};
        size_t ml[] = {7, 7, 5};
        hotkv_geosearch_opts_t o;
        hotkv_reply_t *r;
        double d = 0;
        expect_int(hotkv_geoadd(ctx, KL(g), lon, lat, mem, ml, 3), 3, "GEOADD");
        r = hotkv_geodist(ctx, KL(g), "palermo", 7, "catania", 7, "km");
        CHECK(rdouble(r, &d) && d > 160 && d < 170, "GEODIST km (about 166)");
        hotkv_free_reply(r);
        r = hotkv_geodist(ctx, KL(g), "palermo", 7, "catania", 7, NULL);
        CHECK(rdouble(r, &d) && d > 160000 && d < 170000, "GEODIST meters");
        hotkv_free_reply(r);
        expect_nil(hotkv_geodist(ctx, KL(g), "palermo", 7, "nope", 4, NULL), "GEODIST missing member");
        r = hotkv_geopos(ctx, KL(g), ARR("palermo", "nope"), (size_t[]){7, 4}, 2);
        CHECK(is_agg(r) && r->nelem == 2 && is_agg(elem(r, 0)) && elem(r, 0)->nelem == 2 &&
                  rdouble(elem(elem(r, 0), 0), &d) && d > 13.36 && d < 13.37 &&
                  (elem(r, 1)->type == HOTKV_REPLY_NIL),
              "GEOPOS");
        hotkv_free_reply(r);
        r = hotkv_geohash(ctx, KL(g), ARR("palermo"), (size_t[]){7}, 1);
        CHECK(is_agg(r) && r->nelem == 1 && rtext(elem(r, 0)) && strlen(rtext(elem(r, 0))) == 11 &&
                  strncmp(rtext(elem(r, 0)), "sqc8b49rny", 10) == 0,
              "GEOHASH sqc8b49rny0");
        hotkv_free_reply(r);
        memset(&o, 0, sizeof(o));
        o.from_lonlat = 1; o.lon = 15; o.lat = 37; o.by_radius = 1; o.radius = 200; o.unit = "km";
        o.order = 1;
        expect_strs(hotkv_geosearch(ctx, KL(g), &o), 2, "GEOSEARCH radius ASC", "catania", "palermo");
        o.withdist = 1;
        r = hotkv_geosearch(ctx, KL(g), &o);
        CHECK(is_agg(r) && r->nelem == 2 && is_agg(elem(r, 0)) && elem(r, 0)->nelem == 2,
              "GEOSEARCH WITHDIST nested");
        hotkv_free_reply(r);
        memset(&o, 0, sizeof(o));
        o.from_member = "palermo"; o.by_box = 1; o.width = 400; o.height = 400; o.unit = "km";
        o.order = 2; o.count = 1;
        expect_strs(hotkv_geosearch(ctx, KL(g), &o), 1, "GEOSEARCH box DESC COUNT 1", "catania");
        expect_err(hotkv_geoadd(ctx, KL(g), (double[]){NAN}, (double[]){1.0}, ARR("x"), (size_t[]){1}, 1),
                   "ERR invalid numeric", "GEOADD NaN is refused locally");
        expect_int(hotkv_geoadd(ctx, KL(g), (double[]){1.0}, (double[]){1.0}, ARR("ok"), (size_t[]){2}, 1), 1,
                   "connection still usable after local error");
    }
}

/* ------------------------------------------------------------------------- */
static void test_streams(void) {
    char *s = key("x"), *g = key("xg");
    char id1[64] = "", id2[64] = "", id3[64] = "";
    const char *f[] = {"field"};
    size_t fl[] = {5};
    printf("-- streams --\n");

#define XADD_ID(buf, val)                                                           \
    do {                                                                            \
        hotkv_reply_t *_r = hotkv_xadd(ctx, KL(s), "*", 1, f, fl, ARR(val), (size_t[]){strlen(val)}, 1); \
        if (_r && _r->type == HOTKV_REPLY_BULK) { snprintf(buf, sizeof(buf), "%s", _r->str); pass(); } \
        else fail("XADD", "no id returned");                                       \
        hotkv_free_reply(_r);                                                       \
    } while (0)
    XADD_ID(id1, "one");
    XADD_ID(id2, "two");
    XADD_ID(id3, "three");
    expect_int(hotkv_xlen(ctx, KL(s)), 3, "XLEN");
    {
        hotkv_reply_t *r = hotkv_xrange(ctx, KL(s), "-", 1, "+", 1, 2);
        CHECK(is_agg(r) && r->nelem == 2 && is_agg(elem(r, 0)) && strcmp(rtext(elem(elem(r, 0), 0)), id1) == 0,
              "XRANGE COUNT 2");
        hotkv_free_reply(r);
        r = hotkv_xrevrange(ctx, KL(s), "+", "-", 1);
        CHECK(is_agg(r) && r->nelem == 1 && strcmp(rtext(elem(elem(r, 0), 0)), id3) == 0, "XREVRANGE");
        hotkv_free_reply(r);
        r = hotkv_xread(ctx, ARR(s), (size_t[]){strlen(s)}, ARR("0"), (size_t[]){1}, 1, 10);
        CHECK(r && r->type != HOTKV_REPLY_NIL && r->type != HOTKV_REPLY_ERROR &&
                  hotkv_reply_pair_count(r) == 1,
              "XREAD");
        hotkv_free_reply(r);
        r = hotkv_xinfo_stream(ctx, KL(s));
        CHECK(r && (r->type == HOTKV_REPLY_MAP || r->type == HOTKV_REPLY_ARRAY) && r->nelem >= 2,
              "XINFO STREAM");
        hotkv_free_reply(r);
    }

    expect_ok(hotkv_xgroup_create(ctx, KL(s), g, "0", 0), "XGROUP CREATE");
    expect_err(hotkv_xgroup_create(ctx, KL(s), g, "0", 0), "BUSYGROUP", "XGROUP CREATE twice");
    expect_int(hotkv_xgroup_createconsumer(ctx, KL(s), g, "alice"), 1, "XGROUP CREATECONSUMER");
    {
        hotkv_reply_t *r = hotkv_xreadgroup(ctx, g, "alice", ARR(s), (size_t[]){strlen(s)}, ARR(">"),
                                            (size_t[]){1}, 1, 2, 0);
        CHECK(r && r->type != HOTKV_REPLY_NIL && r->type != HOTKV_REPLY_ERROR &&
                  hotkv_reply_pair_count(r) == 1,
              "XREADGROUP first batch");
        hotkv_free_reply(r);
    }
    {
        hotkv_reply_t *r = hotkv_xpending(ctx, KL(s), g);
        CHECK(is_agg(r) && r->nelem == 4 && elem(r, 0)->integer == 2 &&
                  strcmp(rtext(elem(r, 1)), id1) == 0 && strcmp(rtext(elem(r, 2)), id2) == 0,
              "XPENDING summary (2 pending id1..id2)");
        hotkv_free_reply(r);
        r = hotkv_xpending_range(ctx, KL(s), g, "-", "+", 10, "alice");
        CHECK(is_agg(r) && r->nelem == 2 && is_agg(elem(r, 0)) && strcmp(rtext(elem(elem(r, 0), 0)), id1) == 0 &&
                  strcmp(rtext(elem(elem(r, 0), 1)), "alice") == 0,
              "XPENDING extended");
        hotkv_free_reply(r);
    }
    expect_int(hotkv_xgroup_createconsumer(ctx, KL(s), g, "bob"), 1, "XGROUP CREATECONSUMER bob");
    {
        hotkv_reply_t *r = hotkv_xclaim(ctx, KL(s), g, "bob", 0, ARR(id1), (size_t[]){strlen(id1)}, 1);
        CHECK(is_agg(r) && r->nelem == 1 && is_agg(elem(r, 0)) && strcmp(rtext(elem(elem(r, 0), 0)), id1) == 0,
              "XCLAIM");
        hotkv_free_reply(r);
        r = hotkv_xautoclaim(ctx, KL(s), g, "bob", 0, "0-0", 10);
        CHECK(is_agg(r) && r->nelem >= 2 && is_agg(elem(r, 1)) && elem(r, 1)->nelem == 2, "XAUTOCLAIM claims id1+id2");
        hotkv_free_reply(r);
        r = hotkv_xinfo_groups(ctx, KL(s));
        CHECK(is_agg(r) && r->nelem == 1, "XINFO GROUPS");
        hotkv_free_reply(r);
        r = hotkv_xinfo_consumers(ctx, KL(s), g);
        CHECK(is_agg(r) && r->nelem == 2, "XINFO CONSUMERS (alice, bob)");
        hotkv_free_reply(r);
    }
    expect_int(hotkv_xack(ctx, KL(s), g, ARR(id1, id2), (size_t[]){strlen(id1), strlen(id2)}, 2), 2, "XACK");
    expect_ok(hotkv_xgroup_setid(ctx, KL(s), g, "$"), "XGROUP SETID");
    expect_int(hotkv_xgroup_delconsumer(ctx, KL(s), g, "bob"), 0, "XGROUP DELCONSUMER (nothing pending)");
    expect_int(hotkv_xdel(ctx, KL(s), ARR(id2), (size_t[]){strlen(id2)}, 1), 1, "XDEL");
    expect_int(hotkv_xlen(ctx, KL(s)), 2, "XLEN after XDEL");
    expect_int(hotkv_xtrim(ctx, KL(s), "MAXLEN", 0, "1"), 1, "XTRIM MAXLEN 1");
    expect_int(hotkv_xlen(ctx, KL(s)), 1, "XLEN after XTRIM");
    expect_int(hotkv_xgroup_destroy(ctx, KL(s), g), 1, "XGROUP DESTROY");
}

/* ------------------------------------------------------------------------- */
static void test_scripting_tx_server(void) {
    char *k = key("lua"), *t1 = key("tx1"), *t2 = key("tx2"), *w = key("watch");
    char sha[64] = "";
    printf("-- scripting, transactions, server --\n");

    {
        const char *script = "return {KEYS[1], ARGV[1], redis.call('SET', KEYS[1], ARGV[1]).ok}";
        hotkv_reply_t *r = hotkv_eval(ctx, script, strlen(script), ARR(k), (size_t[]){strlen(k)}, 1,
                                      ARR("v"), (size_t[]){1}, 1);
        CHECK(is_agg(r) && r->nelem == 3 && strcmp(rtext(elem(r, 0)), k) == 0 &&
                  strcmp(rtext(elem(r, 1)), "v") == 0 && strcmp(rtext(elem(r, 2)), "OK") == 0,
              "EVAL");
        hotkv_free_reply(r);
        expect_text(hotkv_get(ctx, KL(k)), "v", "EVAL wrote the key");
        r = hotkv_script_load(ctx, "return redis.call('GET', KEYS[1])", 33);
        if (r && r->type == HOTKV_REPLY_BULK && r->len == 40) {
            snprintf(sha, sizeof(sha), "%s", r->str);
            pass();
        } else {
            fail("SCRIPT LOAD", "no sha1 returned");
        }
        hotkv_free_reply(r);
        expect_ints(hotkv_script_exists(ctx, (const char *[]){sha, "0000000000000000000000000000000000000000"}, 2),
                    2, "SCRIPT EXISTS", 1LL, 0LL);
        expect_text(hotkv_evalsha(ctx, sha, ARR(k), (size_t[]){strlen(k)}, 1, NULL, NULL, 0), "v", "EVALSHA");
        expect_text(hotkv_evalsha_ro(ctx, sha, ARR(k), (size_t[]){strlen(k)}, 1, NULL, NULL, 0), "v", "EVALSHA_RO");
        expect_text(hotkv_eval_ro(ctx, "return redis.call('GET', KEYS[1])", 33, ARR(k), (size_t[]){strlen(k)}, 1,
                                  NULL, NULL, 0), "v", "EVAL_RO");
        expect_err(hotkv_eval_ro(ctx, "return redis.call('SET', KEYS[1], 'x')", 39, ARR(k), (size_t[]){strlen(k)}, 1,
                                 NULL, NULL, 0), "ERR", "EVAL_RO refuses writes");
        if (!g_safe) {
            expect_ok(hotkv_script_flush(ctx), "SCRIPT FLUSH");
            expect_err(hotkv_evalsha(ctx, sha, ARR(k), (size_t[]){strlen(k)}, 1, NULL, NULL, 0), "NOSCRIPT",
                       "EVALSHA after flush");
        } else {
            skip("SCRIPT FLUSH", "safe mode: would clear every client's script cache");
        }
    }
    {
        char fn[64];
        char code[1024];
        hotkv_reply_t *r;
        snprintf(fn, sizeof(fn), "sdkfn%d", (int)hkv_getpid());
        snprintf(code, sizeof(code),
                 "#!lua name=%s\nredis.register_function('%s', function(keys, args) return args[1] end)\n"
                 "redis.register_function{function_name='%s_ro', callback=function(keys, args) return args[1] end, flags={'no-writes'}}",
                 fn, fn, fn);
        r = hotkv_function_load(ctx, code, strlen(code), 1);
        CHECK(r && r->type == HOTKV_REPLY_BULK, "FUNCTION LOAD returns the library name");
        hotkv_free_reply(r);
        expect_text(hotkv_fcall(ctx, fn, NULL, NULL, 0, ARR("hi"), (size_t[]){2}, 1), "hi", "FCALL");
        snprintf(fn + strlen(fn), sizeof(fn) - strlen(fn), "_ro");
        expect_text(hotkv_fcall_ro(ctx, fn, NULL, NULL, 0, ARR("ro"), (size_t[]){2}, 1), "ro", "FCALL_RO");
        r = hotkv_command(ctx, "FUNCTION DELETE %s", fn);
        hotkv_free_reply(r);
        snprintf(fn, sizeof(fn), "sdkfn%d", (int)hkv_getpid());
        expect_ok(hotkv_command(ctx, "FUNCTION DELETE %s", fn), "FUNCTION DELETE (generic call)");
    }

    /* MULTI / EXEC / DISCARD / WATCH */
    expect_ok(hotkv_multi(ctx), "MULTI");
    expect_text(hotkv_command(ctx, "SET %s %s", t1, "a"), "QUEUED", "queued SET");
    expect_text(hotkv_command(ctx, "INCR %s", t2), "QUEUED", "queued INCR");
    {
        hotkv_reply_t *r = hotkv_exec(ctx);
        CHECK(is_agg(r) && r->nelem == 2 && strcmp(rtext(elem(r, 0)), "OK") == 0 &&
                  elem(r, 1)->integer == 1,
              "EXEC results");
        hotkv_free_reply(r);
    }
    expect_ok(hotkv_multi(ctx), "MULTI 2");
    expect_text(hotkv_command(ctx, "SET %s %s", t1, "zzz"), "QUEUED", "queued");
    expect_ok(hotkv_discard(ctx), "DISCARD");
    expect_text(hotkv_get(ctx, KL(t1)), "a", "DISCARD dropped the write");
    expect_ok(hotkv_set(ctx, KL(w), "1", 1), "SET watched");
    expect_ok(hotkv_watch(ctx, ARR(w), (size_t[]){strlen(w)}, 1), "WATCH");
    expect_ok(hotkv_unwatch(ctx), "UNWATCH");
    expect_ok(hotkv_watch(ctx, ARR(w), (size_t[]){strlen(w)}, 1), "WATCH again");
    {
        /* A second connection touches the watched key: EXEC must abort. */
        hotkv_context_t *other = live_connect(g_u, g_proto);
        CHECK(other != NULL, "second connection");
        if (other) {
            expect_ok(hotkv_set(other, KL(w), "changed", 7), "SET from the other connection");
            hotkv_disconnect(other);
        }
    }
    expect_ok(hotkv_multi(ctx), "MULTI after a conflicting write");
    expect_text(hotkv_command(ctx, "SET %s %s", w, "lost"), "QUEUED", "queued");
    expect_nil(hotkv_exec(ctx), "EXEC aborts (nil) when a WATCHed key changed");
    expect_text(hotkv_get(ctx, KL(w)), "changed", "aborted EXEC did not write");
    expect_ok(hotkv_watch(ctx, ARR(w), (size_t[]){strlen(w)}, 1), "WATCH once more");
    expect_ok(hotkv_multi(ctx), "MULTI under WATCH");
    expect_text(hotkv_command(ctx, "SET %s %s", w, "2"), "QUEUED", "queued under WATCH");
    {
        hotkv_reply_t *r = hotkv_exec(ctx);
        CHECK(r && (r->type == HOTKV_REPLY_ARRAY), "EXEC with an untouched WATCH succeeds");
        hotkv_free_reply(r);
    }

    /* server */
    expect_text(hotkv_ping(ctx), "PONG", "PING");
    expect_text(hotkv_echo(ctx, "echo me", 7), "echo me", "ECHO");
    expect_int_at_least(hotkv_dbsize(ctx), 1, "DBSIZE");
    {
        hotkv_reply_t *r = hotkv_time(ctx);
        long long sec = 0, usec = 0;
        CHECK(is_agg(r) && r->nelem == 2 && rint64(elem(r, 0), &sec) && rint64(elem(r, 1), &usec) &&
                  llabs(sec - (long long)time(NULL)) < 120 && usec >= 0 && usec < 1000000,
              "TIME");
        hotkv_free_reply(r);
        expect_int_at_least(hotkv_lastsave(ctx), 1, "LASTSAVE");
        r = hotkv_info(ctx, "server");
        CHECK(r && rtext(r) && strstr(rtext(r), "# Server") != NULL, "INFO server has a Server section");
        hotkv_free_reply(r);
        r = hotkv_info(ctx, NULL);
        CHECK(r && rtext(r) && strstr(rtext(r), "# Keyspace") != NULL, "INFO default has Keyspace");
        hotkv_free_reply(r);
        r = hotkv_config_get(ctx, "maxmemory", 9);
        CHECK(is_agg(r) && r->nelem == 2 && strcmp(rtext(elem(r, 0)), "maxmemory") == 0, "CONFIG GET maxmemory");
        hotkv_free_reply(r);
    }
    if (!g_safe) {
        expect_ok(hotkv_select(ctx, 7), "SELECT 7");
        expect_ok(hotkv_set(ctx, "flushme", 7, "1", 1), "SET in db 7");
        expect_int(hotkv_dbsize(ctx), 1, "DBSIZE db 7");
        expect_ok(hotkv_flushdb(ctx), "FLUSHDB (db 7)");
        expect_int(hotkv_dbsize(ctx), 0, "DBSIZE after FLUSHDB");
        expect_ok(hotkv_select(ctx, 0), "SELECT 0");
    } else {
        skip("FLUSHDB", "safe mode: never flushes a shared server");
    }
    if (!g_safe && getenv("HOTKV_TEST_FLUSHALL") && *getenv("HOTKV_TEST_FLUSHALL") == '1') {
        expect_ok(hotkv_flushall(ctx), "FLUSHALL");
        expect_int(hotkv_dbsize(ctx), 0, "DBSIZE after FLUSHALL");
    } else {
        skip("FLUSHALL", "set HOTKV_TEST_FLUSHALL=1 on a disposable server");
    }
}

/* ------------------------------------------------------------------------- */
static void test_pubsub(const hotkv_url_t *u) {
    hotkv_context_t *sub = live_connect(u, g_proto);
    char *chan = key("chan"), *chan2 = key("chan2"), *pat = key("pat*");
    printf("-- pub/sub --\n");
    CHECK(sub != NULL, "subscriber connects");
    if (!sub) return;
    {
        struct timeval tv = {5, 0};
        hotkv_set_timeout(sub, tv);
    }
    CHECK(hotkv_subscribe(sub, ARR(chan, chan2), (size_t[]){strlen(chan), strlen(chan2)}, 2) == HOTKV_OK,
          "SUBSCRIBE two channels");
    CHECK(hotkv_psubscribe(sub, ARR(pat), (size_t[]){strlen(pat)}, 1) == HOTKV_OK, "PSUBSCRIBE");
    expect_strs(hotkv_pubsub_channels(ctx, chan), 1, "PUBSUB CHANNELS pattern", chan);
    {
        hotkv_reply_t *r = hotkv_pubsub_numsub(ctx, ARR(chan, "sdktest-nobody"), (size_t[]){strlen(chan), 14}, 2);
        long long n0 = -1, n1 = -1;
        CHECK(is_agg(r) && r->nelem == 4 && rint64(elem(r, 1), &n0) && n0 == 1 && rint64(elem(r, 3), &n1) &&
                  n1 == 0,
              "PUBSUB NUMSUB");
        hotkv_free_reply(r);
    }
    expect_int_at_least(hotkv_pubsub_numpat(ctx), 1, "PUBSUB NUMPAT");
    expect_int(hotkv_publish(ctx, KL(chan), "hello", 5), 1, "PUBLISH reaches the subscriber");
    {
        hotkv_reply_t *m = NULL;
        CHECK(hotkv_get_reply(sub, &m) == HOTKV_OK && m, "message arrives");
        if (m) {
            const hotkv_reply_t *kind = elem(m, 0), *ch = elem(m, 1), *body = elem(m, 2);
            CHECK(is_agg(m) && m->nelem == 3 && strcmp(rtext(kind), "message") == 0 &&
                      strcmp(rtext(ch), chan) == 0 && strcmp(rtext(body), "hello") == 0,
                  "message frame is [message, channel, payload]");
            hotkv_free_reply(m);
        }
    }
    {
        char pc[400];
        hotkv_reply_t *m = NULL;
        snprintf(pc, sizeof(pc), "sdktest:%s:patX", g_run_id);
        expect_int(hotkv_publish(ctx, KL(pc), "pm", 2), 1, "PUBLISH matches the pattern");
        CHECK(hotkv_get_reply(sub, &m) == HOTKV_OK && m, "pmessage arrives");
        if (m) {
            CHECK(is_agg(m) && m->nelem == 4 && strcmp(rtext(elem(m, 0)), "pmessage") == 0 &&
                      strcmp(rtext(elem(m, 3)), "pm") == 0,
                  "pmessage frame is [pmessage, pattern, channel, payload]");
            hotkv_free_reply(m);
        }
    }
    CHECK(hotkv_unsubscribe(sub, ARR(chan), (size_t[]){strlen(chan)}, 1) == HOTKV_OK, "UNSUBSCRIBE one");
    expect_int(hotkv_publish(ctx, KL(chan), "gone", 4), 0, "PUBLISH after UNSUBSCRIBE reaches nobody");
    CHECK(hotkv_punsubscribe(sub, ARR(pat), (size_t[]){strlen(pat)}, 1) == HOTKV_OK, "PUNSUBSCRIBE");
    hotkv_disconnect(sub);
}

/* ------------------------------------------------------------------------- */
static void test_hotkv_exclusive(void) {
    char *k = key("nx");
    printf("-- HotKV exclusive core commands (NEXPIRE, TAG, HOTKEYS, LATENCY, GCRA) --\n");
    expect_ok(hotkv_set(ctx, KL(k), "v", 1), "SET");
    expect_int(hotkv_nexpire(ctx, KL(k), 90ULL * 1000000000ULL), 1, "NEXPIRE");
    {
        hotkv_reply_t *r = hotkv_nttl(ctx, KL(k));
        CHECK(r && r->type == HOTKV_REPLY_INTEGER && r->integer > 80LL * 1000000000LL &&
                  r->integer <= 90LL * 1000000000LL,
              "NTTL");
        hotkv_free_reply(r);
    }
    expect_int(hotkv_nexpireat(ctx, KL(k), ((uint64_t)time(NULL) + 100) * 1000000000ULL), 1, "NEXPIREAT");
    expect_int(hotkv_nexpire(ctx, "sdktest-missing", 15, 1000000000ULL), 0, "NEXPIRE missing key");
    expect_int(hotkv_nttl(ctx, "sdktest-missing", 15), -2, "NTTL missing key");

    /* TAG.*: tag index over keys */
    {
        char *t1 = key("tagged1"), *t2 = key("tagged2");
        char red[160], blue[160];
        hotkv_reply_t *r;
        snprintf(red, sizeof(red), "red-%s", g_run_id);
        snprintf(blue, sizeof(blue), "blue-%s", g_run_id);
        expect_ok(hotkv_set(ctx, KL(t1), "1", 1), "SET tagged1");
        expect_ok(hotkv_set(ctx, KL(t2), "2", 1), "SET tagged2");
        expect_int(hotkv_tag_set(ctx, KL(t1), ARR(red, blue), (size_t[]){strlen(red), strlen(blue)}, 2), 2, "TAG.SET two tags");
        expect_int(hotkv_tag_set(ctx, KL(t2), ARR(red), (size_t[]){strlen(red)}, 1), 1, "TAG.SET one tag");
        expect_strset(hotkv_tag_get(ctx, KL(t1)), 2, "TAG.GET", red, blue);
        expect_int(hotkv_tag_count(ctx, KL(t1)), 2, "TAG.COUNT");
        expect_int(hotkv_tag_exists(ctx, KL(t1), red, strlen(red)), 1, "TAG.EXISTS yes");
        expect_int(hotkv_tag_exists(ctx, KL(t2), blue, strlen(blue)), 0, "TAG.EXISTS no");
        expect_strset(hotkv_tag_members(ctx, red, strlen(red)), 2, "TAG.MEMBERS", t1, t2);
        expect_strs(hotkv_tag_inter(ctx, ARR(red, blue), (size_t[]){strlen(red), strlen(blue)}, 2), 1,
                    "TAG.INTER holds only the key with both tags", t1);
        expect_int(hotkv_tag_rem(ctx, KL(t1), ARR(red), (size_t[]){strlen(red)}, 1), 1, "TAG.REM");
        expect_strs(hotkv_tag_get(ctx, KL(t1)), 1, "TAG.GET after TAG.REM", blue);
        expect_int(hotkv_tag_rem(ctx, KL(t1), ARR(blue), (size_t[]){strlen(blue)}, 1), 1, "TAG.REM last tag");
        expect_int(hotkv_tag_rem(ctx, KL(t2), ARR(red), (size_t[]){strlen(red)}, 1), 1, "TAG.REM cleanup");
        r = hotkv_hotkeys_top(ctx, 3);
        CHECK(is_agg(r), "HOTKEYS TOP returns an array");
        hotkv_free_reply(r);
        if (!g_safe) expect_ok(hotkv_hotkeys_reset(ctx), "HOTKEYS RESET");
        r = hotkv_latency_latest(ctx);
        CHECK(is_agg(r), "LATENCY LATEST returns an array");
        hotkv_free_reply(r);
        r = hotkv_latency_history(ctx, "command");
        CHECK(is_agg(r), "LATENCY HISTORY returns an array");
        hotkv_free_reply(r);
        if (!g_safe) expect_int_at_least(hotkv_latency_reset(ctx), 0, "LATENCY RESET returns the number of events cleared");
    }
}

/* ------------------------------------------------------------------------- */
static void live_core_run(const hotkv_url_t *u, int proto) {
    g_proto = proto;
    g_u = u;
    {
        char tag[16];
        snprintf(tag, sizeof(tag), "core-r%d", proto);
        live_init_run_id(tag);
    }
    printf("\n===== RESP%d =====\n", proto);
    ctx = live_connect(u, proto);
    if (!ctx) {
        g_failed++;
        return;
    }
    test_strings();
    test_keys();
    test_lists();
    test_sets();
    test_hashes();
    test_zsets();
    test_bitmaps_hll_geo();
    test_streams();
    test_scripting_tx_server();
    test_pubsub(u);
    test_hotkv_exclusive();
    hotkv_disconnect(ctx);
    ctx = NULL;
}

#endif /* HKV_LIVE_CORE_SUITE_H */
