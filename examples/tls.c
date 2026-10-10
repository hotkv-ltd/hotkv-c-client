/**
 * @file tls.c
 * @brief TLS connection example for libhotkv.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * Demonstrates: connecting to a HotKV server over TLS with CA verification.
 * Requires libhotkv built with -DHOTKV_USE_SSL=ON.
 *
 * Build:
 *   cc -o tls tls.c -lhotkv -lssl -lcrypto
 *
 * Usage:
 *   ./tls [host [port [ca.pem [cert.pem key.pem]]]]
 */

#include <hotkv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *host     = (argc > 1) ? argv[1] : "127.0.0.1";
    int port             = (argc > 2) ? atoi(argv[2]) : 6380;

#ifndef HOTKV_USE_SSL
    (void)host;
    (void)port;
    fprintf(stderr, "This example requires libhotkv built with -DHOTKV_USE_SSL=ON\n");
    return 1;
#else
    const char *ca_path  = (argc > 3) ? argv[3] : "ca.pem";
    const char *cert     = (argc > 4) ? argv[4] : NULL;
    const char *key      = (argc > 5) ? argv[5] : NULL;

    /* Initialize OpenSSL. */
    hotkv_ssl_init();

    /* Build connection options with TLS. Setting tls_ca (or use_tls) makes
     * hotkv_connect_with_options run the TLS handshake before anything else is
     * sent, so a password in the options never crosses the network in clear. For
     * a URL, hotkv_connect_url("hotkvs://:password@host:6380", &opts) does the
     * same. */
    hotkv_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.host = host;
    opts.port = port;
    opts.tls_ca = ca_path;
    opts.tls_cert = cert;
    opts.tls_key = key;
    opts.tls_servername = host; /* SNI */

    hotkv_context_t *ctx = hotkv_connect_with_options(&opts);
    if (!ctx || hotkv_context_err(ctx)) {
        fprintf(stderr, "TLS connection error: %s\n",
                ctx ? hotkv_context_errstr(ctx) : "OOM");
        hotkv_disconnect(ctx);
        return 1;
    }

    printf("TLS connection established to %s:%d\n", host, port);

    /* PING over TLS. */
    hotkv_reply_t *reply = hotkv_ping(ctx);
    if (reply && reply->type == HOTKV_REPLY_STRING) {
        printf("PING -> %s\n", reply->str);
    }
    hotkv_free_reply(reply);

    /* SET + GET over TLS. */
    reply = hotkv_set(ctx, "tls-key", 7, "secure-value", 12);
    if (reply && reply->type == HOTKV_REPLY_STRING) {
        printf("SET tls-key -> %s\n", reply->str);
    }
    hotkv_free_reply(reply);

    reply = hotkv_get(ctx, "tls-key", 7);
    if (reply && reply->type == HOTKV_REPLY_BULK) {
        printf("GET tls-key -> %s\n", reply->str);
    }
    hotkv_free_reply(reply);

    hotkv_disconnect(ctx);
    printf("Disconnected.\n");

    return 0;
#endif
}
