/**
 * @file test_tls.c
 * @brief TLS and authentication behaviour against a real TLS server.
 *
 * Needs a library built with HOTKV_USE_SSL=ON and (all optional, each group is
 * skipped when its variables are unset):
 *
 *   HOTKV_TEST_TLS_URL      hotkvs://[:password@]host:port of a TLS server whose
 *                           certificate the CA below signed (e.g. a server started
 *                           with --tls-port and --requirepass)
 *   HOTKV_TEST_TLS_CA       path to that CA certificate (PEM)
 *   HOTKV_TEST_TLS_PASSWORD the password the server wants (if the URL carries none)
 *   HOTKV_TEST_MTLS_URL     hotkvs://host:port of a server with tls-auth-clients yes
 *   HOTKV_TEST_TLS_CERT / HOTKV_TEST_TLS_KEY   client certificate and key for it
 *   HOTKV_TEST_PLAIN_ADDR   host:port of a plaintext listener to point a TLS client at
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "live_util.h"

#ifndef HOTKV_USE_SSL

int main(void) {
    printf("libhotkv was built without HOTKV_USE_SSL; skipping TLS tests.\n");
    return 77;
}

#else

static const char *env(const char *n) {
    const char *v = getenv(n);
    return (v && *v) ? v : NULL;
}

static int connected_ok(hotkv_context_t *c) {
    return c && hotkv_context_err(c) == HOTKV_OK;
}

static void test_server(const char *url, const char *ca, const char *password) {
    hotkv_options_t o;
    hotkv_context_t *c;
    char *k = key("tls-k"), *big = key("tls-big");
    hotkv_url_t u;
    hotkv_reply_t *r;
    size_t i, n = 1024 * 1024;
    char *blob;
    printf("-- TLS server %s --\n", url);

    CHECK(hotkv_url_parse(url, &u) == HOTKV_OK && u.use_tls == 1, "URL parses as TLS");

    memset(&o, 0, sizeof(o));
    o.tls_ca = ca;
    o.command_timeout.tv_sec = 10;
    o.connect_timeout.tv_sec = 10;

    /* 1. URL form with the CA: handshake happens before AUTH. */
    c = hotkv_connect_url(url, &o);
    CHECK(connected_ok(c), "hotkv_connect_url over TLS with the CA");
    if (!connected_ok(c)) {
        printf("    error: %s\n", c ? hotkv_context_errstr(c) : "OOM");
        hotkv_disconnect(c);
        hotkv_url_free(&u);
        return;
    }
    expect_text(hotkv_ping(c), "PONG", "PING over TLS");
    expect_ok(hotkv_set(c, KL(k), "secure", 6), "SET over TLS");
    expect_text(hotkv_get(c, KL(k)), "secure", "GET over TLS");

    /* A value much larger than one TLS record, both directions. */
    blob = (char *)malloc(n);
    for (i = 0; i < n; i++) blob[i] = (char)('a' + (i * 7 + i / 251) % 26);
    expect_ok(hotkv_set(c, KL(big), blob, n), "SET 1 MiB over TLS");
    r = hotkv_get(c, KL(big));
    CHECK(r && r->type == HOTKV_REPLY_BULK && r->len == n && memcmp(r->str, blob, n) == 0, "GET 1 MiB over TLS round trip");
    hotkv_free_reply(r);
    free(blob);

    /* Pipeline across records. */
    for (i = 0; i < 200; i++) {
        hotkv_append_command(c, "SET %s %d", k, (int)i);
        hotkv_append_command(c, "GET %s", k);
    }
    {
        int ok = 1;
        for (i = 0; i < 400; i++) {
            hotkv_reply_t *pr = NULL;
            if (hotkv_get_reply(c, &pr) != HOTKV_OK || !pr) { ok = 0; break; }
            if (i % 2 == 1) {
                char want[16];
                snprintf(want, sizeof(want), "%d", (int)(i / 2));
                if (!rtext(pr) || strcmp(rtext(pr), want) != 0) ok = 0;
            }
            hotkv_free_reply(pr);
        }
        CHECK(ok, "400 pipelined commands over TLS");
    }
    hotkv_free_reply(hotkv_command(c, "DEL %s %s", k, big));
    hotkv_disconnect(c);

    /* 2. Options form, credentials from the fields. */
    {
        hotkv_options_t o2 = o;
        o2.host = u.host;
        o2.port = u.port;
        o2.password = u.password ? u.password : password;
        o2.use_tls = 1;
        c = hotkv_connect_with_options(&o2);
        CHECK(connected_ok(c), "hotkv_connect_with_options over TLS");
        if (connected_ok(c)) expect_text(hotkv_ping(c), "PONG", "PING (options form)");
        hotkv_disconnect(c);
    }

    /* 3. RESP3 over TLS. */
    {
        hotkv_options_t o3 = o;
        o3.host = u.host;
        o3.port = u.port;
        o3.password = u.password ? u.password : password;
        o3.protocol = HOTKV_RESP3;
        c = hotkv_connect_with_options(&o3);
        CHECK(connected_ok(c), "RESP3 over TLS");
        if (connected_ok(c)) {
            r = hotkv_command(c, "HGETALL %s", "sdktest-no-such-hash");
            CHECK(r && (r->type == HOTKV_REPLY_MAP || r->type == HOTKV_REPLY_ARRAY), "RESP3 map reply over TLS");
            hotkv_free_reply(r);
        }
        hotkv_disconnect(c);
    }

    /* 4. Verification must fail with the wrong trust anchor, with a useful message. */
    {
        hotkv_options_t o4 = o;
        o4.host = u.host;
        o4.port = u.port;
        o4.tls_ca = NULL;     /* system store: does not contain the test CA */
        o4.use_tls = 1;
        c = hotkv_connect_with_options(&o4);
        CHECK(c && hotkv_context_err(c) != HOTKV_OK, "unknown CA is refused");
        CHECK(c && strstr(hotkv_context_errstr(c), "handshake") != NULL, "error message names the handshake");
        CHECK(c && strstr(hotkv_context_errstr(c), "certificate") != NULL, "error message names the certificate problem");
        hotkv_disconnect(c);
    }
    /* 5. A name that is not in the certificate is refused. */
    {
        hotkv_options_t o5 = o;
        o5.host = u.host;
        o5.port = u.port;
        o5.tls_servername = "wrong-name.example.invalid";
        c = hotkv_connect_with_options(&o5);
        CHECK(c && hotkv_context_err(c) != HOTKV_OK, "certificate name mismatch is refused");
        hotkv_disconnect(c);
    }
    /* 6. ... unless verification is switched off explicitly. */
    {
        hotkv_options_t o6 = o;
        o6.host = u.host;
        o6.port = u.port;
        o6.password = u.password ? u.password : password;
        o6.tls_ca = NULL;
        o6.use_tls = 1;
        o6.tls_insecure_skip_verify = 1;
        c = hotkv_connect_with_options(&o6);
        CHECK(connected_ok(c), "tls_insecure_skip_verify connects without trusting the CA");
        if (connected_ok(c)) expect_text(hotkv_ping(c), "PONG", "PING (insecure)");
        hotkv_disconnect(c);
    }
    /* 7. Wrong password: a clean error, not a hang or a crash. */
    {
        hotkv_options_t o7 = o;
        o7.host = u.host;
        o7.port = u.port;
        o7.password = "definitely-not-the-password";
        c = hotkv_connect_with_options(&o7);
        if (u.password || password) {
            CHECK(c && hotkv_context_err(c) != HOTKV_OK, "wrong password fails the connect");
            CHECK(c && (strstr(hotkv_context_errstr(c), "WRONGPASS") || strstr(hotkv_context_errstr(c), "invalid") ||
                        strstr(hotkv_context_errstr(c), "ERR")),
                  "wrong password reports the server error");
        } else {
            skip("wrong password", "server needs no password");
        }
        hotkv_disconnect(c);
    }
    /* 8. No password at all: commands answer NOAUTH. */
    if (u.password || password) {
        hotkv_options_t o8 = o;
        o8.host = u.host;
        o8.port = u.port;
        c = hotkv_connect_with_options(&o8);
        CHECK(connected_ok(c), "connect without a password succeeds (nothing sent yet)");
        if (connected_ok(c)) {
            r = hotkv_ping(c);
            CHECK(r && r->type == HOTKV_REPLY_ERROR && hotkv_reply_is_noauth(r), "PING without AUTH is NOAUTH");
            hotkv_free_reply(r);
        }
        hotkv_disconnect(c);
    }
    hotkv_url_free(&u);
}

static void test_mtls(const char *url, const char *ca, const char *cert, const char *keyfile) {
    hotkv_url_t u;
    hotkv_options_t o;
    hotkv_context_t *c;
    printf("-- mutual TLS %s --\n", url);
    CHECK(hotkv_url_parse(url, &u) == HOTKV_OK, "URL parses");
    memset(&o, 0, sizeof(o));
    o.tls_ca = ca;
    o.command_timeout.tv_sec = 10;
    o.tls_cert = cert;
    o.tls_key = keyfile;
    c = hotkv_connect_url(url, &o);
    CHECK(connected_ok(c), "client certificate accepted");
    if (connected_ok(c)) expect_text(hotkv_ping(c), "PONG", "PING with a client certificate");
    hotkv_disconnect(c);

    o.tls_cert = NULL;
    o.tls_key = NULL;
    c = hotkv_connect_url(url, &o);
    /* TLS 1.3 reports the missing certificate on the first read, TLS 1.2 in the handshake. */
    if (connected_ok(c)) {
        hotkv_reply_t *r = hotkv_ping(c);
        CHECK(!r || r->type == HOTKV_REPLY_ERROR, "no client certificate: the server drops the connection");
        hotkv_free_reply(r);
    } else {
        pass();
    }
    hotkv_disconnect(c);

    o.tls_cert = "/nonexistent/client.crt";
    o.tls_key = keyfile;
    c = hotkv_connect_url(url, &o);
    CHECK(c && hotkv_context_err(c) != HOTKV_OK, "unreadable client certificate path is an error");
    hotkv_disconnect(c);
    hotkv_url_free(&u);
}

static void test_plain_listener(const char *addr) {
    char host[128];
    int port = 0;
    hotkv_options_t o;
    hotkv_context_t *c;
    const char *colon = strrchr(addr, ':');
    printf("-- TLS client against a plaintext listener %s --\n", addr);
    if (!colon || (size_t)(colon - addr) >= sizeof(host)) {
        skip("plaintext listener", "HOTKV_TEST_PLAIN_ADDR must be host:port");
        return;
    }
    memcpy(host, addr, (size_t)(colon - addr));
    host[colon - addr] = '\0';
    port = atoi(colon + 1);
    memset(&o, 0, sizeof(o));
    o.host = host;
    o.port = port;
    o.use_tls = 1;
    o.tls_insecure_skip_verify = 1;
    o.connect_timeout.tv_sec = 5;
    o.command_timeout.tv_sec = 5;
    c = hotkv_connect_with_options(&o);
    CHECK(c && hotkv_context_err(c) != HOTKV_OK, "TLS handshake with a plaintext server fails cleanly (no hang)");
    hotkv_disconnect(c);
}

int main(void) {
    const char *url = env("HOTKV_TEST_TLS_URL"), *ca = env("HOTKV_TEST_TLS_CA"),
               *pw = env("HOTKV_TEST_TLS_PASSWORD"), *murl = env("HOTKV_TEST_MTLS_URL"),
               *cert = env("HOTKV_TEST_TLS_CERT"), *keyf = env("HOTKV_TEST_TLS_KEY"),
               *plain = env("HOTKV_TEST_PLAIN_ADDR");
    if (!(url && ca) && !(murl && ca && cert && keyf) && !plain) {
        printf("No TLS test environment set (HOTKV_TEST_TLS_URL + HOTKV_TEST_TLS_CA, ...); skipping.\n");
        return 77;
    }
    live_init_run_id("tls");
    if (url && ca) test_server(url, ca, pw);
    if (murl && ca && cert && keyf) test_mtls(murl, ca, cert, keyf);
    if (plain) test_plain_listener(plain);
    free_keys();
    printf("\n=================================\n");
    printf("Results: %d ok, %d skipped, %d failed\n", g_ok, g_skipped, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#endif /* HOTKV_USE_SSL */
