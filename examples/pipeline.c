/**
 * @file pipeline.c
 * @brief Pipelining example for libhotkv.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * Demonstrates: appending multiple commands without reading, then
 * reading all replies in one batch. This reduces round-trips.
 *
 * Build:
 *   cc -o pipeline pipeline.c -lhotkv
 *
 * Usage:
 *   ./pipeline [host [port]]
 */

#include <hotkv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : 6379;

    hotkv_context_t *ctx = hotkv_connect(host, port);
    if (!ctx || hotkv_context_err(ctx)) {
        fprintf(stderr, "Connection error: %s\n",
                ctx ? hotkv_context_errstr(ctx) : "OOM");
        hotkv_disconnect(ctx);
        return 1;
    }
    printf("Connected to %s:%d\n", host, port);

    /* --- Pipeline 1000 SET commands ------------------------------------- */
    const int N = 1000;
    printf("Pipelining %d SET commands...\n", N);

    for (int i = 0; i < N; i++) {
        char key[32], val[32];
        int klen = snprintf(key, sizeof(key), "pipe:%d", i);
        int vlen = snprintf(val, sizeof(val), "value:%d", i);
        hotkv_append_command(ctx, "SET %b %b", key, (size_t)klen,
                             val, (size_t)vlen);
    }

    /* --- Read all replies ----------------------------------------------- */
    int ok_count = 0;
    int err_count = 0;

    for (int i = 0; i < N; i++) {
        hotkv_reply_t *reply = NULL;
        if (hotkv_get_reply(ctx, &reply) != HOTKV_OK || !reply) {
            fprintf(stderr, "Error reading reply %d: %s\n",
                    i, hotkv_context_errstr(ctx));
            err_count++;
            continue;
        }
        if (reply->type == HOTKV_REPLY_STRING &&
            strcmp(reply->str, "OK") == 0) {
            ok_count++;
        } else {
            err_count++;
        }
        hotkv_free_reply(reply);
    }

    printf("Pipeline results: %d OK, %d errors\n", ok_count, err_count);

    /* --- Pipeline GET+DEL for cleanup ----------------------------------- */
    printf("Pipelining %d GET + DEL commands...\n", N);

    for (int i = 0; i < N; i++) {
        char key[32];
        int klen = snprintf(key, sizeof(key), "pipe:%d", i);
        hotkv_append_command(ctx, "GET %b", key, (size_t)klen);
        hotkv_append_command(ctx, "DEL %b", key, (size_t)klen);
    }

    /* Read 2*N replies (GET + DEL per key). */
    for (int i = 0; i < N * 2; i++) {
        hotkv_reply_t *reply = NULL;
        hotkv_get_reply(ctx, &reply);
        hotkv_free_reply(reply);
    }

    printf("Cleanup done.\n");

    hotkv_disconnect(ctx);
    printf("Disconnected.\n");

    return 0;
}
