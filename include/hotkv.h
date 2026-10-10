/**
 * @file hotkv.h
 * @brief libhotkv: Official HotKV C Client Library
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * GitHub:  https://github.com/hotkv-ltd/hotkv-c-client
 * Website: https://hotkv.com
 *
 * @section overview Overview
 * libhotkv provides a synchronous blocking API for communicating with HotKV
 * servers over TCP using the RESP2 and RESP3 protocols. It supports TLS
 * (compile-time opt-in via HOTKV_USE_SSL), connection pooling and pipelining.
 *
 * @section usage Basic usage
 * @code
 * hotkv_context_t *ctx = hotkv_connect("127.0.0.1", 6379);
 * if (ctx->err) { fprintf(stderr, "%s\n", ctx->errstr); exit(1); }
 *
 * hotkv_reply_t *reply = hotkv_command(ctx, "SET %s %s", "key", "value");
 * hotkv_free_reply(reply);
 * hotkv_disconnect(ctx);
 * @endcode
 */

#ifndef HOTKV_H
#define HOTKV_H

/* Enable POSIX APIs (struct timeval, getaddrinfo, etc.) under strict -std=c11. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
    #define _POSIX_C_SOURCE 200112L
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Version
 * ========================================================================= */

#define HOTKV_VERSION_MAJOR 1
#define HOTKV_VERSION_MINOR 0
#define HOTKV_VERSION_PATCH 0
#define HOTKV_VERSION_STRING "1.0.0"

/* =========================================================================
 * Platform abstraction
 * ========================================================================= */

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET hotkv_fd_t;
    #define HOTKV_INVALID_FD INVALID_SOCKET
#else
    #include <sys/time.h>
    typedef int hotkv_fd_t;
    #define HOTKV_INVALID_FD (-1)
#endif

/* =========================================================================
 * Export/import macros
 * ========================================================================= */

#if defined(HOTKV_STATIC)
    #define HOTKV_API
#elif defined(_WIN32)
    #if defined(HOTKV_BUILDING)
        #define HOTKV_API __declspec(dllexport)
    #else
        #define HOTKV_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) && __GNUC__ >= 4
    #define HOTKV_API __attribute__((visibility("default")))
#else
    #define HOTKV_API
#endif

/* =========================================================================
 * Error codes
 * ========================================================================= */

/** No error. */
#define HOTKV_OK            0
/** I/O error (read/write/connect failure). */
#define HOTKV_ERR_IO        1
/** Unexpected end-of-file (server closed connection). */
#define HOTKV_ERR_EOF       2
/** RESP protocol violation. */
#define HOTKV_ERR_PROTOCOL  3
/** Memory allocation failure. */
#define HOTKV_ERR_OOM       4
/** Timeout reached. */
#define HOTKV_ERR_TIMEOUT   5
/** Catch-all error. */
#define HOTKV_ERR_OTHER     6

/** Maximum length for the human-readable error string. */
#define HOTKV_ERRSTR_LEN    128

/* =========================================================================
 * RESP reply types
 * ========================================================================= */

/** RESP2: Simple string (+) */
#define HOTKV_REPLY_STRING    1
/** RESP2: Error (-) */
#define HOTKV_REPLY_ERROR     2
/** RESP2: Integer (:) */
#define HOTKV_REPLY_INTEGER   3
/** RESP2: Bulk string ($) / RESP3: Blob string */
#define HOTKV_REPLY_BULK      4
/** RESP2: Array (*) */
#define HOTKV_REPLY_ARRAY     5
/** RESP2: Null bulk string ($-1) or null array (*-1) / RESP3: Null (_) */
#define HOTKV_REPLY_NIL       6
/** RESP3: Double (,) */
#define HOTKV_REPLY_DOUBLE    7
/** RESP3: Boolean (#) */
#define HOTKV_REPLY_BOOL      8
/** RESP3: Map (%) - stored as array with 2*N elements (key, value pairs) */
#define HOTKV_REPLY_MAP       9
/** RESP3: Set (~) - stored as array */
#define HOTKV_REPLY_SET       10
/** RESP3: Big number (() */
#define HOTKV_REPLY_BIGNUM    11
/** RESP3: Verbatim string (=) */
#define HOTKV_REPLY_VERB      12
/** RESP3: Push (>) - stored as array */
#define HOTKV_REPLY_PUSH      13
/** RESP3: Attribute (|) - stored as map with 2*N elements */
#define HOTKV_REPLY_ATTR      14

/* =========================================================================
 * RESP protocol negotiation
 * ========================================================================= */

/** Use RESP2 (default). */
#define HOTKV_RESP2  2
/** Use RESP3. */
#define HOTKV_RESP3  3

/* =========================================================================
 * Reply object
 * ========================================================================= */

/**
 * @brief Tagged union representing a parsed RESP reply.
 *
 * For HOTKV_REPLY_STRING, HOTKV_REPLY_ERROR, HOTKV_REPLY_BULK,
 * HOTKV_REPLY_VERB, HOTKV_REPLY_BIGNUM: str/len are set.
 *
 * For HOTKV_REPLY_INTEGER: integer is set.
 * For HOTKV_REPLY_DOUBLE: dval is set.
 * For HOTKV_REPLY_BOOL: integer is set (0 or 1).
 * For HOTKV_REPLY_ARRAY, HOTKV_REPLY_MAP, HOTKV_REPLY_SET,
 * HOTKV_REPLY_PUSH, HOTKV_REPLY_ATTR: elements/nelem are set.
 * For HOTKV_REPLY_NIL: no payload.
 *
 * For HOTKV_REPLY_VERB: vtype holds the 3-byte encoding prefix (e.g. "txt").
 */
typedef struct hotkv_reply {
    int type;                        /**< One of HOTKV_REPLY_* constants. */

    int64_t integer;                 /**< Integer value or boolean. */
    double dval;                     /**< Double value (RESP3). */

    char *str;                       /**< String data (heap-allocated, NUL-terminated). */
    size_t len;                      /**< Length of str (excluding NUL terminator). */

    char vtype[4];                   /**< Verbatim type prefix, e.g. "txt" (NUL-terminated). */

    struct hotkv_reply **elements;   /**< Array of child reply pointers. */
    size_t nelem;                    /**< Number of child elements. */
} hotkv_reply_t;

/**
 * Returns the number of (member, value) pairs in a WITHSCORES/WITHVALUES
 * style reply (e.g. ZRANGE ... WITHSCORES, HRANDFIELD ... WITHVALUES,
 * ZRANDMEMBER ... WITHSCORES), whichever wire shape produced it: RESP3's
 * native array of 2-element pairs, or RESP2's flat alternating array.
 * Returns 0 for a reply that is not array shaped.
 */
HOTKV_API size_t hotkv_reply_pair_count(const hotkv_reply_t *reply);

/**
 * Reads the key and value replies of pair `index` (0-based) of a
 * WITHSCORES/WITHVALUES style reply, hiding the RESP2/RESP3 wire shape
 * difference described at hotkv_reply_pair_count(). Returns 1 and sets
 * out_key and out_value on success, 0 if index is out of range or reply
 * is not a paired reply. The returned pointers are owned by reply and
 * stay valid until it is freed with hotkv_free_reply().
 */
HOTKV_API int hotkv_reply_pair_at(const hotkv_reply_t *reply, size_t index,
                                   hotkv_reply_t **out_key, hotkv_reply_t **out_value);

/* =========================================================================
 * Connection options
 * ========================================================================= */

/**
 * @brief Options for establishing a connection.
 *
 * Zero-initialize with {0} for defaults, then set the fields you need.
 */
typedef struct hotkv_options {
    const char *host;                /**< Server hostname or IP (default: "127.0.0.1"). */
    int port;                        /**< Server port (default: 6379). */

    struct timeval connect_timeout;  /**< Connection timeout ({0,0} = no timeout). */
    struct timeval command_timeout;  /**< Per-command read/write timeout. */

    int protocol;                    /**< HOTKV_RESP2 or HOTKV_RESP3 (default: RESP2). */

    const char *username;            /**< AUTH username (NULL for legacy single-password auth). */
    const char *password;            /**< AUTH password (NULL to skip AUTH). */
    int db;                          /**< Database index for SELECT after connect (default: 0). */

    size_t reader_bufsz;             /**< Initial read buffer size (default: 16 KB). */
    size_t writer_bufsz;             /**< Initial write buffer size (default: 16 KB). */

#ifdef HOTKV_USE_SSL
    const char *tls_cert;            /**< Path to client certificate (PEM). */
    const char *tls_key;             /**< Path to client private key (PEM). */
    const char *tls_ca;              /**< Path to CA certificate (PEM). */
    const char *tls_ca_dir;          /**< Path to CA certificate directory. */
    const char *tls_servername;      /**< SNI / certificate name (NULL = use host). */
    int use_tls;                     /**< Non-zero: perform the TLS handshake as part of the
                                          connect, before AUTH. Implied when tls_ca, tls_ca_dir
                                          or tls_cert is set. Without a CA the system trust
                                          store is used. */
    int tls_insecure_skip_verify;    /**< Non-zero: do not verify the server certificate or
                                          its name. For testing only. */
#endif
} hotkv_options_t;

/* =========================================================================
 * RESP reader (parser state machine)
 * ========================================================================= */

/**
 * @brief RESP protocol reader/parser.
 *
 * Can be used independently of any connection for unit testing:
 * @code
 * hotkv_reader_t *r = hotkv_reader_new();
 * hotkv_reader_feed(r, data, len);
 * hotkv_reply_t *reply;
 * while (hotkv_reader_next(r, &reply) == HOTKV_OK) {
 *     // process reply
 *     hotkv_free_reply(reply);
 * }
 * hotkv_reader_free(r);
 * @endcode
 */
typedef struct hotkv_reader hotkv_reader_t;

/**
 * @brief Create a new RESP reader.
 * @return Pointer to reader, or NULL on allocation failure.
 */
HOTKV_API hotkv_reader_t *hotkv_reader_new(void);

/**
 * @brief Create a new RESP reader with a custom initial buffer size.
 * @param bufsz Initial buffer capacity in bytes.
 * @return Pointer to reader, or NULL on allocation failure.
 */
HOTKV_API hotkv_reader_t *hotkv_reader_new_with_size(size_t bufsz);

/**
 * @brief Destroy a reader and free all associated memory.
 * @param reader Reader to free (NULL-safe).
 */
HOTKV_API void hotkv_reader_free(hotkv_reader_t *reader);

/**
 * @brief Feed raw data into the reader buffer.
 * @param reader Target reader.
 * @param buf    Pointer to data.
 * @param len    Length of data in bytes.
 * @return HOTKV_OK on success, HOTKV_ERR_OOM on allocation failure.
 */
HOTKV_API int hotkv_reader_feed(hotkv_reader_t *reader, const char *buf, size_t len);

/**
 * @brief Try to parse the next complete reply from the reader buffer.
 * @param reader Target reader.
 * @param reply  Output pointer; set to parsed reply on success, NULL if incomplete.
 * @return HOTKV_OK on success (reply may still be NULL if data is incomplete),
 *         HOTKV_ERR_PROTOCOL on parse error,
 *         HOTKV_ERR_OOM on allocation failure.
 */
HOTKV_API int hotkv_reader_next(hotkv_reader_t *reader, hotkv_reply_t **reply);

/**
 * @brief Reset reader state and discard all buffered data.
 * @param reader Target reader.
 */
HOTKV_API void hotkv_reader_reset(hotkv_reader_t *reader);

/**
 * @brief Return the number of unconsumed bytes in the reader buffer.
 * @param reader Target reader.
 * @return Number of bytes.
 */
HOTKV_API size_t hotkv_reader_buffered(const hotkv_reader_t *reader);

/* =========================================================================
 * Connection context
 * ========================================================================= */

/** Opaque connection context. */
typedef struct hotkv_context hotkv_context_t;

/**
 * @brief Connect to a HotKV server with default options.
 * @param host Server hostname or IP address.
 * @param port Server port number.
 * @return Context pointer on success (check ctx->err), or NULL on OOM.
 */
HOTKV_API hotkv_context_t *hotkv_connect(const char *host, int port);

/**
 * @brief Connect to a HotKV server with full options.
 * @param opts Pointer to options struct.
 * @return Context pointer on success (check ctx->err), or NULL on OOM.
 */
HOTKV_API hotkv_context_t *hotkv_connect_with_options(const hotkv_options_t *opts);

/**
 * @brief Connect from a URL of the form
 * `(hotkv|hotkvs|redis|rediss)://[user[:pass]@]host[:port][/db]` (see
 * hotkv_url_parse). `hotkvs://` and `rediss://` negotiate TLS before anything
 * else is sent, so a password in the URL never crosses the network in clear.
 * @param url      Connection URL.
 * @param overrides Optional settings that are not part of a URL (timeouts,
 *                 protocol, TLS CA/client certificate, buffer sizes); host, port,
 *                 credentials and database come from the URL. NULL for defaults.
 * @return Context pointer (check hotkv_context_err), or NULL on out of memory.
 *         A malformed URL, or a TLS URL on a build without HOTKV_USE_SSL, yields a
 *         context whose error is set.
 */
HOTKV_API hotkv_context_t *hotkv_connect_url(const char *url, const hotkv_options_t *overrides);

/**
 * @brief Disconnect and free the context.
 * @param ctx Context to free (NULL-safe).
 */
HOTKV_API void hotkv_disconnect(hotkv_context_t *ctx);

/**
 * @brief Get the error code from the context.
 * @param ctx Context.
 * @return One of HOTKV_OK or HOTKV_ERR_* constants.
 */
HOTKV_API int hotkv_context_err(const hotkv_context_t *ctx);

/**
 * @brief Get the human-readable error string from the context.
 * @param ctx Context.
 * @return NUL-terminated error string.
 */
HOTKV_API const char *hotkv_context_errstr(const hotkv_context_t *ctx);

/**
 * @brief Get the underlying socket file descriptor.
 * @param ctx Context.
 * @return Socket fd, or HOTKV_INVALID_FD if not connected.
 */
HOTKV_API hotkv_fd_t hotkv_context_fd(const hotkv_context_t *ctx);

/**
 * @brief Set a timeout for subsequent read/write operations.
 * @param ctx Context.
 * @param tv  Timeout value.
 * @return HOTKV_OK on success, HOTKV_ERR_IO on failure.
 */
HOTKV_API int hotkv_set_timeout(hotkv_context_t *ctx, struct timeval tv);

/* =========================================================================
 * Generic command execution
 * ========================================================================= */

/**
 * @brief Execute a command using printf-style format.
 *
 * Format specifiers:
 *   %s  - C string (NUL-terminated)
 *   %b  - Binary-safe string (const char*, size_t) pair
 *   %d  - int
 *   %lld - long long
 *   %%  - Literal percent
 *
 * @param ctx Context.
 * @param format Format string.
 * @return Reply object, or NULL on error (check ctx->err).
 */
HOTKV_API hotkv_reply_t *hotkv_command(hotkv_context_t *ctx, const char *format, ...);

/**
 * @brief Execute a command from a pre-built argument vector.
 * @param ctx    Context.
 * @param argc   Number of arguments.
 * @param argv   Array of argument strings.
 * @param argvlen Array of argument lengths (NULL = use strlen).
 * @return Reply object, or NULL on error.
 */
HOTKV_API hotkv_reply_t *hotkv_command_argv(hotkv_context_t *ctx, int argc,
                                             const char **argv, const size_t *argvlen);

/* =========================================================================
 * Pipelining
 * ========================================================================= */

/**
 * @brief Append a command to the output buffer without reading a reply.
 * @param ctx Context.
 * @param format Format string (same specifiers as hotkv_command).
 * @return HOTKV_OK on success, error code on failure.
 */
HOTKV_API int hotkv_append_command(hotkv_context_t *ctx, const char *format, ...);

/**
 * @brief Append a command from an argument vector.
 * @param ctx     Context.
 * @param argc    Number of arguments.
 * @param argv    Argument strings.
 * @param argvlen Argument lengths (NULL = use strlen).
 * @return HOTKV_OK on success, error code on failure.
 */
HOTKV_API int hotkv_append_command_argv(hotkv_context_t *ctx, int argc,
                                         const char **argv, const size_t *argvlen);

/**
 * @brief Read the next reply from the server (for pipelined commands).
 * @param ctx   Context.
 * @param reply Output pointer for the reply.
 * @return HOTKV_OK on success, error code on failure.
 */
HOTKV_API int hotkv_get_reply(hotkv_context_t *ctx, hotkv_reply_t **reply);

/* =========================================================================
 * Memory management
 * ========================================================================= */

/**
 * @brief Free a reply object and all its children.
 * @param reply Reply to free (NULL-safe).
 */
HOTKV_API void hotkv_free_reply(hotkv_reply_t *reply);

/**
 * @brief Custom allocator function signatures.
 */
typedef void *(*hotkv_malloc_fn)(size_t);
typedef void *(*hotkv_calloc_fn)(size_t, size_t);
typedef void *(*hotkv_realloc_fn)(void *, size_t);
typedef void  (*hotkv_free_fn)(void *);

/**
 * @brief Set custom memory allocator functions.
 *
 * Must be called before any other hotkv function. Not thread-safe.
 *
 * @param malloc_fn  Custom malloc.
 * @param calloc_fn  Custom calloc.
 * @param realloc_fn Custom realloc.
 * @param free_fn    Custom free.
 */
HOTKV_API void hotkv_set_allocator(hotkv_malloc_fn malloc_fn,
                                    hotkv_calloc_fn calloc_fn,
                                    hotkv_realloc_fn realloc_fn,
                                    hotkv_free_fn free_fn);

/* =========================================================================
 * String commands
 * ========================================================================= */

HOTKV_API hotkv_reply_t *hotkv_set(hotkv_context_t *ctx,
                                    const char *key, size_t keylen,
                                    const char *val, size_t vallen);

HOTKV_API hotkv_reply_t *hotkv_get(hotkv_context_t *ctx,
                                    const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_del(hotkv_context_t *ctx,
                                    const char **keys, const size_t *keylens,
                                    size_t nkeys);

HOTKV_API hotkv_reply_t *hotkv_mset(hotkv_context_t *ctx,
                                     const char **keys, const size_t *keylens,
                                     const char **vals, const size_t *vallens,
                                     size_t npairs);

HOTKV_API hotkv_reply_t *hotkv_mget(hotkv_context_t *ctx,
                                     const char **keys, const size_t *keylens,
                                     size_t nkeys);

HOTKV_API hotkv_reply_t *hotkv_incr(hotkv_context_t *ctx,
                                     const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_decr(hotkv_context_t *ctx,
                                     const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_incrby(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       int64_t increment);

HOTKV_API hotkv_reply_t *hotkv_decrby(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       int64_t decrement);

HOTKV_API hotkv_reply_t *hotkv_incrbyfloat(hotkv_context_t *ctx,
                                            const char *key, size_t keylen,
                                            double increment);

HOTKV_API hotkv_reply_t *hotkv_append(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       const char *val, size_t vallen);

HOTKV_API hotkv_reply_t *hotkv_strlen(hotkv_context_t *ctx,
                                       const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_setnx(hotkv_context_t *ctx,
                                      const char *key, size_t keylen,
                                      const char *val, size_t vallen);

HOTKV_API hotkv_reply_t *hotkv_setex(hotkv_context_t *ctx,
                                      const char *key, size_t keylen,
                                      int64_t seconds,
                                      const char *val, size_t vallen);

HOTKV_API hotkv_reply_t *hotkv_psetex(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       int64_t milliseconds,
                                       const char *val, size_t vallen);

HOTKV_API hotkv_reply_t *hotkv_getdel(hotkv_context_t *ctx,
                                       const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_getrange(hotkv_context_t *ctx,
                                         const char *key, size_t keylen,
                                         int64_t start, int64_t end);

HOTKV_API hotkv_reply_t *hotkv_setrange(hotkv_context_t *ctx,
                                         const char *key, size_t keylen,
                                         int64_t offset,
                                         const char *val, size_t vallen);

HOTKV_API hotkv_reply_t *hotkv_getset(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       const char *val, size_t vallen);

/** GETEX key [EX seconds | PX ms | EXAT ts | PXAT ts | PERSIST]. expire_type
 *  NULL or "" sends plain GETEX; for "PERSIST" expire_value is ignored. */
HOTKV_API hotkv_reply_t *hotkv_getex(hotkv_context_t *ctx,
                                      const char *key, size_t keylen,
                                      const char *expire_type,
                                      int64_t expire_value);

HOTKV_API hotkv_reply_t *hotkv_msetnx(hotkv_context_t *ctx,
                                       const char **keys, const size_t *keylens,
                                       const char **vals, const size_t *vallens,
                                       size_t npairs);

/* =========================================================================
 * List commands
 * ========================================================================= */

HOTKV_API hotkv_reply_t *hotkv_lpush(hotkv_context_t *ctx,
                                      const char *key, size_t keylen,
                                      const char **vals, const size_t *vallens,
                                      size_t nvals);

HOTKV_API hotkv_reply_t *hotkv_rpush(hotkv_context_t *ctx,
                                      const char *key, size_t keylen,
                                      const char **vals, const size_t *vallens,
                                      size_t nvals);

HOTKV_API hotkv_reply_t *hotkv_lpop(hotkv_context_t *ctx,
                                     const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_rpop(hotkv_context_t *ctx,
                                     const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_llen(hotkv_context_t *ctx,
                                     const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_lrange(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       int64_t start, int64_t stop);

HOTKV_API hotkv_reply_t *hotkv_lindex(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       int64_t index);

HOTKV_API hotkv_reply_t *hotkv_lset(hotkv_context_t *ctx,
                                     const char *key, size_t keylen,
                                     int64_t index,
                                     const char *val, size_t vallen);

HOTKV_API hotkv_reply_t *hotkv_lrem(hotkv_context_t *ctx,
                                     const char *key, size_t keylen,
                                     int64_t count,
                                     const char *val, size_t vallen);

/* =========================================================================
 * Set commands
 * ========================================================================= */

HOTKV_API hotkv_reply_t *hotkv_sadd(hotkv_context_t *ctx,
                                     const char *key, size_t keylen,
                                     const char **members, const size_t *memlens,
                                     size_t nmembers);

HOTKV_API hotkv_reply_t *hotkv_srem(hotkv_context_t *ctx,
                                     const char *key, size_t keylen,
                                     const char **members, const size_t *memlens,
                                     size_t nmembers);

HOTKV_API hotkv_reply_t *hotkv_smembers(hotkv_context_t *ctx,
                                         const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_sismember(hotkv_context_t *ctx,
                                          const char *key, size_t keylen,
                                          const char *member, size_t memlen);

HOTKV_API hotkv_reply_t *hotkv_scard(hotkv_context_t *ctx,
                                      const char *key, size_t keylen);

/* =========================================================================
 * Hash commands
 * ========================================================================= */

HOTKV_API hotkv_reply_t *hotkv_hset(hotkv_context_t *ctx,
                                     const char *key, size_t keylen,
                                     const char **fields, const size_t *fieldlens,
                                     const char **vals, const size_t *vallens,
                                     size_t npairs);

HOTKV_API hotkv_reply_t *hotkv_hget(hotkv_context_t *ctx,
                                     const char *key, size_t keylen,
                                     const char *field, size_t fieldlen);

HOTKV_API hotkv_reply_t *hotkv_hdel(hotkv_context_t *ctx,
                                     const char *key, size_t keylen,
                                     const char **fields, const size_t *fieldlens,
                                     size_t nfields);

HOTKV_API hotkv_reply_t *hotkv_hexists(hotkv_context_t *ctx,
                                        const char *key, size_t keylen,
                                        const char *field, size_t fieldlen);

HOTKV_API hotkv_reply_t *hotkv_hlen(hotkv_context_t *ctx,
                                     const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_hgetall(hotkv_context_t *ctx,
                                        const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_hmget(hotkv_context_t *ctx,
                                      const char *key, size_t keylen,
                                      const char **fields, const size_t *fieldlens,
                                      size_t nfields);

HOTKV_API hotkv_reply_t *hotkv_hincrby(hotkv_context_t *ctx,
                                        const char *key, size_t keylen,
                                        const char *field, size_t fieldlen,
                                        int64_t increment);

HOTKV_API hotkv_reply_t *hotkv_hincrbyfloat(hotkv_context_t *ctx,
                                              const char *key, size_t keylen,
                                              const char *field, size_t fieldlen,
                                              double increment);

/** HRANDFIELD key: one random field, or a nil reply if key does not exist. */
HOTKV_API hotkv_reply_t *hotkv_hrandfield(hotkv_context_t *ctx,
                                           const char *key, size_t keylen);

/**
 * HRANDFIELD key count [WITHVALUES]. A negative count allows repeats and
 * always returns exactly -count fields. Read a WITHVALUES reply with
 * hotkv_reply_pair_count()/hotkv_reply_pair_at().
 */
HOTKV_API hotkv_reply_t *hotkv_hrandfield_count(hotkv_context_t *ctx,
                                                 const char *key, size_t keylen,
                                                 int64_t count, int withvalues);

/* =========================================================================
 * Sorted set commands
 * ========================================================================= */

HOTKV_API hotkv_reply_t *hotkv_zadd(hotkv_context_t *ctx,
                                     const char *key, size_t keylen,
                                     const double *scores,
                                     const char **members, const size_t *memlens,
                                     size_t nmembers);

HOTKV_API hotkv_reply_t *hotkv_zrem(hotkv_context_t *ctx,
                                     const char *key, size_t keylen,
                                     const char **members, const size_t *memlens,
                                     size_t nmembers);

HOTKV_API hotkv_reply_t *hotkv_zscore(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       const char *member, size_t memlen);

HOTKV_API hotkv_reply_t *hotkv_zrank(hotkv_context_t *ctx,
                                      const char *key, size_t keylen,
                                      const char *member, size_t memlen);

HOTKV_API hotkv_reply_t *hotkv_zcard(hotkv_context_t *ctx,
                                      const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_zrange(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       int64_t start, int64_t stop,
                                       int withscores);

HOTKV_API hotkv_reply_t *hotkv_zcount(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       const char *min, const char *max);

/** ZPOPMIN key [count]: pass count < 0 to omit it (pops a single member). */
HOTKV_API hotkv_reply_t *hotkv_zpopmin(hotkv_context_t *ctx,
                                        const char *key, size_t keylen,
                                        int64_t count);

/** ZPOPMAX key [count]: pass count < 0 to omit it (pops a single member). */
HOTKV_API hotkv_reply_t *hotkv_zpopmax(hotkv_context_t *ctx,
                                        const char *key, size_t keylen,
                                        int64_t count);

/** ZRANDMEMBER key: one random member, or a nil reply if key does not exist. */
HOTKV_API hotkv_reply_t *hotkv_zrandmember(hotkv_context_t *ctx,
                                            const char *key, size_t keylen);

/**
 * ZRANDMEMBER key count [WITHSCORES]. A negative count allows repeats and
 * always returns exactly -count members. Read a WITHSCORES reply with
 * hotkv_reply_pair_count()/hotkv_reply_pair_at().
 */
HOTKV_API hotkv_reply_t *hotkv_zrandmember_count(hotkv_context_t *ctx,
                                                  const char *key, size_t keylen,
                                                  int64_t count, int withscores);

/* =========================================================================
 * Key commands
 * ========================================================================= */

HOTKV_API hotkv_reply_t *hotkv_exists(hotkv_context_t *ctx,
                                       const char **keys, const size_t *keylens,
                                       size_t nkeys);

HOTKV_API hotkv_reply_t *hotkv_expire(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       int64_t seconds);

HOTKV_API hotkv_reply_t *hotkv_pexpire(hotkv_context_t *ctx,
                                        const char *key, size_t keylen,
                                        int64_t milliseconds);

HOTKV_API hotkv_reply_t *hotkv_ttl(hotkv_context_t *ctx,
                                    const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_pttl(hotkv_context_t *ctx,
                                     const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_persist(hotkv_context_t *ctx,
                                        const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_type(hotkv_context_t *ctx,
                                     const char *key, size_t keylen);

HOTKV_API hotkv_reply_t *hotkv_rename(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       const char *newkey, size_t newkeylen);

HOTKV_API hotkv_reply_t *hotkv_keys(hotkv_context_t *ctx,
                                     const char *pattern, size_t patternlen);

HOTKV_API hotkv_reply_t *hotkv_scan(hotkv_context_t *ctx,
                                     uint64_t cursor,
                                     const char *pattern, size_t patternlen,
                                     int64_t count);

/* =========================================================================
 * Server commands
 * ========================================================================= */

HOTKV_API hotkv_reply_t *hotkv_ping(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_echo(hotkv_context_t *ctx,
                                     const char *msg, size_t msglen);
HOTKV_API hotkv_reply_t *hotkv_dbsize(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_flushdb(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_flushall(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_info(hotkv_context_t *ctx, const char *section);
HOTKV_API hotkv_reply_t *hotkv_select(hotkv_context_t *ctx, int db);

/**
 * CONFIG GET parameter. Replies as a RESP3 map (2*N elements, matching
 * RESP2's flat array; see HOTKV_REPLY_MAP) of the matching parameter
 * names and their current values.
 */
HOTKV_API hotkv_reply_t *hotkv_config_get(hotkv_context_t *ctx,
                                           const char *parameter, size_t paramlen);

/* =========================================================================
 * Transaction commands
 * ========================================================================= */

HOTKV_API hotkv_reply_t *hotkv_multi(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_exec(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_discard(hotkv_context_t *ctx);

/* =========================================================================
 * Pub/Sub commands
 * ========================================================================= */

HOTKV_API hotkv_reply_t *hotkv_publish(hotkv_context_t *ctx,
                                        const char *channel, size_t channellen,
                                        const char *message, size_t messagelen);

/* =========================================================================
 * Stream commands
 * ========================================================================= */

/**
 * XADD key id field value [field value ...]. Pass id "*" (idlen 1) to let
 * the server assign the entry ID. Replies with the assigned ID as a bulk
 * string.
 */
HOTKV_API hotkv_reply_t *hotkv_xadd(hotkv_context_t *ctx,
                                     const char *key, size_t keylen,
                                     const char *id, size_t idlen,
                                     const char **fields, const size_t *fieldlens,
                                     const char **vals, const size_t *vallens,
                                     size_t npairs);

/** XLEN key. */
HOTKV_API hotkv_reply_t *hotkv_xlen(hotkv_context_t *ctx,
                                     const char *key, size_t keylen);

/**
 * XRANGE key start end [COUNT count]. Pass count <= 0 to omit COUNT.
 * Replies with an array of [id, [field, value, ...]] entries.
 */
HOTKV_API hotkv_reply_t *hotkv_xrange(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       const char *start, size_t startlen,
                                       const char *end, size_t endlen,
                                       int64_t count);

/**
 * XREAD [COUNT count] STREAMS key [key ...] id [id ...]. Pass count <= 0
 * to omit COUNT. Replies as a RESP3 map of stream name to its entries
 * under HELLO 3, or an array of [name, entries] pairs under RESP2; read
 * either shape with hotkv_reply_pair_count()/hotkv_reply_pair_at(), or a
 * nil reply if no stream had new entries.
 */
HOTKV_API hotkv_reply_t *hotkv_xread(hotkv_context_t *ctx,
                                      const char **keys, const size_t *keylens,
                                      const char **ids, const size_t *idlens,
                                      size_t nstreams, int64_t count);

/**
 * XINFO STREAM key. Replies as a RESP3 map (2*N elements, matching
 * RESP2's flat array; see HOTKV_REPLY_MAP) of the stream's metadata
 * fields (length, last-generated-id, first-entry, last-entry, ...).
 */
HOTKV_API hotkv_reply_t *hotkv_xinfo_stream(hotkv_context_t *ctx,
                                             const char *key, size_t keylen);

/* =========================================================================
 * Extended data-structure commands
 *
 * Typed wrappers for the remaining core families (keys, bitmaps, lists,
 * sets, hashes, sorted sets, geo, HyperLogLog, streams, scripting, pub/sub
 * subscription). Conventions, the same everywhere in this section:
 *
 *   - Keys, members, values and fields are binary safe (pointer + length).
 *     Ranges, scores bounds, stream IDs, units and keywords are plain
 *     NUL-terminated strings ("-inf", "(5", "0-0", "km", "LEFT").
 *   - Several keys/members are passed as parallel arrays (const char **,
 *     const size_t *) plus a count.
 *   - Optional integer arguments take a negative value to mean "omit" where
 *     a negative value is not meaningful; optional strings take NULL.
 *   - Every function returns the raw reply (free with hotkv_free_reply) or
 *     NULL on a transport error (see hotkv_context_err). A numeric argument
 *     that cannot be sent (NaN, or a negative/non-finite USD amount) comes
 *     back as a local error reply ("ERR invalid numeric argument ...") and
 *     nothing is written to the connection, which stays usable.
 *   - Blocking commands (BLPOP, BRPOP, BLMOVE, BZPOP*, XREAD BLOCK ...) have
 *     no typed wrapper: use hotkv_command_argv() on a connection with a
 *     suitable hotkv_set_timeout().
 *
 * Commands not listed here (ACL, CLIENT, CLUSTER, DEBUG, FUNCTION, MODULE,
 * OBJECT, MEMORY, SLOWLOG, SORT, SCRIPT DEBUG, ...) go through
 * hotkv_command()/hotkv_command_argv().
 * ========================================================================= */

/* ----- Keys ----- */
HOTKV_API hotkv_reply_t *hotkv_unlink(hotkv_context_t *ctx, const char **keys,
                                       const size_t *keylens, size_t nkeys);
HOTKV_API hotkv_reply_t *hotkv_touch(hotkv_context_t *ctx, const char **keys,
                                      const size_t *keylens, size_t nkeys);
HOTKV_API hotkv_reply_t *hotkv_expireat(hotkv_context_t *ctx, const char *key, size_t keylen,
                                         int64_t unix_seconds);
HOTKV_API hotkv_reply_t *hotkv_pexpireat(hotkv_context_t *ctx, const char *key, size_t keylen,
                                          int64_t unix_milliseconds);
HOTKV_API hotkv_reply_t *hotkv_expiretime(hotkv_context_t *ctx, const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_pexpiretime(hotkv_context_t *ctx, const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_renamenx(hotkv_context_t *ctx, const char *key, size_t keylen,
                                         const char *newkey, size_t newkeylen);
/** COPY source destination [REPLACE]. */
HOTKV_API hotkv_reply_t *hotkv_copy(hotkv_context_t *ctx, const char *src, size_t srclen,
                                     const char *dst, size_t dstlen, int replace);
HOTKV_API hotkv_reply_t *hotkv_move(hotkv_context_t *ctx, const char *key, size_t keylen, int db);
HOTKV_API hotkv_reply_t *hotkv_randomkey(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_dump(hotkv_context_t *ctx, const char *key, size_t keylen);
/** RESTORE key ttl_ms payload [REPLACE]. ttl_ms 0 means no expiry. */
HOTKV_API hotkv_reply_t *hotkv_restore(hotkv_context_t *ctx, const char *key, size_t keylen,
                                        int64_t ttl_ms, const char *payload, size_t payloadlen,
                                        int replace);
HOTKV_API hotkv_reply_t *hotkv_watch(hotkv_context_t *ctx, const char **keys,
                                      const size_t *keylens, size_t nkeys);
HOTKV_API hotkv_reply_t *hotkv_unwatch(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_time(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_lastsave(hotkv_context_t *ctx);

/* ----- Bitmaps ----- */
HOTKV_API hotkv_reply_t *hotkv_setbit(hotkv_context_t *ctx, const char *key, size_t keylen,
                                       int64_t offset, int bit);
HOTKV_API hotkv_reply_t *hotkv_getbit(hotkv_context_t *ctx, const char *key, size_t keylen,
                                       int64_t offset);
/** BITCOUNT key [start end [BYTE|BIT]]. mode NULL omits the unit (bytes);
 *  has_range 0 counts the whole string and ignores start/end/mode. */
HOTKV_API hotkv_reply_t *hotkv_bitcount(hotkv_context_t *ctx, const char *key, size_t keylen,
                                         int has_range, int64_t start, int64_t end,
                                         const char *mode);
/** BITPOS key bit [start [end [BYTE|BIT]]]. start/end < 0 are sent as given
 *  (negative offsets are valid); use has_start/has_end to omit them. */
HOTKV_API hotkv_reply_t *hotkv_bitpos(hotkv_context_t *ctx, const char *key, size_t keylen,
                                       int bit, int has_start, int64_t start, int has_end,
                                       int64_t end, const char *mode);
/** BITOP operation destkey key [key ...]; operation is AND, OR, XOR or NOT. */
HOTKV_API hotkv_reply_t *hotkv_bitop(hotkv_context_t *ctx, const char *operation,
                                      const char *dest, size_t destlen, const char **keys,
                                      const size_t *keylens, size_t nkeys);
/** LCS key1 key2 [LEN]. */
HOTKV_API hotkv_reply_t *hotkv_lcs(hotkv_context_t *ctx, const char *key1, size_t key1len,
                                    const char *key2, size_t key2len, int len_only);

/* ----- Lists ----- */
HOTKV_API hotkv_reply_t *hotkv_ltrim(hotkv_context_t *ctx, const char *key, size_t keylen,
                                      int64_t start, int64_t stop);
/** LINSERT key BEFORE|AFTER pivot value. */
HOTKV_API hotkv_reply_t *hotkv_linsert(hotkv_context_t *ctx, const char *key, size_t keylen,
                                        int before, const char *pivot, size_t pivotlen,
                                        const char *value, size_t valuelen);
/** LPOS key element [RANK rank] [COUNT count] [MAXLEN maxlen]. rank 0, count < 0
 *  and maxlen < 0 omit the option (count 0 means "all matches"). */
HOTKV_API hotkv_reply_t *hotkv_lpos(hotkv_context_t *ctx, const char *key, size_t keylen,
                                     const char *element, size_t elementlen, int64_t rank,
                                     int64_t count, int64_t maxlen);
/** LMOVE source destination LEFT|RIGHT LEFT|RIGHT. */
HOTKV_API hotkv_reply_t *hotkv_lmove(hotkv_context_t *ctx, const char *src, size_t srclen,
                                      const char *dst, size_t dstlen, const char *wherefrom,
                                      const char *whereto);
HOTKV_API hotkv_reply_t *hotkv_rpoplpush(hotkv_context_t *ctx, const char *src, size_t srclen,
                                          const char *dst, size_t dstlen);
HOTKV_API hotkv_reply_t *hotkv_lpushx(hotkv_context_t *ctx, const char *key, size_t keylen,
                                       const char **vals, const size_t *vallens, size_t nvals);
HOTKV_API hotkv_reply_t *hotkv_rpushx(hotkv_context_t *ctx, const char *key, size_t keylen,
                                       const char **vals, const size_t *vallens, size_t nvals);
/** LPOP key count / RPOP key count (an array reply, or nil if the key is missing). */
HOTKV_API hotkv_reply_t *hotkv_lpop_count(hotkv_context_t *ctx, const char *key, size_t keylen,
                                           int64_t count);
HOTKV_API hotkv_reply_t *hotkv_rpop_count(hotkv_context_t *ctx, const char *key, size_t keylen,
                                           int64_t count);

/* ----- Sets ----- */
HOTKV_API hotkv_reply_t *hotkv_sdiff(hotkv_context_t *ctx, const char **keys,
                                      const size_t *keylens, size_t nkeys);
HOTKV_API hotkv_reply_t *hotkv_sinter(hotkv_context_t *ctx, const char **keys,
                                       const size_t *keylens, size_t nkeys);
HOTKV_API hotkv_reply_t *hotkv_sunion(hotkv_context_t *ctx, const char **keys,
                                       const size_t *keylens, size_t nkeys);
HOTKV_API hotkv_reply_t *hotkv_sdiffstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                           const char **keys, const size_t *keylens,
                                           size_t nkeys);
HOTKV_API hotkv_reply_t *hotkv_sinterstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                            const char **keys, const size_t *keylens,
                                            size_t nkeys);
HOTKV_API hotkv_reply_t *hotkv_sunionstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                            const char **keys, const size_t *keylens,
                                            size_t nkeys);
/** SINTERCARD numkeys key [key ...] [LIMIT limit]; limit < 0 omits LIMIT. */
HOTKV_API hotkv_reply_t *hotkv_sintercard(hotkv_context_t *ctx, const char **keys,
                                           const size_t *keylens, size_t nkeys, int64_t limit);
HOTKV_API hotkv_reply_t *hotkv_smismember(hotkv_context_t *ctx, const char *key, size_t keylen,
                                           const char **members, const size_t *memlens,
                                           size_t nmembers);
HOTKV_API hotkv_reply_t *hotkv_smove(hotkv_context_t *ctx, const char *src, size_t srclen,
                                      const char *dst, size_t dstlen, const char *member,
                                      size_t memlen);
HOTKV_API hotkv_reply_t *hotkv_spop(hotkv_context_t *ctx, const char *key, size_t keylen);
/** SPOP key count. */
HOTKV_API hotkv_reply_t *hotkv_spop_count(hotkv_context_t *ctx, const char *key, size_t keylen,
                                           int64_t count);
HOTKV_API hotkv_reply_t *hotkv_srandmember(hotkv_context_t *ctx, const char *key, size_t keylen);
/** SRANDMEMBER key count (negative count allows repeats). */
HOTKV_API hotkv_reply_t *hotkv_srandmember_count(hotkv_context_t *ctx, const char *key,
                                                  size_t keylen, int64_t count);
/** SSCAN key cursor [MATCH pattern] [COUNT count]; pattern NULL / count <= 0 omit. */
HOTKV_API hotkv_reply_t *hotkv_sscan(hotkv_context_t *ctx, const char *key, size_t keylen,
                                      uint64_t cursor, const char *pattern, int64_t count);

/* ----- Hashes ----- */
HOTKV_API hotkv_reply_t *hotkv_hsetnx(hotkv_context_t *ctx, const char *key, size_t keylen,
                                       const char *field, size_t fieldlen, const char *val,
                                       size_t vallen);
HOTKV_API hotkv_reply_t *hotkv_hmset(hotkv_context_t *ctx, const char *key, size_t keylen,
                                      const char **fields, const size_t *fieldlens,
                                      const char **vals, const size_t *vallens, size_t npairs);
HOTKV_API hotkv_reply_t *hotkv_hkeys(hotkv_context_t *ctx, const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_hvals(hotkv_context_t *ctx, const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_hstrlen(hotkv_context_t *ctx, const char *key, size_t keylen,
                                        const char *field, size_t fieldlen);
/** HSCAN key cursor [MATCH pattern] [COUNT count]; pattern NULL / count <= 0 omit. */
HOTKV_API hotkv_reply_t *hotkv_hscan(hotkv_context_t *ctx, const char *key, size_t keylen,
                                      uint64_t cursor, const char *pattern, int64_t count);
/** HGETDEL key FIELDS numfields field [field ...]. */
HOTKV_API hotkv_reply_t *hotkv_hgetdel(hotkv_context_t *ctx, const char *key, size_t keylen,
                                        const char **fields, const size_t *fieldlens,
                                        size_t nfields);
/** HEXPIRE key seconds [NX|XX|GT|LT] FIELDS numfields field [field ...]
 *  (condition NULL omits it). */
HOTKV_API hotkv_reply_t *hotkv_hexpire(hotkv_context_t *ctx, const char *key, size_t keylen,
                                        int64_t seconds, const char *condition,
                                        const char **fields, const size_t *fieldlens,
                                        size_t nfields);
HOTKV_API hotkv_reply_t *hotkv_hpexpire(hotkv_context_t *ctx, const char *key, size_t keylen,
                                         int64_t milliseconds, const char *condition,
                                         const char **fields, const size_t *fieldlens,
                                         size_t nfields);
/** HTTL / HPTTL / HPERSIST / HEXPIRETIME / HPEXPIRETIME key FIELDS numfields field ... */
HOTKV_API hotkv_reply_t *hotkv_httl(hotkv_context_t *ctx, const char *key, size_t keylen,
                                     const char **fields, const size_t *fieldlens, size_t nfields);
HOTKV_API hotkv_reply_t *hotkv_hpttl(hotkv_context_t *ctx, const char *key, size_t keylen,
                                      const char **fields, const size_t *fieldlens,
                                      size_t nfields);
HOTKV_API hotkv_reply_t *hotkv_hpersist(hotkv_context_t *ctx, const char *key, size_t keylen,
                                         const char **fields, const size_t *fieldlens,
                                         size_t nfields);
HOTKV_API hotkv_reply_t *hotkv_hexpiretime(hotkv_context_t *ctx, const char *key, size_t keylen,
                                            const char **fields, const size_t *fieldlens,
                                            size_t nfields);
HOTKV_API hotkv_reply_t *hotkv_hpexpiretime(hotkv_context_t *ctx, const char *key, size_t keylen,
                                             const char **fields, const size_t *fieldlens,
                                             size_t nfields);

/* ----- Sorted sets ----- */
HOTKV_API hotkv_reply_t *hotkv_zincrby(hotkv_context_t *ctx, const char *key, size_t keylen,
                                        double increment, const char *member, size_t memlen);
HOTKV_API hotkv_reply_t *hotkv_zmscore(hotkv_context_t *ctx, const char *key, size_t keylen,
                                        const char **members, const size_t *memlens,
                                        size_t nmembers);
HOTKV_API hotkv_reply_t *hotkv_zrevrank(hotkv_context_t *ctx, const char *key, size_t keylen,
                                         const char *member, size_t memlen);
HOTKV_API hotkv_reply_t *hotkv_zrevrange(hotkv_context_t *ctx, const char *key, size_t keylen,
                                          int64_t start, int64_t stop, int withscores);
/** ZRANGEBYSCORE / ZREVRANGEBYSCORE key min max [WITHSCORES] [LIMIT offset count].
 *  (ZREVRANGEBYSCORE takes max first, as the server does.) limit_count < 0 omits LIMIT. */
HOTKV_API hotkv_reply_t *hotkv_zrangebyscore(hotkv_context_t *ctx, const char *key, size_t keylen,
                                              const char *min, const char *max, int withscores,
                                              int64_t limit_offset, int64_t limit_count);
HOTKV_API hotkv_reply_t *hotkv_zrevrangebyscore(hotkv_context_t *ctx, const char *key,
                                                 size_t keylen, const char *max, const char *min,
                                                 int withscores, int64_t limit_offset,
                                                 int64_t limit_count);
/** ZRANGEBYLEX / ZREVRANGEBYLEX key min max [LIMIT offset count]. */
HOTKV_API hotkv_reply_t *hotkv_zrangebylex(hotkv_context_t *ctx, const char *key, size_t keylen,
                                            const char *min, const char *max,
                                            int64_t limit_offset, int64_t limit_count);
HOTKV_API hotkv_reply_t *hotkv_zrevrangebylex(hotkv_context_t *ctx, const char *key, size_t keylen,
                                               const char *max, const char *min,
                                               int64_t limit_offset, int64_t limit_count);
HOTKV_API hotkv_reply_t *hotkv_zlexcount(hotkv_context_t *ctx, const char *key, size_t keylen,
                                          const char *min, const char *max);
HOTKV_API hotkv_reply_t *hotkv_zremrangebyscore(hotkv_context_t *ctx, const char *key,
                                                 size_t keylen, const char *min, const char *max);
HOTKV_API hotkv_reply_t *hotkv_zremrangebyrank(hotkv_context_t *ctx, const char *key,
                                                size_t keylen, int64_t start, int64_t stop);
HOTKV_API hotkv_reply_t *hotkv_zremrangebylex(hotkv_context_t *ctx, const char *key,
                                               size_t keylen, const char *min, const char *max);
/** ZUNIONSTORE / ZINTERSTORE dest numkeys key ... [WEIGHTS w ...] [AGGREGATE SUM|MIN|MAX].
 *  weights NULL omits WEIGHTS (otherwise one per key); aggregate NULL omits it. */
HOTKV_API hotkv_reply_t *hotkv_zunionstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                            const char **keys, const size_t *keylens,
                                            size_t nkeys, const double *weights,
                                            const char *aggregate);
HOTKV_API hotkv_reply_t *hotkv_zinterstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                            const char **keys, const size_t *keylens,
                                            size_t nkeys, const double *weights,
                                            const char *aggregate);
/** ZUNION / ZINTER numkeys key ... [WEIGHTS ...] [AGGREGATE ...] [WITHSCORES]. */
HOTKV_API hotkv_reply_t *hotkv_zunion(hotkv_context_t *ctx, const char **keys,
                                       const size_t *keylens, size_t nkeys, const double *weights,
                                       const char *aggregate, int withscores);
HOTKV_API hotkv_reply_t *hotkv_zinter(hotkv_context_t *ctx, const char **keys,
                                       const size_t *keylens, size_t nkeys, const double *weights,
                                       const char *aggregate, int withscores);
/** ZDIFF numkeys key ... [WITHSCORES] / ZDIFFSTORE dest numkeys key ... */
HOTKV_API hotkv_reply_t *hotkv_zdiff(hotkv_context_t *ctx, const char **keys,
                                      const size_t *keylens, size_t nkeys, int withscores);
HOTKV_API hotkv_reply_t *hotkv_zdiffstore(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                           const char **keys, const size_t *keylens,
                                           size_t nkeys);
/** ZINTERCARD numkeys key ... [LIMIT limit]; limit < 0 omits LIMIT. */
HOTKV_API hotkv_reply_t *hotkv_zintercard(hotkv_context_t *ctx, const char **keys,
                                           const size_t *keylens, size_t nkeys, int64_t limit);
/** ZSCAN key cursor [MATCH pattern] [COUNT count]; pattern NULL / count <= 0 omit. */
HOTKV_API hotkv_reply_t *hotkv_zscan(hotkv_context_t *ctx, const char *key, size_t keylen,
                                      uint64_t cursor, const char *pattern, int64_t count);

/* ----- Geo ----- */
/** GEOADD key lon lat member [lon lat member ...] */
HOTKV_API hotkv_reply_t *hotkv_geoadd(hotkv_context_t *ctx, const char *key, size_t keylen,
                                       const double *lons, const double *lats,
                                       const char **members, const size_t *memlens,
                                       size_t npoints);
/** GEODIST key member1 member2 [unit]; unit NULL means meters. */
HOTKV_API hotkv_reply_t *hotkv_geodist(hotkv_context_t *ctx, const char *key, size_t keylen,
                                        const char *member1, size_t member1len,
                                        const char *member2, size_t member2len, const char *unit);
HOTKV_API hotkv_reply_t *hotkv_geopos(hotkv_context_t *ctx, const char *key, size_t keylen,
                                       const char **members, const size_t *memlens,
                                       size_t nmembers);
HOTKV_API hotkv_reply_t *hotkv_geohash(hotkv_context_t *ctx, const char *key, size_t keylen,
                                        const char **members, const size_t *memlens,
                                        size_t nmembers);

/** Options for hotkv_geosearch(). Zero-initialise, then set one origin
 *  (from_member or from_lonlat) and one shape (radius or box). */
typedef struct hotkv_geosearch_opts {
    const char *from_member;   /**< FROMMEMBER member (NUL-terminated), or NULL. */
    int from_lonlat;           /**< Non-zero: FROMLONLAT lon lat. */
    double lon, lat;
    int by_radius;             /**< Non-zero: BYRADIUS radius unit. */
    double radius;
    int by_box;                /**< Non-zero: BYBOX width height unit. */
    double width, height;
    const char *unit;          /**< "m", "km", "ft" or "mi". */
    int order;                 /**< 0 none, 1 ASC, 2 DESC. */
    int64_t count;             /**< COUNT n when > 0. */
    int any;                   /**< COUNT ... ANY (requires count > 0). */
    int withcoord, withdist, withhash;
} hotkv_geosearch_opts_t;

HOTKV_API hotkv_reply_t *hotkv_geosearch(hotkv_context_t *ctx, const char *key, size_t keylen,
                                          const hotkv_geosearch_opts_t *opts);

/* ----- HyperLogLog ----- */
HOTKV_API hotkv_reply_t *hotkv_pfadd(hotkv_context_t *ctx, const char *key, size_t keylen,
                                      const char **elements, const size_t *elemlens,
                                      size_t nelements);
HOTKV_API hotkv_reply_t *hotkv_pfcount(hotkv_context_t *ctx, const char **keys,
                                        const size_t *keylens, size_t nkeys);
HOTKV_API hotkv_reply_t *hotkv_pfmerge(hotkv_context_t *ctx, const char *dest, size_t destlen,
                                        const char **keys, const size_t *keylens, size_t nkeys);

/* ----- Streams (consumer groups and maintenance) ----- */
HOTKV_API hotkv_reply_t *hotkv_xdel(hotkv_context_t *ctx, const char *key, size_t keylen,
                                     const char **ids, const size_t *idlens, size_t nids);
/** XTRIM key MAXLEN|MINID [~] threshold. strategy is "MAXLEN" or "MINID". */
HOTKV_API hotkv_reply_t *hotkv_xtrim(hotkv_context_t *ctx, const char *key, size_t keylen,
                                      const char *strategy, int approximate,
                                      const char *threshold);
HOTKV_API hotkv_reply_t *hotkv_xrevrange(hotkv_context_t *ctx, const char *key, size_t keylen,
                                          const char *end, const char *start, int64_t count);
/** XGROUP CREATE key group id [MKSTREAM]. */
HOTKV_API hotkv_reply_t *hotkv_xgroup_create(hotkv_context_t *ctx, const char *key, size_t keylen,
                                              const char *group, const char *id, int mkstream);
HOTKV_API hotkv_reply_t *hotkv_xgroup_destroy(hotkv_context_t *ctx, const char *key,
                                               size_t keylen, const char *group);
HOTKV_API hotkv_reply_t *hotkv_xgroup_createconsumer(hotkv_context_t *ctx, const char *key,
                                                      size_t keylen, const char *group,
                                                      const char *consumer);
HOTKV_API hotkv_reply_t *hotkv_xgroup_delconsumer(hotkv_context_t *ctx, const char *key,
                                                   size_t keylen, const char *group,
                                                   const char *consumer);
HOTKV_API hotkv_reply_t *hotkv_xgroup_setid(hotkv_context_t *ctx, const char *key, size_t keylen,
                                             const char *group, const char *id);
HOTKV_API hotkv_reply_t *hotkv_xack(hotkv_context_t *ctx, const char *key, size_t keylen,
                                     const char *group, const char **ids, const size_t *idlens,
                                     size_t nids);
/** XREADGROUP GROUP group consumer [COUNT count] [NOACK] STREAMS key ... id ...
 *  (non-blocking; count <= 0 omits COUNT). */
HOTKV_API hotkv_reply_t *hotkv_xreadgroup(hotkv_context_t *ctx, const char *group,
                                           const char *consumer, const char **keys,
                                           const size_t *keylens, const char **ids,
                                           const size_t *idlens, size_t nstreams, int64_t count,
                                           int noack);
/** XPENDING key group (summary form). */
HOTKV_API hotkv_reply_t *hotkv_xpending(hotkv_context_t *ctx, const char *key, size_t keylen,
                                         const char *group);
/** XPENDING key group start end count [consumer] (extended form). */
HOTKV_API hotkv_reply_t *hotkv_xpending_range(hotkv_context_t *ctx, const char *key,
                                               size_t keylen, const char *group,
                                               const char *start, const char *end, int64_t count,
                                               const char *consumer);
/** XCLAIM key group consumer min-idle-ms id [id ...]. */
HOTKV_API hotkv_reply_t *hotkv_xclaim(hotkv_context_t *ctx, const char *key, size_t keylen,
                                       const char *group, const char *consumer,
                                       int64_t min_idle_ms, const char **ids,
                                       const size_t *idlens, size_t nids);
/** XAUTOCLAIM key group consumer min-idle-ms start [COUNT count]; count <= 0 omits it. */
HOTKV_API hotkv_reply_t *hotkv_xautoclaim(hotkv_context_t *ctx, const char *key, size_t keylen,
                                           const char *group, const char *consumer,
                                           int64_t min_idle_ms, const char *start,
                                           int64_t count);
HOTKV_API hotkv_reply_t *hotkv_xinfo_groups(hotkv_context_t *ctx, const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_xinfo_consumers(hotkv_context_t *ctx, const char *key,
                                                size_t keylen, const char *group);

/* ----- Scripting ----- */
/** EVAL / EVAL_RO script numkeys key ... arg ... */
HOTKV_API hotkv_reply_t *hotkv_eval(hotkv_context_t *ctx, const char *script, size_t scriptlen,
                                     const char **keys, const size_t *keylens, size_t nkeys,
                                     const char **args, const size_t *arglens, size_t nargs);
HOTKV_API hotkv_reply_t *hotkv_eval_ro(hotkv_context_t *ctx, const char *script, size_t scriptlen,
                                        const char **keys, const size_t *keylens, size_t nkeys,
                                        const char **args, const size_t *arglens, size_t nargs);
/** EVALSHA / EVALSHA_RO sha1 numkeys key ... arg ... */
HOTKV_API hotkv_reply_t *hotkv_evalsha(hotkv_context_t *ctx, const char *sha1, const char **keys,
                                        const size_t *keylens, size_t nkeys, const char **args,
                                        const size_t *arglens, size_t nargs);
HOTKV_API hotkv_reply_t *hotkv_evalsha_ro(hotkv_context_t *ctx, const char *sha1,
                                           const char **keys, const size_t *keylens, size_t nkeys,
                                           const char **args, const size_t *arglens,
                                           size_t nargs);
HOTKV_API hotkv_reply_t *hotkv_script_load(hotkv_context_t *ctx, const char *script,
                                            size_t scriptlen);
HOTKV_API hotkv_reply_t *hotkv_script_exists(hotkv_context_t *ctx, const char **sha1s,
                                              size_t nsha1s);
HOTKV_API hotkv_reply_t *hotkv_script_flush(hotkv_context_t *ctx);
/** FUNCTION LOAD [REPLACE] code. */
HOTKV_API hotkv_reply_t *hotkv_function_load(hotkv_context_t *ctx, const char *code,
                                              size_t codelen, int replace);
/** FCALL / FCALL_RO function numkeys key ... arg ... */
HOTKV_API hotkv_reply_t *hotkv_fcall(hotkv_context_t *ctx, const char *function,
                                      const char **keys, const size_t *keylens, size_t nkeys,
                                      const char **args, const size_t *arglens, size_t nargs);
HOTKV_API hotkv_reply_t *hotkv_fcall_ro(hotkv_context_t *ctx, const char *function,
                                         const char **keys, const size_t *keylens, size_t nkeys,
                                         const char **args, const size_t *arglens, size_t nargs);

/* ----- Pub/Sub subscription -----
 * These send the command and read the one confirmation per channel/pattern,
 * so they return a status (HOTKV_OK or an error code), not a reply. After a
 * successful subscribe the connection only receives messages: read them
 * with hotkv_get_reply() (RESP2: arrays ["message", channel, payload] or
 * ["pmessage", pattern, channel, payload]; RESP3: push frames). Only
 * (P)SUBSCRIBE/(P)UNSUBSCRIBE/PING/QUIT are valid on that connection until
 * every subscription is cancelled. */
HOTKV_API int hotkv_subscribe(hotkv_context_t *ctx, const char **channels, const size_t *chanlens,
                               size_t nchannels);
HOTKV_API int hotkv_psubscribe(hotkv_context_t *ctx, const char **patterns, const size_t *patlens,
                                size_t npatterns);
HOTKV_API int hotkv_unsubscribe(hotkv_context_t *ctx, const char **channels,
                                 const size_t *chanlens, size_t nchannels);
HOTKV_API int hotkv_punsubscribe(hotkv_context_t *ctx, const char **patterns,
                                  const size_t *patlens, size_t npatterns);
/** PUBSUB CHANNELS [pattern] / PUBSUB NUMSUB [channel ...] / PUBSUB NUMPAT. */
HOTKV_API hotkv_reply_t *hotkv_pubsub_channels(hotkv_context_t *ctx, const char *pattern);
HOTKV_API hotkv_reply_t *hotkv_pubsub_numsub(hotkv_context_t *ctx, const char **channels,
                                              const size_t *chanlens, size_t nchannels);
HOTKV_API hotkv_reply_t *hotkv_pubsub_numpat(hotkv_context_t *ctx);

/* =========================================================================
 * HotKV-exclusive: Nanosecond expiration
 * ========================================================================= */

/**
 * @brief Set key expiration in nanoseconds.
 * @param ctx Context.
 * @param key Key name.
 * @param keylen Key length.
 * @param nanoseconds Expiration time in nanoseconds.
 * @return Reply object (integer: 1 if set, 0 if key does not exist).
 */
HOTKV_API hotkv_reply_t *hotkv_nexpire(hotkv_context_t *ctx,
                                        const char *key, size_t keylen,
                                        uint64_t nanoseconds);

/**
 * @brief Return the TTL of a key in nanoseconds.
 * @param ctx Context.
 * @param key Key name.
 * @param keylen Key length.
 * @return Reply object (integer: TTL in nanoseconds, -1 if no TTL, -2 if key missing).
 */
HOTKV_API hotkv_reply_t *hotkv_nttl(hotkv_context_t *ctx,
                                     const char *key, size_t keylen);

/**
 * @brief Set key expiration at a nanosecond epoch timestamp.
 * @param ctx Context.
 * @param key Key name.
 * @param keylen Key length.
 * @param ns_timestamp Nanosecond UNIX timestamp.
 * @return Reply object (integer: 1 if set, 0 if key does not exist).
 */
HOTKV_API hotkv_reply_t *hotkv_nexpireat(hotkv_context_t *ctx,
                                          const char *key, size_t keylen,
                                          uint64_t ns_timestamp);

/* =========================================================================
 * HotKV-exclusive: Tag commands
 * ========================================================================= */

/**
 * @brief Attach one or more tags to an existing key.
 * @param ctx Context.
 * @param key Key name.
 * @param keylen Key length.
 * @param tags Array of tag strings.
 * @param taglens Array of tag lengths.
 * @param ntags Number of tags.
 * @return Reply object (+OK on success).
 */
HOTKV_API hotkv_reply_t *hotkv_tag_set(hotkv_context_t *ctx,
                                        const char *key, size_t keylen,
                                        const char **tags, const size_t *taglens,
                                        size_t ntags);

/**
 * @brief Remove one or more tags from a key.
 * @param ctx Context.
 * @param key Key name.
 * @param keylen Key length.
 * @param tags Array of tag strings.
 * @param taglens Array of tag lengths.
 * @param ntags Number of tags.
 * @return Reply object (integer: number of tags removed).
 */
HOTKV_API hotkv_reply_t *hotkv_tag_rem(hotkv_context_t *ctx,
                                        const char *key, size_t keylen,
                                        const char **tags, const size_t *taglens,
                                        size_t ntags);

/**
 * @brief Return all tags attached to a key.
 * @param ctx Context.
 * @param key Key name.
 * @param keylen Key length.
 * @return Reply object (array of bulk strings).
 */
HOTKV_API hotkv_reply_t *hotkv_tag_get(hotkv_context_t *ctx,
                                        const char *key, size_t keylen);

/**
 * @brief Return all keys that carry a given tag.
 * @param ctx Context.
 * @param tag Tag name.
 * @param taglen Tag length.
 * @return Reply object (array of bulk strings).
 */
HOTKV_API hotkv_reply_t *hotkv_tag_members(hotkv_context_t *ctx,
                                            const char *tag, size_t taglen);

/**
 * @brief Return keys that carry ALL of the given tags (intersection).
 * @param ctx Context.
 * @param tags Array of tag strings.
 * @param taglens Array of tag lengths.
 * @param ntags Number of tags.
 * @return Reply object (array of bulk strings).
 */
HOTKV_API hotkv_reply_t *hotkv_tag_inter(hotkv_context_t *ctx,
                                          const char **tags, const size_t *taglens,
                                          size_t ntags);

/**
 * @brief Return the number of tags attached to a key.
 * @param ctx Context.
 * @param key Key name.
 * @param keylen Key length.
 * @return Reply object (integer: tag count).
 */
HOTKV_API hotkv_reply_t *hotkv_tag_count(hotkv_context_t *ctx,
                                          const char *key, size_t keylen);

/**
 * @brief Check whether a key has a specific tag.
 * @param ctx Context.
 * @param key Key name.
 * @param keylen Key length.
 * @param tag Tag name.
 * @param taglen Tag length.
 * @return Reply object (integer: 1 if exists, 0 otherwise).
 */
HOTKV_API hotkv_reply_t *hotkv_tag_exists(hotkv_context_t *ctx,
                                           const char *key, size_t keylen,
                                           const char *tag, size_t taglen);

/* =========================================================================
 * HotKV-exclusive: Monitoring
 * ========================================================================= */

/**
 * @brief Show the hottest keys by access frequency.
 * @param ctx Context.
 * @param count Number of top keys to return (0 = default).
 * @return Reply object (array).
 */
HOTKV_API hotkv_reply_t *hotkv_hotkeys_top(hotkv_context_t *ctx, int64_t count);

/**
 * @brief Reset hot key tracking counters.
 * @param ctx Context.
 * @return Reply object (+OK).
 */
HOTKV_API hotkv_reply_t *hotkv_hotkeys_reset(hotkv_context_t *ctx);

/**
 * @brief Get latest per-command latency data.
 * @param ctx Context.
 * @return Reply object.
 */
HOTKV_API hotkv_reply_t *hotkv_latency_latest(hotkv_context_t *ctx);

/**
 * @brief Get latency history for a specific command.
 * @param ctx Context.
 * @param cmd Command name.
 * @return Reply object.
 */
HOTKV_API hotkv_reply_t *hotkv_latency_history(hotkv_context_t *ctx, const char *cmd);

/**
 * @brief Reset latency tracking data.
 * @param ctx Context.
 * @return Reply object (+OK).
 */
HOTKV_API hotkv_reply_t *hotkv_latency_reset(hotkv_context_t *ctx);

/* =========================================================================
 * HotKV exclusive and AI/enterprise commands
 *
 * These wrappers build the exact argument sequence the server expects and
 * return the raw reply, like every other command in this header. Enterprise
 * families require a server license that includes the feature; hotkv_reply_is_nolicense() tells
 * you whether a reply is that specific "not licensed" error so callers (and
 * tests) can distinguish it from every other error.
 *
 * Sentinel conventions (see hotkv_enterprise.c for the rationale):
 *   - uint32_t/uint64_t ttl/max_entries/count/window/cost style params: 0
 *     means "omit the option" (matches the server default).
 *   - double tuning params (cost_per_hit, refill_rate, drain_rate): 0.0
 *     means omit.
 *   - const char* enum-like params (metric, strategy, algo, severity): NULL
 *     or "" means omit.
 *   - "const T *" params (threshold, alpha, from_ts/to_ts, limit, timestamp,
 *     error flag): NULL means omit; a non-NULL pointer supplies the value.
 * ========================================================================= */

/** @return Non-zero if reply is a HotKV "NOLICENSE" error (enterprise
 * feature not covered by the currently loaded license). */
HOTKV_API int hotkv_reply_is_nolicense(const hotkv_reply_t *reply);

/**
 * @brief Shared "token usage group" for LLM-call accounting: attach it to a
 * cache SET (PCACHE.SET, SCACHE.SET, every keyed cache SET) or to
 * LLMBUDGET.CONSUME so a later cache hit (or the budget's own accounting)
 * attributes the tokens, and once model is priced (hotkv_llmstats_price_set()),
 * the cost, that original call spent.
 *
 * model is required whenever a usage group is sent at all: pass a NULL
 * `const hotkv_token_usage_t *` argument (not a struct with an empty model)
 * to omit the group entirely, sending nothing new. The four token counts
 * default to 0 and are left off the wire when zero; MODEL itself is always
 * sent whenever this struct is passed.
 */
typedef struct hotkv_token_usage {
    const char *model;              /**< Required; never NULL/empty when the struct is used. */
    uint64_t input_tokens;          /**< Uncached prompt tokens. */
    uint64_t output_tokens;         /**< Output tokens. */
    uint64_t cache_read_tokens;     /**< Provider prompt-cache hit tokens. */
    uint64_t cache_write_5m_tokens; /**< Anthropic-style 5-minute-TTL cache-write tokens. */
    uint64_t cache_write_1h_tokens; /**< Anthropic-style 1-hour-TTL cache-write tokens. */
} hotkv_token_usage_t;

/**
 * @brief Tokens and cost a cache's hits did not have to spend, accumulated
 * from hotkv_token_usage_t values recorded on SET (see e.g. hotkv_pcache_set()).
 * Every field is zero when no SET ever carried a usage group. Shared by
 * hotkv_scache_stats_t, hotkv_pcache_stats_t and hotkv_keyedcache_stats_t.
 */
typedef struct hotkv_usage_savings {
    int64_t requests;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t cache_read_tokens;
    int64_t cache_write_tokens;
    double cost_usd;
} hotkv_usage_savings_t;

/* --- GCRA.* (rate limiter primitive) --- */

HOTKV_API hotkv_reply_t *hotkv_gcra_limit(hotkv_context_t *ctx,
                                           const char *key, size_t keylen,
                                           int64_t period_ms, int64_t burst, int64_t count);
HOTKV_API hotkv_reply_t *hotkv_gcra_reset(hotkv_context_t *ctx,
                                           const char **keys, const size_t *keylens, size_t nkeys);

/* --- VSIM.* (vector similarity index) --- */

HOTKV_API hotkv_reply_t *hotkv_vsim_createindex(hotkv_context_t *ctx,
                                                 const char *name, size_t namelen, uint32_t dims,
                                                 const char *metric, uint32_t m,
                                                 uint32_t ef_construction, uint32_t ef_search);
HOTKV_API hotkv_reply_t *hotkv_vsim_dropindex(hotkv_context_t *ctx,
                                               const char *name, size_t namelen);
HOTKV_API hotkv_reply_t *hotkv_vsim_set(hotkv_context_t *ctx,
                                         const char *index, size_t indexlen,
                                         const char *key, size_t keylen,
                                         const float *vec, size_t dims);
HOTKV_API hotkv_reply_t *hotkv_vsim_get(hotkv_context_t *ctx,
                                         const char *index, size_t indexlen,
                                         const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_vsim_del(hotkv_context_t *ctx,
                                         const char *index, size_t indexlen,
                                         const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_vsim_search(hotkv_context_t *ctx,
                                            const char *index, size_t indexlen, uint32_t k,
                                            const float *vec, size_t dims,
                                            const char **filter_tags, const size_t *filter_taglens,
                                            size_t nfilter, uint32_t ef);
/** @param index NULL to list all index names. */
HOTKV_API hotkv_reply_t *hotkv_vsim_info(hotkv_context_t *ctx,
                                          const char *index, size_t indexlen);

/* --- VSIM persistence (needs a license with the VSIM persistence feature) --- */

HOTKV_API hotkv_reply_t *hotkv_vsim_save(hotkv_context_t *ctx,
                                          const char *name, size_t namelen,
                                          const char *blob, size_t bloblen);
HOTKV_API hotkv_reply_t *hotkv_vsim_load(hotkv_context_t *ctx,
                                          const char *name, size_t namelen);
HOTKV_API hotkv_reply_t *hotkv_vsim_list(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_vsim_drop(hotkv_context_t *ctx,
                                          const char *name, size_t namelen);

/* --- SCACHE.* (semantic cache, needs a license with the semantic-cache feature) --- */

HOTKV_API hotkv_reply_t *hotkv_scache_create(hotkv_context_t *ctx,
                                              const char *ns, size_t nslen, uint32_t dims,
                                              const char *metric, const double *threshold,
                                              uint32_t ttl_seconds);
/** @param usage NULL to omit; see hotkv_token_usage_t. Records the original
 * (uncached) call's token usage so a later hotkv_scache_get() hit
 * attributes the tokens and cost that hit did not have to spend (see
 * hotkv_parse_scache_stats()). */
HOTKV_API hotkv_reply_t *hotkv_scache_set(hotkv_context_t *ctx,
                                           const char *ns, size_t nslen,
                                           const char *query, size_t querylen,
                                           const char *response, size_t responselen,
                                           const float *vec, size_t dims, uint32_t ttl_seconds,
                                           const hotkv_token_usage_t *usage);
HOTKV_API hotkv_reply_t *hotkv_scache_get(hotkv_context_t *ctx,
                                           const char *ns, size_t nslen,
                                           const float *vec, size_t dims,
                                           const double *threshold_override);
HOTKV_API hotkv_reply_t *hotkv_scache_stats(hotkv_context_t *ctx, const char *ns, size_t nslen);

/** Decoded SCACHE.STATS reply; see hotkv_parse_scache_stats(). */
typedef struct hotkv_scache_stats {
    int64_t entries;
    int64_t hits;
    int64_t misses;
    double hit_rate;
    int64_t p50_us;
    hotkv_usage_savings_t usage;
} hotkv_scache_stats_t;

/**
 * @brief Parses a SCACHE.STATS reply into out by field name (RESP2 flat
 * array or RESP3 map): missing
 * fields default to zero and unknown extra fields are ignored. out's
 * fields are plain values (nothing is allocated; reply may be freed right
 * after this call).
 * @return HOTKV_OK, or HOTKV_ERR_OTHER if reply is not a pair-shaped reply.
 */
HOTKV_API int hotkv_parse_scache_stats(const hotkv_reply_t *reply, hotkv_scache_stats_t *out);
HOTKV_API hotkv_reply_t *hotkv_scache_flush(hotkv_context_t *ctx, const char *ns, size_t nslen);
HOTKV_API hotkv_reply_t *hotkv_scache_drop(hotkv_context_t *ctx, const char *ns, size_t nslen);
HOTKV_API hotkv_reply_t *hotkv_scache_list(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_scache_threshold(hotkv_context_t *ctx,
                                                 const char *ns, size_t nslen, double value);

/* --- PCACHE.* (prompt/LLM response cache, needs a license with the prompt-cache feature) --- */

HOTKV_API hotkv_reply_t *hotkv_pcache_create(hotkv_context_t *ctx,
                                              const char *ns, size_t nslen,
                                              uint64_t max_entries, uint32_t ttl_seconds,
                                              double cost_per_hit);
/** @param usage NULL to omit; see hotkv_token_usage_t. Records the original
 * (uncached) call's token usage so a later hotkv_pcache_get()/
 * hotkv_pcache_getex() hit attributes the tokens and cost that hit did not
 * have to spend (see hotkv_parse_pcache_stats()). */
HOTKV_API hotkv_reply_t *hotkv_pcache_set(hotkv_context_t *ctx,
                                           const char *ns, size_t nslen,
                                           const char *prompt, size_t promptlen,
                                           const char *response, size_t responselen,
                                           uint32_t ttl_seconds,
                                           const char *meta, size_t metalen,
                                           const hotkv_token_usage_t *usage);
HOTKV_API hotkv_reply_t *hotkv_pcache_get(hotkv_context_t *ctx,
                                           const char *ns, size_t nslen,
                                           const char *prompt, size_t promptlen);
HOTKV_API hotkv_reply_t *hotkv_pcache_getex(hotkv_context_t *ctx,
                                             const char *ns, size_t nslen,
                                             const char *prompt, size_t promptlen);
HOTKV_API hotkv_reply_t *hotkv_pcache_del(hotkv_context_t *ctx,
                                           const char *ns, size_t nslen,
                                           const char *prompt, size_t promptlen);
HOTKV_API hotkv_reply_t *hotkv_pcache_stats(hotkv_context_t *ctx, const char *ns, size_t nslen);

/** Decoded PCACHE.STATS reply; see hotkv_parse_pcache_stats(). */
typedef struct hotkv_pcache_stats {
    int64_t entries;
    int64_t hits;
    int64_t misses;
    int64_t evictions;
    int64_t memory_bytes;
    double hit_rate;
    int64_t p50_ns;
    int64_t p95_ns;
    int64_t p99_ns;
    double cost_saved_usd;
    hotkv_usage_savings_t usage;
} hotkv_pcache_stats_t;

/** @brief Parses a PCACHE.STATS reply into out by field name; see
 * hotkv_parse_scache_stats() for the parsing rules this follows.
 * @return HOTKV_OK, or HOTKV_ERR_OTHER if reply is not a pair-shaped reply. */
HOTKV_API int hotkv_parse_pcache_stats(const hotkv_reply_t *reply, hotkv_pcache_stats_t *out);

HOTKV_API hotkv_reply_t *hotkv_pcache_flush(hotkv_context_t *ctx, const char *ns, size_t nslen);
HOTKV_API hotkv_reply_t *hotkv_pcache_drop(hotkv_context_t *ctx, const char *ns, size_t nslen);
HOTKV_API hotkv_reply_t *hotkv_pcache_list(hotkv_context_t *ctx);
/** @param option One of "MAX_ENTRIES", "TTL", "COST_PER_HIT". */
HOTKV_API hotkv_reply_t *hotkv_pcache_config_set(hotkv_context_t *ctx,
                                                  const char *ns, size_t nslen,
                                                  const char *option,
                                                  const char *value, size_t valuelen);

/* --- Keyed caches: TOOL.CACHE.* / NCACHE.* / GUARD.* / RERANK.* ---
 * Identical shape per family (each needs a license with its own feature):
 * CREATE, SET, GET, DEL, STATS, FLUSH, DROP, LIST.
 */

#define HOTKV_DECLARE_KEYED_CACHE_FAMILY(NAME)                                                   \
    HOTKV_API hotkv_reply_t *hotkv_##NAME##_create(hotkv_context_t *ctx,                         \
                                                    const char *ns, size_t nslen,                 \
                                                    uint64_t max_entries, uint32_t ttl);          \
    HOTKV_API hotkv_reply_t *hotkv_##NAME##_set(hotkv_context_t *ctx,                            \
                                                 const char *ns, size_t nslen,                    \
                                                 const char *key, size_t keylen,                  \
                                                 const char *value, size_t valuelen,              \
                                                 uint32_t ttl, const char *meta, size_t metalen,  \
                                                 const hotkv_token_usage_t *usage);                \
    HOTKV_API hotkv_reply_t *hotkv_##NAME##_get(hotkv_context_t *ctx,                            \
                                                 const char *ns, size_t nslen,                    \
                                                 const char *key, size_t keylen);                 \
    HOTKV_API hotkv_reply_t *hotkv_##NAME##_del(hotkv_context_t *ctx,                            \
                                                 const char *ns, size_t nslen,                    \
                                                 const char *key, size_t keylen);                 \
    HOTKV_API hotkv_reply_t *hotkv_##NAME##_stats(hotkv_context_t *ctx,                          \
                                                   const char *ns, size_t nslen);                 \
    HOTKV_API hotkv_reply_t *hotkv_##NAME##_flush(hotkv_context_t *ctx,                          \
                                                   const char *ns, size_t nslen);                 \
    HOTKV_API hotkv_reply_t *hotkv_##NAME##_drop(hotkv_context_t *ctx,                           \
                                                  const char *ns, size_t nslen);                  \
    HOTKV_API hotkv_reply_t *hotkv_##NAME##_list(hotkv_context_t *ctx)

HOTKV_DECLARE_KEYED_CACHE_FAMILY(toolcache);
HOTKV_DECLARE_KEYED_CACHE_FAMILY(ncache);
HOTKV_DECLARE_KEYED_CACHE_FAMILY(guardcache);
HOTKV_DECLARE_KEYED_CACHE_FAMILY(rerankcache);

#undef HOTKV_DECLARE_KEYED_CACHE_FAMILY

/** Decoded <PREFIX>.STATS reply for a keyed cache family, shared by
 * hotkv_toolcache_stats(), hotkv_ncache_stats(), hotkv_guardcache_stats()
 * and hotkv_rerankcache_stats(); see hotkv_parse_keyedcache_stats(). */
typedef struct hotkv_keyedcache_stats {
    int64_t entries;
    int64_t hits;
    int64_t misses;
    int64_t evictions;
    int64_t memory_bytes;
    hotkv_usage_savings_t usage;
} hotkv_keyedcache_stats_t;

/** @brief Parses a keyed cache STATS reply into out by field name; see
 * hotkv_parse_scache_stats() for the parsing rules this follows.
 * @return HOTKV_OK, or HOTKV_ERR_OTHER if reply is not a pair-shaped reply. */
HOTKV_API int hotkv_parse_keyedcache_stats(const hotkv_reply_t *reply,
                                           hotkv_keyedcache_stats_t *out);

/* --- LLMSTATS.* (needs a license with the LLM stats feature) --- */

HOTKV_API hotkv_reply_t *hotkv_llmstats_bucket(hotkv_context_t *ctx,
                                                const char *name, size_t namelen);
/**
 * @param error_flag NULL to omit the ERROR option.
 * @param model NULL or "" to omit; otherwise the server computes cost_usd
 * from its price table (see hotkv_llmstats_price_set()) and ignores the
 * cost_usd argument (pass 0.0). An unpriced model still records tokens,
 * costs 0, and increments unpriced_requests (hotkv_llmstats_get_t).
 * @param cache_write_5m_tokens 0 to omit; only meaningful together with model.
 * @param cache_write_1h_tokens 0 to omit; only meaningful together with model.
 */
HOTKV_API hotkv_reply_t *hotkv_llmstats_record(hotkv_context_t *ctx,
                                                const char *bucket, size_t bucketlen,
                                                uint64_t input_tokens, uint64_t output_tokens,
                                                uint64_t cached_tokens, double cost_usd,
                                                uint64_t latency_ms, const int *error_flag,
                                                const char *model, uint64_t cache_write_5m_tokens,
                                                uint64_t cache_write_1h_tokens);
HOTKV_API hotkv_reply_t *hotkv_llmstats_get(hotkv_context_t *ctx,
                                             const char *bucket, size_t bucketlen);

/** Decoded LLMSTATS.GET reply; see hotkv_parse_llmstats_get(). */
typedef struct hotkv_llmstats_get {
    int64_t requests;
    int64_t errors;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t cached_tokens;
    double cost_usd;
    double avg_latency_ms;
    int64_t requests_1m;
    int64_t tokens_1m;
    double cost_usd_1m;
    int64_t cache_write_5m_tokens;
    int64_t cache_write_1h_tokens;
    int64_t unpriced_requests;
} hotkv_llmstats_get_t;

/** @brief Parses an LLMSTATS.GET reply into out by field name; see
 * hotkv_parse_scache_stats() for the parsing rules this follows.
 * @return HOTKV_OK, or HOTKV_ERR_OTHER if reply is not a pair-shaped reply. */
HOTKV_API int hotkv_parse_llmstats_get(const hotkv_reply_t *reply, hotkv_llmstats_get_t *out);

HOTKV_API hotkv_reply_t *hotkv_llmstats_reset(hotkv_context_t *ctx,
                                               const char *bucket, size_t bucketlen);
HOTKV_API hotkv_reply_t *hotkv_llmstats_drop(hotkv_context_t *ctx,
                                              const char *bucket, size_t bucketlen);
HOTKV_API hotkv_reply_t *hotkv_llmstats_list(hotkv_context_t *ctx);

/* --- LLMSTATS.PRICE SET|GET|LIST|DEL: runtime-overridable per-model price
 * table, shared by LLMSTATS.RECORD's and LLMBUDGET.CONSUME's MODEL option
 * (needs a license with the LLM stats feature) --- */

/** Decoded LLMSTATS.PRICE GET/LIST entry; see hotkv_parse_llm_price(). */
typedef struct hotkv_llm_price {
    const char *model;
    const char *source; /**< "default", "custom" or "unpriced". */
    int priced;
    int retired;
    double input_usd_per_mtok;
    double output_usd_per_mtok;
    double cache_read_usd_per_mtok;
    double cache_write_5m_usd_per_mtok;
    double cache_write_1h_usd_per_mtok;
} hotkv_llm_price_t;

/**
 * @brief Sets (or replaces) a custom price override for model. Prices are
 * USD per 1,000,000 tokens.
 * @param cache_read_usd_per_mtok NULL to default to 0.1x input_usd_per_mtok.
 * @param cache_write_5m_usd_per_mtok NULL to default to 1.25x input_usd_per_mtok.
 * @param cache_write_1h_usd_per_mtok NULL to default to 2x input_usd_per_mtok.
 */
HOTKV_API hotkv_reply_t *hotkv_llmstats_price_set(hotkv_context_t *ctx,
                                                   const char *model, size_t modellen,
                                                   double input_usd_per_mtok,
                                                   double output_usd_per_mtok,
                                                   const double *cache_read_usd_per_mtok,
                                                   const double *cache_write_5m_usd_per_mtok,
                                                   const double *cache_write_1h_usd_per_mtok);
/** @brief Returns model's price sheet. Unknown models reply with source
 * "unpriced"/priced 0 rather than an error; see hotkv_parse_llm_price(). */
HOTKV_API hotkv_reply_t *hotkv_llmstats_price_get(hotkv_context_t *ctx,
                                                   const char *model, size_t modellen);
/** @brief Returns every known model's price sheet (built-in and custom) as
 * an array whose elements each parse with hotkv_parse_llm_price(). */
HOTKV_API hotkv_reply_t *hotkv_llmstats_price_list(hotkv_context_t *ctx);
/** @brief Removes a custom price override.
 * @return An HOTKV_REPLY_INTEGER reply: 1 if one existed (the model falls
 * back to its built-in price, if any), else 0. */
HOTKV_API hotkv_reply_t *hotkv_llmstats_price_del(hotkv_context_t *ctx,
                                                   const char *model, size_t modellen);

/** @brief Parses an LLMSTATS.PRICE GET reply, or one element of a
 * LLMSTATS.PRICE LIST reply, into out by field name; see
 * hotkv_parse_scache_stats() for the parsing rules this follows. String
 * fields point into reply and stay valid only as long as reply is not freed.
 * @return HOTKV_OK, or HOTKV_ERR_OTHER if reply is not a pair-shaped reply. */
HOTKV_API int hotkv_parse_llm_price(const hotkv_reply_t *reply, hotkv_llm_price_t *out);

/* --- LLMBUDGET.* (needs a license with the LLM budget feature) --- */

HOTKV_API hotkv_reply_t *hotkv_llmbudget_set(hotkv_context_t *ctx,
                                              const char *name, size_t namelen,
                                              uint64_t token_limit, double cost_limit_usd,
                                              uint32_t window_seconds);
/** @param usage NULL to omit; see hotkv_token_usage_t. When given, tokens
 * and cost_usd are IGNORED (pass 0 and 0.0): tokens is summed from usage's
 * counts and cost is priced from usage->model, exactly like
 * hotkv_llmstats_record()'s model option. Counts without a model produce an
 * ERR reply (a usage group's model is always required). */
HOTKV_API hotkv_reply_t *hotkv_llmbudget_consume(hotkv_context_t *ctx,
                                                  const char *name, size_t namelen,
                                                  uint64_t tokens, double cost_usd,
                                                  const hotkv_token_usage_t *usage);
HOTKV_API hotkv_reply_t *hotkv_llmbudget_status(hotkv_context_t *ctx,
                                                 const char *name, size_t namelen);

/** Decoded LLMBUDGET.STATUS reply; see hotkv_parse_llmbudget_status(). */
typedef struct hotkv_llmbudget_status {
    int64_t tokens_used;
    int64_t token_limit;
    double cost_used_usd;
    double cost_limit_usd;
    int64_t window_seconds;
    int64_t window_remaining;
    int64_t requests_allowed;
    int64_t requests_rejected;
    int64_t unpriced_requests;
} hotkv_llmbudget_status_t;

/** @brief Parses an LLMBUDGET.STATUS reply into out by field name; see
 * hotkv_parse_scache_stats() for the parsing rules this follows.
 * @return HOTKV_OK, or HOTKV_ERR_OTHER if reply is not a pair-shaped reply. */
HOTKV_API int hotkv_parse_llmbudget_status(const hotkv_reply_t *reply,
                                           hotkv_llmbudget_status_t *out);
HOTKV_API hotkv_reply_t *hotkv_llmbudget_reset(hotkv_context_t *ctx,
                                                const char *name, size_t namelen);
HOTKV_API hotkv_reply_t *hotkv_llmbudget_drop(hotkv_context_t *ctx,
                                               const char *name, size_t namelen);
HOTKV_API hotkv_reply_t *hotkv_llmbudget_list(hotkv_context_t *ctx);

/* --- PROMPT.* (versioned prompt registry, needs a license with the prompt-registry feature) --- */

HOTKV_API hotkv_reply_t *hotkv_prompt_put(hotkv_context_t *ctx,
                                           const char *name, size_t namelen,
                                           const char *content, size_t contentlen,
                                           const char *meta, size_t metalen);
/** @param version 0 for latest. */
HOTKV_API hotkv_reply_t *hotkv_prompt_get(hotkv_context_t *ctx,
                                           const char *name, size_t namelen, uint32_t version);
HOTKV_API hotkv_reply_t *hotkv_prompt_latest(hotkv_context_t *ctx,
                                              const char *name, size_t namelen);
HOTKV_API hotkv_reply_t *hotkv_prompt_setactive(hotkv_context_t *ctx,
                                                 const char *name, size_t namelen,
                                                 uint32_t version);
HOTKV_API hotkv_reply_t *hotkv_prompt_versions(hotkv_context_t *ctx,
                                                const char *name, size_t namelen);
HOTKV_API hotkv_reply_t *hotkv_prompt_drop(hotkv_context_t *ctx,
                                            const char *name, size_t namelen);
HOTKV_API hotkv_reply_t *hotkv_prompt_list(hotkv_context_t *ctx);

/* --- EMBED.DEDUP.* (needs a license with the embed-dedup feature) --- */

HOTKV_API hotkv_reply_t *hotkv_embed_dedup_put(hotkv_context_t *ctx,
                                                const char *ns, size_t nslen,
                                                const char *hash, size_t hashlen,
                                                const char *model, size_t modellen,
                                                const char *embedding, size_t embeddinglen);
HOTKV_API hotkv_reply_t *hotkv_embed_dedup_get(hotkv_context_t *ctx,
                                                const char *ns, size_t nslen,
                                                const char *hash, size_t hashlen);
HOTKV_API hotkv_reply_t *hotkv_embed_dedup_release(hotkv_context_t *ctx,
                                                    const char *ns, size_t nslen,
                                                    const char *hash, size_t hashlen);
HOTKV_API hotkv_reply_t *hotkv_embed_dedup_stats(hotkv_context_t *ctx,
                                                  const char *ns, size_t nslen);
HOTKV_API hotkv_reply_t *hotkv_embed_dedup_drop(hotkv_context_t *ctx,
                                                 const char *ns, size_t nslen);
HOTKV_API hotkv_reply_t *hotkv_embed_dedup_purge(hotkv_context_t *ctx,
                                                  const char *ns, size_t nslen);
HOTKV_API hotkv_reply_t *hotkv_embed_dedup_list(hotkv_context_t *ctx);

/* --- AMEM.* (AI agent memory, needs a license with the agent-memory feature) --- */

HOTKV_API hotkv_reply_t *hotkv_amem_conv_create(hotkv_context_t *ctx,
                                                 const char *session, size_t sessionlen,
                                                 uint32_t window, uint32_t ttl);
HOTKV_API hotkv_reply_t *hotkv_amem_conv_append(hotkv_context_t *ctx,
                                                 const char *session, size_t sessionlen,
                                                 const char *role, size_t rolelen,
                                                 const char *content, size_t contentlen);
HOTKV_API hotkv_reply_t *hotkv_amem_conv_get(hotkv_context_t *ctx,
                                              const char *session, size_t sessionlen);
HOTKV_API hotkv_reply_t *hotkv_amem_conv_window(hotkv_context_t *ctx,
                                                 const char *session, size_t sessionlen,
                                                 uint32_t n);
HOTKV_API hotkv_reply_t *hotkv_amem_conv_clear(hotkv_context_t *ctx,
                                                const char *session, size_t sessionlen);
HOTKV_API hotkv_reply_t *hotkv_amem_conv_drop(hotkv_context_t *ctx,
                                               const char *session, size_t sessionlen);

HOTKV_API hotkv_reply_t *hotkv_amem_sem_create(hotkv_context_t *ctx,
                                                const char *agent, size_t agentlen, uint32_t dims);
HOTKV_API hotkv_reply_t *hotkv_amem_sem_store(hotkv_context_t *ctx,
                                               const char *agent, size_t agentlen,
                                               const char *key, size_t keylen,
                                               const char *content, size_t contentlen,
                                               const float *vec, size_t dims, uint32_t ttl);
HOTKV_API hotkv_reply_t *hotkv_amem_sem_recall(hotkv_context_t *ctx,
                                                const char *agent, size_t agentlen, uint32_t k,
                                                const float *vec, size_t dims);
HOTKV_API hotkv_reply_t *hotkv_amem_sem_forget(hotkv_context_t *ctx,
                                                const char *agent, size_t agentlen,
                                                const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_amem_sem_list(hotkv_context_t *ctx,
                                              const char *agent, size_t agentlen);

HOTKV_API hotkv_reply_t *hotkv_amem_work_set(hotkv_context_t *ctx,
                                              const char *session, size_t sessionlen,
                                              const char *key, size_t keylen,
                                              const char *value, size_t valuelen, uint32_t ttl);
HOTKV_API hotkv_reply_t *hotkv_amem_work_get(hotkv_context_t *ctx,
                                              const char *session, size_t sessionlen,
                                              const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_amem_work_del(hotkv_context_t *ctx,
                                              const char *session, size_t sessionlen,
                                              const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_amem_work_clear(hotkv_context_t *ctx,
                                                const char *session, size_t sessionlen);
HOTKV_API hotkv_reply_t *hotkv_amem_work_list(hotkv_context_t *ctx,
                                               const char *session, size_t sessionlen);

HOTKV_API hotkv_reply_t *hotkv_amem_epis_log(hotkv_context_t *ctx,
                                              const char *agent, size_t agentlen,
                                              const char *event_type, size_t event_typelen,
                                              const char *content, size_t contentlen,
                                              const char *metadata, size_t metadatalen);
HOTKV_API hotkv_reply_t *hotkv_amem_epis_query(hotkv_context_t *ctx,
                                                const char *agent, size_t agentlen,
                                                const char *type, size_t typelen,
                                                const int64_t *from_ts, const int64_t *to_ts,
                                                const uint32_t *limit);
HOTKV_API hotkv_reply_t *hotkv_amem_epis_clear(hotkv_context_t *ctx,
                                                const char *agent, size_t agentlen);
HOTKV_API hotkv_reply_t *hotkv_amem_epis_stats(hotkv_context_t *ctx,
                                                const char *agent, size_t agentlen);

HOTKV_API hotkv_reply_t *hotkv_amem_search(hotkv_context_t *ctx,
                                            const char *agent, size_t agentlen, uint32_t k,
                                            const float *vec, size_t dims);

/* --- AMEM.SUMMARY.* (needs a license with the agent-memory summary feature) --- */

HOTKV_API hotkv_reply_t *hotkv_amem_summary_set(hotkv_context_t *ctx,
                                                 const char *agent, size_t agentlen,
                                                 const char *summary, size_t summarylen);
HOTKV_API hotkv_reply_t *hotkv_amem_summary_append(hotkv_context_t *ctx,
                                                    const char *agent, size_t agentlen,
                                                    const char *extra, size_t extralen);
HOTKV_API hotkv_reply_t *hotkv_amem_summary_get(hotkv_context_t *ctx,
                                                 const char *agent, size_t agentlen);
HOTKV_API hotkv_reply_t *hotkv_amem_summary_drop(hotkv_context_t *ctx,
                                                  const char *agent, size_t agentlen);
HOTKV_API hotkv_reply_t *hotkv_amem_summary_list(hotkv_context_t *ctx);

/* --- RAG.* (needs a license with the RAG feature) --- */

HOTKV_API hotkv_reply_t *hotkv_rag_create(hotkv_context_t *ctx,
                                           const char *name, size_t namelen, uint32_t dims,
                                           const char *strategy, uint32_t chunk_size,
                                           uint32_t chunk_overlap);
HOTKV_API hotkv_reply_t *hotkv_rag_ingest(hotkv_context_t *ctx,
                                           const char *pipeline, size_t pipelinelen,
                                           const char *doc_id, size_t doc_idlen,
                                           const char *text, size_t textlen,
                                           const char **meta_keys, const size_t *meta_keylens,
                                           const char **meta_vals, const size_t *meta_vallens,
                                           size_t nmeta,
                                           const float *const *vectors, const size_t *vector_lens,
                                           size_t nvectors);
HOTKV_API hotkv_reply_t *hotkv_rag_search(hotkv_context_t *ctx,
                                           const char *pipeline, size_t pipelinelen,
                                           const char *query_text, size_t query_textlen,
                                           uint32_t k, const double *alpha,
                                           const float *vector, size_t vector_len,
                                           const char **filter_keys, const size_t *filter_keylens,
                                           const char **filter_vals, const size_t *filter_vallens,
                                           size_t nfilter);
HOTKV_API hotkv_reply_t *hotkv_rag_stats(hotkv_context_t *ctx,
                                          const char *pipeline, size_t pipelinelen);
HOTKV_API hotkv_reply_t *hotkv_rag_drop(hotkv_context_t *ctx,
                                         const char *pipeline, size_t pipelinelen);
HOTKV_API hotkv_reply_t *hotkv_rag_list(hotkv_context_t *ctx);

/* --- RATELIMIT.* (needs a license with the rate-limiting feature) --- */

HOTKV_API hotkv_reply_t *hotkv_ratelimit_create(hotkv_context_t *ctx,
                                                 const char *name, size_t namelen,
                                                 uint64_t limit, uint64_t window_ms,
                                                 const char *algo, double refill_rate,
                                                 double drain_rate);
HOTKV_API hotkv_reply_t *hotkv_ratelimit_check(hotkv_context_t *ctx,
                                                const char *name, size_t namelen,
                                                const char *key, size_t keylen, uint64_t cost);
HOTKV_API hotkv_reply_t *hotkv_ratelimit_peek(hotkv_context_t *ctx,
                                               const char *name, size_t namelen,
                                               const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_ratelimit_reset(hotkv_context_t *ctx,
                                                const char *name, size_t namelen,
                                                const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_ratelimit_stats(hotkv_context_t *ctx,
                                                const char *name, size_t namelen);
HOTKV_API hotkv_reply_t *hotkv_ratelimit_drop(hotkv_context_t *ctx,
                                               const char *name, size_t namelen);
HOTKV_API hotkv_reply_t *hotkv_ratelimit_list(hotkv_context_t *ctx);

/* --- TS.* (time series, needs a license with the time-series feature) --- */

HOTKV_API hotkv_reply_t *hotkv_ts_create(hotkv_context_t *ctx,
                                          const char *key, size_t keylen, uint64_t retention_ms,
                                          const char **label_keys, const size_t *label_keylens,
                                          const char **label_vals, const size_t *label_vallens,
                                          size_t nlabels);
/** @param timestamp_ms NULL to use the server's current time ("*" on the wire). */
HOTKV_API hotkv_reply_t *hotkv_ts_add(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       const int64_t *timestamp_ms, double value);
HOTKV_API hotkv_reply_t *hotkv_ts_range(hotkv_context_t *ctx,
                                         const char *key, size_t keylen,
                                         int64_t from_ms, int64_t to_ms, uint64_t count);
/** @param agg_type One of "avg", "min", "max", "sum", "count". */
HOTKV_API hotkv_reply_t *hotkv_ts_agg(hotkv_context_t *ctx,
                                       const char *key, size_t keylen,
                                       int64_t from_ms, int64_t to_ms, uint64_t bucket_ms,
                                       const char *agg_type);
HOTKV_API hotkv_reply_t *hotkv_ts_info(hotkv_context_t *ctx, const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_ts_delete(hotkv_context_t *ctx, const char *key, size_t keylen);
HOTKV_API hotkv_reply_t *hotkv_ts_retention(hotkv_context_t *ctx,
                                             const char *key, size_t keylen,
                                             uint64_t retention_ms);
HOTKV_API hotkv_reply_t *hotkv_ts_downsample(hotkv_context_t *ctx,
                                              const char *src, size_t srclen,
                                              const char *dest, size_t destlen,
                                              const char *agg_type, uint64_t bucket_ms);
/** @param label_key NULL to list every series unfiltered. */
HOTKV_API hotkv_reply_t *hotkv_ts_list(hotkv_context_t *ctx,
                                        const char *label_key, size_t label_keylen,
                                        const char *label_val, size_t label_vallen);

/* --- FEATURE.* (feature store, needs a license with the feature-store feature) --- */

/** @param types Each entry one of "int64","double","string","int_array","double_array". */
HOTKV_API hotkv_reply_t *hotkv_feature_create(hotkv_context_t *ctx,
                                               const char *group, size_t grouplen,
                                               const char *description, size_t descriptionlen,
                                               const char **names, const size_t *namelens,
                                               const char **types, const size_t *typelens,
                                               size_t nfeatures);
HOTKV_API hotkv_reply_t *hotkv_feature_set(hotkv_context_t *ctx,
                                            const char *group, size_t grouplen,
                                            const char *entity, size_t entitylen,
                                            const char **names, const size_t *namelens,
                                            const char **values, const size_t *valuelens,
                                            size_t nfeatures);
/** @param nnames 0 to fetch every feature for the entity. */
HOTKV_API hotkv_reply_t *hotkv_feature_get(hotkv_context_t *ctx,
                                            const char *group, size_t grouplen,
                                            const char *entity, size_t entitylen,
                                            const char **names, const size_t *namelens,
                                            size_t nnames);
HOTKV_API hotkv_reply_t *hotkv_feature_batch(hotkv_context_t *ctx,
                                              const char *group, size_t grouplen,
                                              const char **entities, const size_t *entitylens,
                                              size_t nentities,
                                              const char **names, const size_t *namelens,
                                              size_t nnames);
HOTKV_API hotkv_reply_t *hotkv_feature_schema(hotkv_context_t *ctx,
                                               const char *group, size_t grouplen);
HOTKV_API hotkv_reply_t *hotkv_feature_list(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_feature_drop(hotkv_context_t *ctx,
                                             const char *group, size_t grouplen);
HOTKV_API hotkv_reply_t *hotkv_feature_stats(hotkv_context_t *ctx,
                                              const char *group, size_t grouplen);

/* --- OBSERVE / ALERTS (needs a license with the observability feature, admin) --- */

HOTKV_API hotkv_reply_t *hotkv_observe_info(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_observe_metrics(hotkv_context_t *ctx);
HOTKV_API hotkv_reply_t *hotkv_alerts_info(hotkv_context_t *ctx);
/** @param severity NULL for the dispatcher's configured minimum. */
HOTKV_API hotkv_reply_t *hotkv_alerts_test(hotkv_context_t *ctx,
                                            const char *severity,
                                            const char *message, size_t messagelen);
/** @param url Empty string to disable alerting. */
HOTKV_API hotkv_reply_t *hotkv_alerts_seturl(hotkv_context_t *ctx,
                                              const char *url, size_t urllen);
/** @param severity One of "info", "warning", "critical". */
HOTKV_API hotkv_reply_t *hotkv_alerts_setseverity(hotkv_context_t *ctx, const char *severity);
HOTKV_API hotkv_reply_t *hotkv_alerts_set_enabled(hotkv_context_t *ctx, int enabled);

/* =========================================================================
 * Utilities: connection URL parsing, cluster key slots, typed errors
 * ========================================================================= */

/** Parsed connection URL. Free with hotkv_url_free(). */
typedef struct hotkv_url {
    char *host;      /**< Heap-allocated, NUL-terminated. Never NULL on success. */
    int port;        /**< Default 6379 if not present in the URL. */
    char *username;  /**< Heap-allocated, or NULL if not present. */
    char *password;  /**< Heap-allocated, or NULL if not present. */
    int db;          /**< Database index from the URL path, default 0. */
    int use_tls;     /**< 1 for hotkvs:// or rediss://, 0 otherwise. */
} hotkv_url_t;

/**
 * @brief Parse a connection URL of the form
 * `(hotkv|hotkvs|redis|rediss)://[user[:pass]@]host[:port][/db]`.
 * `hotkvs://`/`rediss://` set use_tls; `redis://`/`rediss://` are accepted
 * for drop-in migration and behave identically to `hotkv://`/`hotkvs://`.
 * @param url NUL-terminated URL string.
 * @param out Output struct, overwritten unconditionally.
 * @return HOTKV_OK on success, HOTKV_ERR_PROTOCOL on a malformed URL,
 *         HOTKV_ERR_OOM on allocation failure, HOTKV_ERR_OTHER on NULL args.
 */
HOTKV_API int hotkv_url_parse(const char *url, hotkv_url_t *out);

/** @brief Free the strings owned by a parsed URL (NULL-safe). */
HOTKV_API void hotkv_url_free(hotkv_url_t *out);

/**
 * @brief Compute the raw CRC16/XMODEM checksum of a buffer (polynomial
 * 0x1021, initial value 0, no reflection): the same variant HotKV
 * cluster mode uses. crc16("123456789", 9) == 0x31C3.
 */
HOTKV_API uint16_t hotkv_crc16(const char *buf, size_t len);

/**
 * @brief Compute the cluster hash slot (0..16383) for a key, honoring
 * `{hash tag}` substrings exactly like `CLUSTER KEYSLOT`: if the key
 * contains '{', and a '}' follows it with at least one byte in between,
 * only the bytes between them are hashed; otherwise the whole key is
 * hashed.
 */
HOTKV_API uint16_t hotkv_key_slot(const char *key, size_t len);

/**
 * @brief Generic check for whether an error reply's prefix word (the text
 * before the first space) equals prefix exactly, e.g. "MOVED", "ASK",
 * "WRONGTYPE", "NOAUTH", "READONLY", "NOLICENSE".
 */
HOTKV_API int hotkv_reply_has_error_prefix(const hotkv_reply_t *reply, const char *prefix);

/**
 * @brief Copy an error reply's prefix word into buf (truncated to fit).
 * @return buf on success, NULL if reply is not an error reply or args are invalid.
 */
HOTKV_API const char *hotkv_reply_error_prefix(const hotkv_reply_t *reply, char *buf,
                                                size_t buflen);

/** @return Non-zero if reply is a MOVED cluster redirect error. */
HOTKV_API int hotkv_reply_is_moved(const hotkv_reply_t *reply);
/** @return Non-zero if reply is an ASK cluster redirect error. */
HOTKV_API int hotkv_reply_is_ask(const hotkv_reply_t *reply);
/** @return Non-zero if reply is a WRONGTYPE error. */
HOTKV_API int hotkv_reply_is_wrongtype(const hotkv_reply_t *reply);
/** @return Non-zero if reply is a NOAUTH error. */
HOTKV_API int hotkv_reply_is_noauth(const hotkv_reply_t *reply);
/** @return Non-zero if reply is a READONLY error (write sent to a replica). */
HOTKV_API int hotkv_reply_is_readonly(const hotkv_reply_t *reply);

/* =========================================================================
 * AI helpers: PromptCache and SemanticCache
 * ========================================================================= */

/**
 * @brief Build the deterministic PromptCache cache key for a request:
 * canonical compact sorted-key JSON of {provider, model, system, messages,
 * params}, SHA-256 hashed to a lowercase hex string. Every official HotKV
 * SDK builds this exact same key for the same logical input, so it is
 * safe to share a PCACHE namespace across
 * services written in different languages.
 *
 * @param provider Provider name, e.g. "openai" (never NULL; use "").
 * @param model Model name, e.g. "gpt-4" (never NULL; use "").
 * @param system System prompt, or NULL (treated as "").
 * @param msg_roles Array of nmessages role strings, in order.
 * @param msg_contents Array of nmessages content strings, in order.
 * @param nmessages Number of messages.
 * @param param_keys Array of nparams generation-parameter names.
 * @param param_values Array of nparams string-formatted values (format
 *        numbers yourself, e.g. "0.7", so keys are stable across SDKs).
 * @param nparams Number of parameters. Sorted internally; caller order
 *        does not matter.
 * @return Heap-allocated 64-character hex string (free with hotkv_free()),
 *         or NULL on allocation failure.
 */
HOTKV_API char *hotkv_promptcache_key(const char *provider, const char *model,
                                       const char *system, const char **msg_roles,
                                       const char **msg_contents, size_t nmessages,
                                       const char **param_keys, const char **param_values,
                                       size_t nparams);

/**
 * @brief Token/cost/latency accounting for one LLM call, for
 * LLMSTATS.RECORD and, via model, for attributing tokens/cost to a stored
 * cache entry (see hotkv_promptcache_get_or_call(),
 * hotkv_semanticcache_get_or_call()).
 *
 * model is the provider's model name; leave it NULL or "" (the default) to
 * skip attaching a usage group to the cache entry (LLMSTATS.RECORD
 * accounting, when enabled, still uses cost_usd as given).
 * cache_read_tokens is provider prompt-cache hit tokens;
 * cache_write_5m_tokens/cache_write_1h_tokens are Anthropic-style
 * cache-write tokens by TTL tier (leave both 0 for providers without a
 * separate cache-write cost, e.g. OpenAI).
 */
typedef struct hotkv_llm_usage {
    const char *model;
    uint64_t input_tokens;
    uint64_t output_tokens;
    uint64_t cache_read_tokens;
    uint64_t cache_write_5m_tokens;
    uint64_t cache_write_1h_tokens;
    double cost_usd;
    uint64_t latency_ms;
} hotkv_llm_usage_t;

/**
 * @brief Callback invoked by hotkv_promptcache_get_or_call() on a cache
 * miss. Must set *out_response to a hotkv_strdup()-compatible heap string
 * (freed by the caller of get_or_call, not by fn) and fill *out_usage.
 * @return HOTKV_OK on success, any other HOTKV_ERR_* to abort.
 */
typedef int (*hotkv_llm_call_fn)(void *userdata, char **out_response,
                                  hotkv_llm_usage_t *out_usage);

/** PromptCache: a get-or-call cache in front of PCACHE.* (needs
 * a license with the prompt-cache feature). Caller-allocated; init/destroy own its
 * small heap fields. */
typedef struct hotkv_promptcache {
    hotkv_context_t *ctx;
    char *ns;
    uint32_t ttl_seconds;
    char *stats_bucket;
} hotkv_promptcache_t;

/** @brief Initialize pc to store entries in namespace ns with the given
 * default TTL in seconds (0 = no expiry). Does not touch the server;
 * call hotkv_promptcache_create_namespace() once before first use. */
HOTKV_API int hotkv_promptcache_init(hotkv_promptcache_t *pc, hotkv_context_t *ctx,
                                      const char *ns, uint32_t ttl_seconds);
/** @brief Enable LLMSTATS.RECORD accounting on every get_or_call (hit or
 * miss) against bucket. Pass NULL to disable (the default). */
HOTKV_API void hotkv_promptcache_set_stats_bucket(hotkv_promptcache_t *pc, const char *bucket);
/** @brief Free pc's owned strings (does not close ctx or touch the server). */
HOTKV_API void hotkv_promptcache_destroy(hotkv_promptcache_t *pc);
/** @brief PCACHE.CREATE the namespace (max_entries 0 = engine default). */
HOTKV_API int hotkv_promptcache_create_namespace(hotkv_promptcache_t *pc, uint64_t max_entries,
                                                  double cost_per_hit);
/**
 * @brief Return the cached response for this request if present, otherwise
 * call fn, cache its response together with any usage fn reports (model
 * required for it to be attached; see hotkv_llm_usage_t), and return that.
 * @param out_response Set to a heap string the caller must hotkv_free().
 * @param out_hit Set to 1 on a cache hit, 0 on a miss (may be NULL).
 * @return HOTKV_OK on success, or fn's error code, or an OOM/OTHER code.
 */
HOTKV_API int hotkv_promptcache_get_or_call(hotkv_promptcache_t *pc, const char *provider,
                                             const char *model, const char *system,
                                             const char **msg_roles, const char **msg_contents,
                                             size_t nmessages, const char **param_keys,
                                             const char **param_values, size_t nparams,
                                             hotkv_llm_call_fn fn, void *userdata,
                                             char **out_response, int *out_hit);
/** @brief Current PCACHE.STATS for this cache's namespace, including how
 * many tokens and how much cost recorded usage has saved on hits.
 * @return HOTKV_OK, or the error hotkv_pcache_stats() returned. */
HOTKV_API int hotkv_promptcache_stats(hotkv_promptcache_t *pc, hotkv_pcache_stats_t *out);

/**
 * @brief Callback invoked by hotkv_semanticcache_get_or_call() on a cache
 * miss. Must set *out_response to a heap string the caller of get_or_call
 * will free, and fill *out_usage (model required for it to be attached to
 * the stored entry; see hotkv_llm_usage_t).
 * @return HOTKV_OK on success, any other HOTKV_ERR_* to abort.
 */
typedef int (*hotkv_text_call_fn)(void *userdata, char **out_response,
                                   hotkv_llm_usage_t *out_usage);

/** SemanticCache: a get-or-call cache in front of SCACHE.* (needs
 * a license with the semantic-cache feature). Never computes embeddings itself; the
 * caller supplies vectors from whatever embedding provider it uses. */
typedef struct hotkv_semanticcache {
    hotkv_context_t *ctx;
    char *ns;
    uint32_t ttl_seconds;
} hotkv_semanticcache_t;

/** @brief Initialize sc to store entries in namespace ns with the given
 * default TTL in seconds (0 = no expiry). */
HOTKV_API int hotkv_semanticcache_init(hotkv_semanticcache_t *sc, hotkv_context_t *ctx,
                                        const char *ns, uint32_t ttl_seconds);
/** @brief Free sc's owned strings (does not close ctx or touch the server). */
HOTKV_API void hotkv_semanticcache_destroy(hotkv_semanticcache_t *sc);
/** @brief SCACHE.CREATE the namespace. metric NULL defaults to "cosine";
 * threshold NULL defaults to the engine's 0.85. */
HOTKV_API int hotkv_semanticcache_create_namespace(hotkv_semanticcache_t *sc, uint32_t dims,
                                                    const char *metric, const double *threshold);
/**
 * @brief Return the cached answer for the nearest neighbor of vector above
 * the namespace's similarity threshold, if any; otherwise call fn, cache
 * (query, response, vector) together with any usage fn reports, and return
 * the fresh answer.
 * @param out_response Set to a heap string the caller must hotkv_free().
 * @param out_hit Set to 1 on a cache hit, 0 on a miss (may be NULL).
 */
HOTKV_API int hotkv_semanticcache_get_or_call(hotkv_semanticcache_t *sc, const char *query,
                                               size_t querylen, const float *vector, size_t dims,
                                               hotkv_text_call_fn fn, void *userdata,
                                               char **out_response, int *out_hit);
/** @brief Current SCACHE.STATS for this cache's namespace, including how
 * many tokens and how much cost recorded usage has saved on hits.
 * @return HOTKV_OK, or the error hotkv_scache_stats() returned. */
HOTKV_API int hotkv_semanticcache_stats(hotkv_semanticcache_t *sc, hotkv_scache_stats_t *out);

/* =========================================================================
 * Connection pool
 * ========================================================================= */

/** Opaque connection pool handle. */
typedef struct hotkv_pool hotkv_pool_t;

/**
 * @brief Pool configuration.
 */
typedef struct hotkv_pool_options {
    hotkv_options_t conn_opts;       /**< Connection options for each pooled connection. */
    size_t min_connections;          /**< Minimum idle connections (default: 1). */
    size_t max_connections;          /**< Maximum total connections (default: 10). */
    int health_check_interval_sec;   /**< Seconds between PING health checks (0 = disabled). */
} hotkv_pool_options_t;

/**
 * @brief Create a connection pool.
 * @param opts Pool options.
 * @return Pool handle, or NULL on error.
 */
HOTKV_API hotkv_pool_t *hotkv_pool_new(const hotkv_pool_options_t *opts);

/**
 * @brief Destroy a pool and close all connections.
 * @param pool Pool handle (NULL-safe).
 */
HOTKV_API void hotkv_pool_free(hotkv_pool_t *pool);

/**
 * @brief Acquire a connection from the pool.
 *
 * Blocks until a connection is available or the pool fails to create one.
 *
 * @param pool Pool handle.
 * @return Context pointer, or NULL on error.
 */
HOTKV_API hotkv_context_t *hotkv_pool_acquire(hotkv_pool_t *pool);

/**
 * @brief Return a connection to the pool.
 * @param pool Pool handle.
 * @param ctx  Context to return.
 */
HOTKV_API void hotkv_pool_release(hotkv_pool_t *pool, hotkv_context_t *ctx);

/**
 * @brief Return the number of currently active (acquired) connections.
 * @param pool Pool handle.
 * @return Count.
 */
HOTKV_API size_t hotkv_pool_active(const hotkv_pool_t *pool);

/**
 * @brief Return the number of idle connections in the pool.
 * @param pool Pool handle.
 * @return Count.
 */
HOTKV_API size_t hotkv_pool_idle(const hotkv_pool_t *pool);

/* =========================================================================
 * TLS (compile-time opt-in)
 * ========================================================================= */

#ifdef HOTKV_USE_SSL

/**
 * @brief Initialize the global OpenSSL library (call once).
 * @return HOTKV_OK on success.
 */
HOTKV_API int hotkv_ssl_init(void);

/**
 * @brief Run the TLS handshake on a connected context. hotkv_connect_with_options
 * already does this when opts->use_tls (or a tls_* path) is set, and calling this
 * on a context that is already encrypted is a no-op returning HOTKV_OK. The
 * server certificate is verified against opts->tls_ca / tls_ca_dir, or the system
 * trust store when neither is given, and matched against opts->tls_servername
 * (default: the host, which may be a DNS name or an IP address).
 * @param ctx Context.
 * @param opts Connection options containing TLS paths.
 * @return HOTKV_OK on success, error code on failure.
 */
HOTKV_API int hotkv_ssl_connect(hotkv_context_t *ctx, const hotkv_options_t *opts);

#endif /* HOTKV_USE_SSL */

#ifdef __cplusplus
}
#endif

#endif /* HOTKV_H */
