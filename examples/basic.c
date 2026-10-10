/**
 * @file basic.c
 * @brief Basic usage example for libhotkv.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * Demonstrates: connect, SET, GET, DEL, INCR, typed helpers, disconnect.
 *
 * Build:
 *   cc -o basic basic.c -lhotkv
 *
 * Usage:
 *   ./basic [host [port]]
 */

#include <hotkv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : 6379;

    /* --- Connect -------------------------------------------------------- */
    hotkv_context_t *ctx = hotkv_connect(host, port);
    if (!ctx) {
        fprintf(stderr, "Failed to allocate context\n");
        return 1;
    }
    if (hotkv_context_err(ctx)) {
        fprintf(stderr, "Connection error: %s\n", hotkv_context_errstr(ctx));
        hotkv_disconnect(ctx);
        return 1;
    }
    printf("Connected to %s:%d\n", host, port);

    /* --- PING ----------------------------------------------------------- */
    hotkv_reply_t *reply = hotkv_ping(ctx);
    if (reply && reply->type == HOTKV_REPLY_STRING) {
        printf("PING -> %s\n", reply->str);
    }
    hotkv_free_reply(reply);

    /* --- SET / GET ------------------------------------------------------ */
    reply = hotkv_set(ctx, "greeting", 8, "Hello, HotKV!", 13);
    if (reply && reply->type == HOTKV_REPLY_STRING) {
        printf("SET greeting -> %s\n", reply->str);
    }
    hotkv_free_reply(reply);

    reply = hotkv_get(ctx, "greeting", 8);
    if (reply && reply->type == HOTKV_REPLY_BULK) {
        printf("GET greeting -> %s\n", reply->str);
    }
    hotkv_free_reply(reply);

    /* --- INCR ----------------------------------------------------------- */
    reply = hotkv_set(ctx, "counter", 7, "0", 1);
    hotkv_free_reply(reply);

    reply = hotkv_incr(ctx, "counter", 7);
    if (reply && reply->type == HOTKV_REPLY_INTEGER) {
        printf("INCR counter -> %lld\n", (long long)reply->integer);
    }
    hotkv_free_reply(reply);

    reply = hotkv_incrby(ctx, "counter", 7, 41);
    if (reply && reply->type == HOTKV_REPLY_INTEGER) {
        printf("INCRBY counter 41 -> %lld\n", (long long)reply->integer);
    }
    hotkv_free_reply(reply);

    /* --- DEL ------------------------------------------------------------ */
    const char *keys[] = {"greeting", "counter"};
    const size_t keylens[] = {8, 7};
    reply = hotkv_del(ctx, keys, keylens, 2);
    if (reply && reply->type == HOTKV_REPLY_INTEGER) {
        printf("DEL greeting counter -> %lld keys removed\n",
               (long long)reply->integer);
    }
    hotkv_free_reply(reply);

    /* --- Generic command ------------------------------------------------ */
    reply = hotkv_command(ctx, "ECHO %s", "libhotkv works!");
    if (reply && (reply->type == HOTKV_REPLY_BULK ||
                  reply->type == HOTKV_REPLY_STRING)) {
        printf("ECHO -> %s\n", reply->str);
    }
    hotkv_free_reply(reply);

    /* --- Disconnect ----------------------------------------------------- */
    hotkv_disconnect(ctx);
    printf("Disconnected.\n");

    return 0;
}
