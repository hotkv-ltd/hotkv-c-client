/**
 * @file hotkv_pool.c
 * @brief Connection pool implementation.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * A simple thread-safe connection pool backed by a fixed-size array.
 * On platforms with pthreads, uses a mutex + condition variable.
 * On Windows, uses CRITICAL_SECTION + CONDITION_VARIABLE.
 */

#include "hotkv_private.h"

#include <string.h>

/* =========================================================================
 * Platform mutex/condition
 * ========================================================================= */

#ifdef _WIN32
    #include <windows.h>
    typedef CRITICAL_SECTION pool_mutex_t;
    typedef CONDITION_VARIABLE pool_cond_t;

    static void pool_mutex_init(pool_mutex_t *m) { InitializeCriticalSection(m); }
    static void pool_mutex_destroy(pool_mutex_t *m) { DeleteCriticalSection(m); }
    static void pool_mutex_lock(pool_mutex_t *m) { EnterCriticalSection(m); }
    static void pool_mutex_unlock(pool_mutex_t *m) { LeaveCriticalSection(m); }
    static void pool_cond_init(pool_cond_t *c) { InitializeConditionVariable(c); }
    static void pool_cond_destroy(pool_cond_t *c) { (void)c; }
    static void pool_cond_wait(pool_cond_t *c, pool_mutex_t *m) {
        SleepConditionVariableCS(c, m, INFINITE);
    }
    static void pool_cond_signal(pool_cond_t *c) { WakeConditionVariable(c); }
#else
    #include <pthread.h>
    typedef pthread_mutex_t pool_mutex_t;
    typedef pthread_cond_t pool_cond_t;

    static void pool_mutex_init(pool_mutex_t *m) { pthread_mutex_init(m, NULL); }
    static void pool_mutex_destroy(pool_mutex_t *m) { pthread_mutex_destroy(m); }
    static void pool_mutex_lock(pool_mutex_t *m) { pthread_mutex_lock(m); }
    static void pool_mutex_unlock(pool_mutex_t *m) { pthread_mutex_unlock(m); }
    static void pool_cond_init(pool_cond_t *c) { pthread_cond_init(c, NULL); }
    static void pool_cond_destroy(pool_cond_t *c) { pthread_cond_destroy(c); }
    static void pool_cond_wait(pool_cond_t *c, pool_mutex_t *m) {
        pthread_cond_wait(c, m);
    }
    static void pool_cond_signal(pool_cond_t *c) { pthread_cond_signal(c); }
#endif

/* =========================================================================
 * Pool internals
 * ========================================================================= */

struct hotkv_pool {
    hotkv_options_t conn_opts;
    size_t          min_connections;
    size_t          max_connections;

    hotkv_context_t **idle;     /* array of idle connection pointers */
    size_t            nidle;    /* count of idle connections */
    size_t            nactive;  /* count of acquired connections */
    size_t            ntotal;   /* nidle + nactive = total created */

    pool_mutex_t     mutex;
    pool_cond_t      cond;
};

/* =========================================================================
 * Public API
 * ========================================================================= */

hotkv_pool_t *hotkv_pool_new(const hotkv_pool_options_t *opts) {
    if (!opts) return NULL;

    hotkv_pool_t *pool = (hotkv_pool_t *)hotkv_calloc(1, sizeof(hotkv_pool_t));
    if (!pool) return NULL;

    pool->conn_opts = opts->conn_opts;
    pool->min_connections = opts->min_connections > 0 ? opts->min_connections : 1;
    pool->max_connections = opts->max_connections > 0 ? opts->max_connections : 10;

    if (pool->min_connections > pool->max_connections) {
        pool->min_connections = pool->max_connections;
    }

    pool->idle = (hotkv_context_t **)hotkv_calloc(
        pool->max_connections, sizeof(hotkv_context_t *));
    if (!pool->idle) {
        hotkv_free(pool);
        return NULL;
    }

    pool_mutex_init(&pool->mutex);
    pool_cond_init(&pool->cond);

    /* Pre-create minimum idle connections. */
    for (size_t i = 0; i < pool->min_connections; i++) {
        hotkv_context_t *ctx = hotkv_connect_with_options(&pool->conn_opts);
        if (ctx && ctx->err == HOTKV_OK) {
            pool->idle[pool->nidle++] = ctx;
            pool->ntotal++;
        } else {
            hotkv_disconnect(ctx);
        }
    }

    return pool;
}

void hotkv_pool_free(hotkv_pool_t *pool) {
    if (!pool) return;

    pool_mutex_lock(&pool->mutex);

    /* Close all idle connections. */
    for (size_t i = 0; i < pool->nidle; i++) {
        hotkv_disconnect(pool->idle[i]);
    }

    pool_mutex_unlock(&pool->mutex);

    pool_cond_destroy(&pool->cond);
    pool_mutex_destroy(&pool->mutex);
    hotkv_free(pool->idle);
    hotkv_free(pool);
}

hotkv_context_t *hotkv_pool_acquire(hotkv_pool_t *pool) {
    if (!pool) return NULL;

    pool_mutex_lock(&pool->mutex);

    for (;;) {
        /* Try to grab an idle connection. */
        if (pool->nidle > 0) {
            hotkv_context_t *ctx = pool->idle[--pool->nidle];
            pool->nactive++;
            pool_mutex_unlock(&pool->mutex);

            /* Quick health check: PING. */
            hotkv_reply_t *reply = hotkv_ping(ctx);
            if (reply && reply->type != HOTKV_REPLY_ERROR) {
                hotkv_free_reply(reply);
                return ctx;
            }
            hotkv_free_reply(reply);

            /* Dead connection: discard and try again. */
            hotkv_disconnect(ctx);
            pool_mutex_lock(&pool->mutex);
            pool->nactive--;
            pool->ntotal--;
            continue;
        }

        /* Can we create a new connection? */
        if (pool->ntotal < pool->max_connections) {
            pool->ntotal++;
            pool->nactive++;
            pool_mutex_unlock(&pool->mutex);

            hotkv_context_t *ctx = hotkv_connect_with_options(&pool->conn_opts);
            if (ctx && ctx->err == HOTKV_OK) {
                return ctx;
            }

            /* Failed to connect. */
            hotkv_disconnect(ctx);
            pool_mutex_lock(&pool->mutex);
            pool->nactive--;
            pool->ntotal--;
            pool_mutex_unlock(&pool->mutex);
            return NULL;
        }

        /* Pool exhausted: wait for a release. */
        pool_cond_wait(&pool->cond, &pool->mutex);
    }
}

void hotkv_pool_release(hotkv_pool_t *pool, hotkv_context_t *ctx) {
    if (!pool || !ctx) return;

    pool_mutex_lock(&pool->mutex);

    if (ctx->err != HOTKV_OK) {
        /* Connection is broken: discard it. */
        pool->nactive--;
        pool->ntotal--;
        pool_mutex_unlock(&pool->mutex);
        hotkv_disconnect(ctx);

        /* Signal a waiter so they can create a fresh connection. */
        pool_mutex_lock(&pool->mutex);
        pool_cond_signal(&pool->cond);
        pool_mutex_unlock(&pool->mutex);
        return;
    }

    /* Return to idle pool. */
    pool->idle[pool->nidle++] = ctx;
    pool->nactive--;
    pool_cond_signal(&pool->cond);
    pool_mutex_unlock(&pool->mutex);
}

size_t hotkv_pool_active(const hotkv_pool_t *pool) {
    return pool ? pool->nactive : 0;
}

size_t hotkv_pool_idle(const hotkv_pool_t *pool) {
    return pool ? pool->nidle : 0;
}
