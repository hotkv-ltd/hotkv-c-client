/**
 * @file test_live_enterprise.c
 * @brief Value-checked live tests for every typed enterprise / AI wrapper
 * against a licensed HotKV server (set HOTKV_TEST_URL; exits 77 without it).
 *
 * If the server answers NOLICENSE the whole suite is reported as skipped (it
 * cannot be verified end to end without a license); with a license every
 * wrapper's reply is compared with the documented value. Runs once per RESP
 * version. All names are unique per run and dropped at the end.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "live_util.h"
#include "live_enterprise_suite.h"


int main(void) {
    const char *url = getenv("HOTKV_TEST_URL");
    hotkv_url_t u;
    if (!url || !*url) {
        printf("HOTKV_TEST_URL not set; skipping live enterprise tests.\n");
        return 77;
    }
    if (hotkv_url_parse(url, &u) != HOTKV_OK) {
        fprintf(stderr, "invalid HOTKV_TEST_URL: %s\n", url);
        return 1;
    }
    live_enterprise_run(&u, HOTKV_RESP2);
    live_enterprise_run(&u, HOTKV_RESP3);
    free_keys();
    hotkv_url_free(&u);
    printf("\n=================================\n");
    printf("Results: %d ok, %d skipped, %d failed\n", g_ok, g_skipped, g_failed);
    return g_failed > 0 ? 1 : 0;
}
