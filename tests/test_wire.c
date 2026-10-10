/**
 * @file test_wire.c
 * @brief Exact-wire tests against a fake server: no HotKV server needed.
 *
 * A forked child accepts connections on 127.0.0.1, records the RESP command
 * it receives and answers +OK. The test then compares the arguments the
 * library sent with what the server parsers accept. Focus: how every number
 * is written (plain decimal, never an exponent, never a locale comma) and the
 * argument order of commands that cannot be exercised live.
 *
 * POSIX only; on Windows the program reports SKIP (exit 77).
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#define _POSIX_C_SOURCE 200809L

#ifdef _WIN32

#include <stdio.h>

int main(void) {
    printf("test_wire needs fork(); skipped on Windows.\n");
    return 77;
}

#else

#include "hkv_test.h"

#include "hotkv.h"
#include "hotkv_private.h"

#include <arpa/inet.h>
#include <locale.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS 64

static int g_pipe_r = -1;
static pid_t g_child = -1;
static hotkv_context_t *g_ctx;

/* ---- child: fake server --------------------------------------------- */

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Reads one RESP array-of-bulk command from fd into buf (raw bytes). Returns
 * the length, or -1 on EOF. */
static long read_command(int fd, char *buf, size_t cap) {
    size_t n = 0;
    int argc = -1, i;
    char c;
    /* header: *N\r\n */
    for (;;) {
        if (read(fd, &c, 1) != 1) return -1;
        if (n + 1 >= cap) return -1;
        buf[n++] = c;
        if (c == '\n') break;
    }
    argc = atoi(buf + 1);
    for (i = 0; i < argc; i++) {
        long len;
        size_t start = n;
        for (;;) {
            if (read(fd, &c, 1) != 1) return -1;
            if (n + 1 >= cap) return -1;
            buf[n++] = c;
            if (c == '\n') break;
        }
        len = atol(buf + start + 1);
        if (len < 0 || n + (size_t)len + 2 >= cap) return -1;
        if (read_full(fd, buf + n, (size_t)len + 2) != 0) return -1;
        n += (size_t)len + 2;
    }
    return (long)n;
}

static void child_main(int lfd, int wfd) {
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) _exit(0);
        for (;;) {
            static char buf[1 << 16];
            long n = read_command(cfd, buf, sizeof(buf));
            uint32_t len;
            if (n < 0) break;
            len = (uint32_t)n;
            if (write(wfd, &len, 4) != 4 || write(wfd, buf, (size_t)n) != n) _exit(1);
            if (write(cfd, "+OK\r\n", 5) != 5) break;
        }
        close(cfd);
    }
}

static int start_fake(int *port) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0), p[2], one = 1;
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 4) != 0) return -1;
    getsockname(lfd, (struct sockaddr *)&a, &al);
    *port = ntohs(a.sin_port);
    if (pipe(p) != 0) return -1;
    g_child = fork();
    if (g_child == 0) {
        close(p[0]);
        child_main(lfd, p[1]);
        _exit(0);
    }
    close(p[1]);
    close(lfd);
    g_pipe_r = p[0];
    return 0;
}

/* ---- parent: capture + compare ---------------------------------------- */

/* Decodes the next captured command into argv strings (malloc'd). */
static int next_command(char **args, size_t *lens) {
    uint32_t len;
    char *raw, *p;
    int argc, i;
    if (read_full(g_pipe_r, &len, 4) != 0) return -1;
    raw = (char *)malloc(len + 1);
    if (read_full(g_pipe_r, raw, len) != 0) { free(raw); return -1; }
    raw[len] = '\0';
    argc = atoi(raw + 1);
    p = strchr(raw, '\n') + 1;
    for (i = 0; i < argc && i < MAX_ARGS; i++) {
        long l = atol(p + 1);
        p = strchr(p, '\n') + 1;
        args[i] = (char *)malloc((size_t)l + 1);
        memcpy(args[i], p, (size_t)l);
        args[i][l] = '\0';
        lens[i] = (size_t)l;
        p += l + 2;
    }
    free(raw);
    return argc;
}

/* Receives the command sent by the library and compares it with the NULL
 * terminated list of expected arguments. */
static int wire_matches(hotkv_reply_t *reply, const char *what, ...) {
    char *args[MAX_ARGS];
    size_t lens[MAX_ARGS];
    int argc = next_command(args, lens), i, ok = 1, want = 0;
    va_list ap;
    const char *e;
    va_start(ap, what);
    while ((e = va_arg(ap, const char *)) != NULL) {
        if (want >= argc || strcmp(args[want], e) != 0) {
            printf("FAIL\n    %s: argument %d is \"%s\", expected \"%s\"\n", what, want,
                   want < argc ? args[want] : "(missing)", e);
            ok = 0;
            break;
        }
        want++;
    }
    va_end(ap);
    if (ok && want != argc) {
        printf("FAIL\n    %s: %d arguments sent, %d expected\n", what, argc, want);
        ok = 0;
    }
    if (ok && (!reply || reply->type != HOTKV_REPLY_STRING)) {
        printf("FAIL\n    %s: no +OK reply\n", what);
        ok = 0;
    }
    for (i = 0; i < argc && i < MAX_ARGS; i++) free(args[i]);
    hotkv_free_reply(reply);
    if (!ok) g_tests_failed++;
    return ok;
}

#define WIRE(call, ...)                                             \
    do {                                                            \
        if (!wire_matches((call), #call, __VA_ARGS__, (char *)NULL)) return; \
    } while (0)

/* ---- tests ------------------------------------------------------------ */

TEST(llmbudget_set_cost_limit_is_exact_fixed_point) {
    WIRE(hotkv_llmbudget_set(g_ctx, "b", 1, 50000, 1.5, 60), "LLMBUDGET.SET", "b", "50000", "1.5", "60");
    WIRE(hotkv_llmbudget_set(g_ctx, "b", 1, 0, 0.00001, 0), "LLMBUDGET.SET", "b", "0", "0.00001", "0");
    WIRE(hotkv_llmbudget_set(g_ctx, "b", 1, 1000000000ULL, 1e9, 3600), "LLMBUDGET.SET", "b",
         "1000000000", "1000000000", "3600");
    WIRE(hotkv_llmbudget_set(g_ctx, "b", 1, 0, 123456.789012, 0), "LLMBUDGET.SET", "b", "0", "123456.789012", "0");
    WIRE(hotkv_llmbudget_set(g_ctx, "b", 1, 0, 0.0, 0), "LLMBUDGET.SET", "b", "0", "0", "0");
    WIRE(hotkv_llmbudget_set(g_ctx, "b", 1, 0, 10.0, 0), "LLMBUDGET.SET", "b", "0", "10", "0");
    WIRE(hotkv_llmbudget_set(g_ctx, "b", 1, 0, 1e-6, 0), "LLMBUDGET.SET", "b", "0", "0.000001", "0");
    PASS();
}

TEST(llmbudget_set_refuses_what_the_server_would_refuse) {
    hotkv_reply_t *r = hotkv_llmbudget_set(g_ctx, "b", 1, 0, -1.0, 0);
    ASSERT(r && r->type == HOTKV_REPLY_ERROR);
    hotkv_free_reply(r);
    r = hotkv_llmbudget_set(g_ctx, "b", 1, 0, NAN, 0);
    ASSERT(r && r->type == HOTKV_REPLY_ERROR);
    hotkv_free_reply(r);
    r = hotkv_llmbudget_set(g_ctx, "b", 1, 0, INFINITY, 0);
    ASSERT(r && r->type == HOTKV_REPLY_ERROR);
    hotkv_free_reply(r);
    /* nothing was sent, and the connection still works */
    WIRE(hotkv_llmbudget_set(g_ctx, "b", 1, 7, 2.5, 1), "LLMBUDGET.SET", "b", "7", "2.5", "1");
    PASS();
}

TEST(costs_and_prices_are_plain_decimals) {
    WIRE(hotkv_llmbudget_consume(g_ctx, "b", 1, 10, 0.000004, NULL), "LLMBUDGET.CONSUME", "b", "10", "0.000004");
    WIRE(hotkv_llmstats_record(g_ctx, "b", 1, 1, 2, 3, 1e-5, 9, NULL, NULL, 0, 0), "LLMSTATS.RECORD", "b", "1",
         "2", "3", "0.00001", "9");
    {
        double cr = 0.000001;
        WIRE(hotkv_llmstats_price_set(g_ctx, "m", 1, 1e-5, 12345678.25, &cr, NULL, NULL), "LLMSTATS.PRICE", "SET", "m",
             "INPUT", "0.00001", "OUTPUT", "12345678.25", "CACHEREAD", "0.000001");
    }
    PASS();
}

TEST(general_doubles_never_use_exponent_form) {
    WIRE(hotkv_incrbyfloat(g_ctx, "k", 1, 1e-5), "INCRBYFLOAT", "k", "0.00001");
    WIRE(hotkv_incrbyfloat(g_ctx, "k", 1, -2.5e-7), "INCRBYFLOAT", "k", "-0.00000025");
    WIRE(hotkv_incrbyfloat(g_ctx, "k", 1, 1e21), "INCRBYFLOAT", "k", "1000000000000000000000");
    WIRE(hotkv_incrbyfloat(g_ctx, "k", 1, 3.0), "INCRBYFLOAT", "k", "3");
    WIRE(hotkv_hincrbyfloat(g_ctx, "k", 1, "f", 1, 0.1), "HINCRBYFLOAT", "k", "f", "0.1");
    WIRE(hotkv_zincrby(g_ctx, "z", 1, 2.5, "m", 1), "ZINCRBY", "z", "2.5", "m");
    {
        double sc[] = {1e-5, -1e10, 0.1 + 0.2};
        const char *mm[] = {"a", "b", "c"};
        size_t ml[] = {1, 1, 1};
        WIRE(hotkv_zadd(g_ctx, "z", 1, sc, mm, ml, 3), "ZADD", "z", "0.00001", "a", "-10000000000", "b",
             "0.30000000000000004", "c");
    }
    {
        double lon[] = {13.361389}, lat[] = {1e-7};
        const char *mm[] = {"p"};
        size_t ml[] = {1};
        WIRE(hotkv_geoadd(g_ctx, "g", 1, lon, lat, mm, ml, 1), "GEOADD", "g", "13.361389", "0.0000001", "p");
    }
    {
        float v[3] = {0.1f, 1e-5f, -3.5f};
        WIRE(hotkv_vsim_set(g_ctx, "i", 1, "k", 1, v, 3), "VSIM.SET", "i", "k", "0.1", "0.00001", "-3.5");
    }
    PASS();
}

TEST(enterprise_double_options) {
    {
        double thr = 0.85;
        WIRE(hotkv_scache_create(g_ctx, "n", 1, 3, "cosine", &thr, 0), "SCACHE.CREATE", "n", "3", "METRIC", "cosine",
             "THRESHOLD", "0.85");
    }
    WIRE(hotkv_scache_threshold(g_ctx, "n", 1, 1e-3), "SCACHE.THRESHOLD", "n", "0.001");
    WIRE(hotkv_pcache_create(g_ctx, "n", 1, 100, 60, 1e-7), "PCACHE.CREATE", "n", "MAX_ENTRIES", "100", "TTL", "60",
         "COST_PER_HIT", "0.0000001");
    WIRE(hotkv_ratelimit_create(g_ctx, "r", 1, 10, 1000, "token_bucket", 5.5, 1e-3), "RATELIMIT.CREATE", "r", "10",
         "1000", "ALGO", "token_bucket", "REFILL", "5.5", "DRAIN", "0.001");
    {
        int64_t ts = 1700000000000LL;
        WIRE(hotkv_ts_add(g_ctx, "t", 1, &ts, 1e-7), "TS.ADD", "t", "1700000000000", "0.0000001");
    }
    PASS();
}

TEST(numbers_survive_a_comma_locale) {
    const char *names[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", NULL};
    const char *used = NULL;
    char probe[16];
    int i;
    for (i = 0; names[i]; i++) {
        if (setlocale(LC_ALL, names[i])) { used = names[i]; break; }
    }
    if (!used) {
        printf("SKIP (no comma-decimal locale installed)\n");
        g_tests_passed++;
        return;
    }
    snprintf(probe, sizeof(probe), "%.1f", 0.5);
    if (!strchr(probe, ',')) {
        setlocale(LC_ALL, "C");
        printf("SKIP (%s has no decimal comma)\n", used);
        g_tests_passed++;
        return;
    }
    /* Same calls as above, under the comma locale. */
    {
        hotkv_reply_t *r;
        int ok;
        r = hotkv_llmbudget_set(g_ctx, "b", 1, 0, 1234.5, 0);
        ok = wire_matches(r, "LLMBUDGET.SET (de_DE)", "LLMBUDGET.SET", "b", "0", "1234.5", "0", (char *)NULL);
        r = hotkv_incrbyfloat(g_ctx, "k", 1, 0.25);
        ok = wire_matches(r, "INCRBYFLOAT (de_DE)", "INCRBYFLOAT", "k", "0.25", (char *)NULL) && ok;
        {
            float v[2] = {0.5f, 1e-5f};
            r = hotkv_vsim_set(g_ctx, "i", 1, "k", 1, v, 2);
            ok = wire_matches(r, "VSIM.SET (de_DE)", "VSIM.SET", "i", "k", "0.5", "0.00001", (char *)NULL) && ok;
        }
        setlocale(LC_ALL, "C");
        if (!ok) return;
    }
    printf("OK (%s)\n", used);
    g_tests_passed++;
}

int main(void) {
    int port = 0;
    hotkv_options_t o;
    int status;
    signal(SIGPIPE, SIG_IGN);
    if (start_fake(&port) != 0) {
        fprintf(stderr, "cannot start the fake server\n");
        return 1;
    }
    memset(&o, 0, sizeof(o));
    o.host = "127.0.0.1";
    o.port = port;
    o.command_timeout.tv_sec = 5;
    g_ctx = hotkv_connect_with_options(&o);
    if (!g_ctx || hotkv_context_err(g_ctx) != HOTKV_OK) {
        fprintf(stderr, "cannot connect to the fake server\n");
        return 1;
    }

    printf("Exact-wire tests (fake server on port %d)\n", port);
    run_test_llmbudget_set_cost_limit_is_exact_fixed_point();
    run_test_llmbudget_set_refuses_what_the_server_would_refuse();
    run_test_costs_and_prices_are_plain_decimals();
    run_test_general_doubles_never_use_exponent_form();
    run_test_enterprise_double_options();
    run_test_numbers_survive_a_comma_locale();

    hotkv_disconnect(g_ctx);
    close(g_pipe_r);
    kill(g_child, SIGTERM);
    waitpid(g_child, &status, 0);
    HKV_TEST_SUMMARY();
    return g_tests_failed > 0 ? 1 : 0;
}

#endif /* _WIN32 */
