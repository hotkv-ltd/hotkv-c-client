/**
 * @file test_live_core.c
 * @brief Value-checked live tests for every typed core command wrapper.
 *
 * Needs a real HotKV server: set HOTKV_TEST_URL (e.g. hotkv://127.0.0.1:6379).
 * Without it the program exits 77 (CTest SKIP). Every wrapper is called and
 * its reply is compared with the value the command is specified to return,
 * not just "no error". The whole suite runs once per RESP version (RESP2 and
 * RESP3). HOTKV_TEST_FLUSHALL=1 additionally exercises FLUSHALL.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "live_util.h"
#include "live_core_suite.h"


int main(void) {
    const char *url = getenv("HOTKV_TEST_URL");
    hotkv_url_t u;
    if (!url || !*url) {
        printf("HOTKV_TEST_URL not set; skipping live core tests.\n");
        return 77;
    }
    if (hotkv_url_parse(url, &u) != HOTKV_OK) {
        fprintf(stderr, "invalid HOTKV_TEST_URL: %s\n", url);
        return 1;
    }
    live_init_run_id("core");
    live_core_run(&u, HOTKV_RESP2);
    live_core_run(&u, HOTKV_RESP3);
    free_keys();
    hotkv_url_free(&u);
    printf("\n=================================\n");
    printf("Results: %d ok, %d skipped, %d failed\n", g_ok, g_skipped, g_failed);
    return g_failed > 0 ? 1 : 0;
}
