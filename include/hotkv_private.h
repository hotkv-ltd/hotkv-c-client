/**
 * @file hotkv_private.h
 * @brief libhotkv internal types and helpers, not part of the public API.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#ifndef HOTKV_PRIVATE_H
#define HOTKV_PRIVATE_H

#include "hotkv.h"

#include <stdarg.h>
#include <string.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int hotkv_ssize_t;
#else
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <poll.h>
    typedef ssize_t hotkv_ssize_t;
#endif

#ifdef HOTKV_USE_SSL
    #include <openssl/ssl.h>
    #include <openssl/err.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Memory allocator
 * ========================================================================= */

/* Exported (HOTKV_API) because the unit tests link against the shared
 * library too, and Windows DLLs only export what is marked. */
HOTKV_API void *hotkv_malloc(size_t size);
HOTKV_API void *hotkv_calloc(size_t count, size_t size);
HOTKV_API void *hotkv_realloc(void *ptr, size_t size);
HOTKV_API void  hotkv_free(void *ptr);
HOTKV_API char *hotkv_strdup(const char *s);

/* =========================================================================
 * Dynamic buffer
 * ========================================================================= */

#define HOTKV_BUF_INITIAL_SIZE  16384  /* 16 KB */

typedef struct hotkv_buf {
    char  *data;
    size_t len;     /* bytes used */
    size_t cap;     /* bytes allocated */
    size_t pos;     /* read cursor (consumed bytes) */
} hotkv_buf_t;

HOTKV_API int  hotkv_buf_init(hotkv_buf_t *buf, size_t initial_cap);
HOTKV_API void hotkv_buf_destroy(hotkv_buf_t *buf);
HOTKV_API int  hotkv_buf_ensure(hotkv_buf_t *buf, size_t additional);
HOTKV_API int  hotkv_buf_append(hotkv_buf_t *buf, const char *data, size_t len);
HOTKV_API void hotkv_buf_consume(hotkv_buf_t *buf, size_t n);
HOTKV_API void hotkv_buf_compact(hotkv_buf_t *buf);

static inline size_t hotkv_buf_readable(const hotkv_buf_t *buf) {
    return buf->len - buf->pos;
}

static inline const char *hotkv_buf_readptr(const hotkv_buf_t *buf) {
    return buf->data + buf->pos;
}

/* =========================================================================
 * RESP reader internals
 * ========================================================================= */

/** Maximum nesting depth for arrays/maps to prevent stack overflow. */
#define HOTKV_MAX_DEPTH       32

/** Maximum array/map element count to prevent memory exhaustion. */
#define HOTKV_MAX_ELEMENTS    1048576   /* 1M */

/** Maximum bulk string length. */
#define HOTKV_MAX_BULK_LEN    536870912 /* 512 MB */

/**
 * @brief Internal RESP reader state.
 */
struct hotkv_reader {
    hotkv_buf_t buf;
    int  err;
    char errstr[HOTKV_ERRSTR_LEN];
    int  protocol;  /* HOTKV_RESP2 or HOTKV_RESP3 */
};

/* =========================================================================
 * Connection context internals
 * ========================================================================= */

struct hotkv_context {
    hotkv_fd_t  fd;
    int         err;
    char        errstr[HOTKV_ERRSTR_LEN];

    hotkv_reader_t *reader;
    hotkv_buf_t     obuf;      /* output (write) buffer */

    int         protocol;      /* negotiated RESP version */
    int         db;            /* selected database index */

    struct timeval connect_timeout;
    struct timeval command_timeout;

    char       *host;
    int         port;

    int         pipeline_count; /* number of pending pipelined commands */

#ifdef HOTKV_USE_SSL
    SSL        *ssl;
    SSL_CTX    *ssl_ctx;
#endif
};

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/** Set an error on a context. */
void hotkv_set_error(hotkv_context_t *ctx, int type, const char *fmt, ...);

/** Set an error on a reader. */
void hotkv_reader_set_error(hotkv_reader_t *reader, int type, const char *fmt, ...);

/** Write all bytes in obuf to socket. Returns HOTKV_OK or error code. */
int hotkv_flush(hotkv_context_t *ctx);

/** Read from socket into reader buffer. Returns HOTKV_OK or error code. */
int hotkv_read(hotkv_context_t *ctx);

/** Format a RESP command into the output buffer from a format string + va_list. */
HOTKV_API int hotkv_format_command(hotkv_buf_t *obuf, const char *format, va_list ap);

/** Format a RESP command into the output buffer from an argv array. */
HOTKV_API int hotkv_format_command_argv(hotkv_buf_t *obuf, int argc,
                                         const char **argv, const size_t *argvlen);

/* =========================================================================
 * Number formatting for the wire (src/hotkv_num.c)
 *
 * Locale independent, never uses exponent notation. All return the length
 * written (NUL terminated), or 0 on failure (NaN, a value the target parser
 * cannot take, or a buffer that is too small).
 * ========================================================================= */

/** Buffer size that always fits hotkv_fmt_double / hotkv_fmt_float output. */
#define HOTKV_FMT_NUM_MAX 400

/** Shortest plain-decimal text that reads back as exactly v. +-inf give
 *  "inf" / "-inf" (strtod style); NaN fails. */
HOTKV_API size_t hotkv_fmt_double(char *buf, size_t bufsz, double v);

/** Same for a float: the shortest text that reads back as the same float. */
HOTKV_API size_t hotkv_fmt_float(char *buf, size_t bufsz, float v);

/** A USD amount as HotKV parses it: [0-9]+(.[0-9]{1,6})?, rounded to the
 *  nearest micro-dollar. Fails for negative, NaN, infinite or too large. */
HOTKV_API size_t hotkv_fmt_usd(char *buf, size_t bufsz, double usd);

/** Send a command and read one reply (sync). */
hotkv_reply_t *hotkv_blocking_command(hotkv_context_t *ctx);

/** Allocate a reply node. */
hotkv_reply_t *hotkv_reply_alloc(void);

/** Build an error reply that never touched the network (an argument that
 *  cannot be represented on the wire). Unlike hotkv_set_error() it leaves the
 *  connection usable. NULL only on out-of-memory. */
hotkv_reply_t *hotkv_local_error_reply(const char *msg);

/** Portable close socket. */
static inline void hotkv_close_socket(hotkv_fd_t fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

/** Portable socket error code. */
static inline int hotkv_socket_errno(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

/** Set socket to non-blocking mode. */
int hotkv_set_nonblocking(hotkv_fd_t fd);

/** Set socket to blocking mode. */
int hotkv_set_blocking(hotkv_fd_t fd);

/** Make writes to a closed peer fail with an error instead of raising SIGPIPE
 *  (macOS/BSD; Linux uses MSG_NOSIGNAL per send). */
int hotkv_set_nosigpipe(hotkv_fd_t fd);

/** Set TCP_NODELAY on socket. */
int hotkv_set_tcp_nodelay(hotkv_fd_t fd);

/** Set SO_KEEPALIVE on socket. */
int hotkv_set_keepalive(hotkv_fd_t fd);

/** Platform init (Winsock on Windows). */
int hotkv_net_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HOTKV_PRIVATE_H */
