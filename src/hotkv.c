/**
 * @file hotkv.c
 * @brief Core connection, I/O, and command execution.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "hotkv_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

/* =========================================================================
 * Platform networking init
 * ========================================================================= */

#ifdef _WIN32
static int s_wsa_initialized = 0;
#endif

int hotkv_net_init(void) {
#ifdef _WIN32
    if (!s_wsa_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return HOTKV_ERR_IO;
        }
        s_wsa_initialized = 1;
    }
#endif
    return HOTKV_OK;
}

/* =========================================================================
 * Socket helpers
 * ========================================================================= */

int hotkv_set_nonblocking(hotkv_fd_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? HOTKV_OK : HOTKV_ERR_IO;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return HOTKV_ERR_IO;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 ? HOTKV_ERR_IO : HOTKV_OK;
#endif
}

int hotkv_set_blocking(hotkv_fd_t fd) {
#ifdef _WIN32
    u_long mode = 0;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? HOTKV_OK : HOTKV_ERR_IO;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return HOTKV_ERR_IO;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1 ? HOTKV_ERR_IO : HOTKV_OK;
#endif
}

int hotkv_set_tcp_nodelay(hotkv_fd_t fd) {
    int yes = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                      (const char *)&yes, sizeof(yes)) == 0 ? HOTKV_OK : HOTKV_ERR_IO;
}

int hotkv_set_keepalive(hotkv_fd_t fd) {
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                      (const char *)&yes, sizeof(yes)) == 0 ? HOTKV_OK : HOTKV_ERR_IO;
}

/* A write to a connection the server already closed must return an error, not
 * raise SIGPIPE and kill the host process. Linux does that per send() with
 * MSG_NOSIGNAL; macOS and the BSDs have a per-socket option instead. */
int hotkv_set_nosigpipe(hotkv_fd_t fd) {
#if defined(SO_NOSIGPIPE)
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                      (const char *)&yes, sizeof(yes)) == 0 ? HOTKV_OK : HOTKV_ERR_IO;
#else
    (void)fd;
    return HOTKV_OK;
#endif
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* =========================================================================
 * Error helpers
 * ========================================================================= */

void hotkv_set_error(hotkv_context_t *ctx, int type, const char *fmt, ...) {
    ctx->err = type;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->errstr, HOTKV_ERRSTR_LEN, fmt, ap);
    va_end(ap);
}

/* =========================================================================
 * TCP connect with optional timeout
 * ========================================================================= */

static hotkv_fd_t tcp_connect(const char *host, int port,
                               const struct timeval *timeout,
                               int *err, char *errstr) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0) {
        *err = HOTKV_ERR_IO;
        snprintf(errstr, HOTKV_ERRSTR_LEN, "getaddrinfo: %s", gai_strerror(gai));
        return HOTKV_INVALID_FD;
    }

    hotkv_fd_t fd = HOTKV_INVALID_FD;
    struct addrinfo *rp;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == HOTKV_INVALID_FD) continue;

        if (timeout && (timeout->tv_sec > 0 || timeout->tv_usec > 0)) {
            /* Non-blocking connect with timeout. */
            hotkv_set_nonblocking(fd);

            int rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
#ifdef _WIN32
            if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
#else
            if (rc != 0 && errno != EINPROGRESS) {
#endif
                hotkv_close_socket(fd);
                fd = HOTKV_INVALID_FD;
                continue;
            }

            if (rc != 0) {
                /* Wait for connect to complete. */
#ifdef _WIN32
                fd_set wfds, efds;
                FD_ZERO(&wfds);
                FD_ZERO(&efds);
                FD_SET(fd, &wfds);
                FD_SET(fd, &efds);
                struct timeval tv = *timeout;
                int sel = select(0, NULL, &wfds, &efds, &tv);
#else
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                int timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
                int sel = poll(&pfd, 1, timeout_ms);
#endif
                if (sel <= 0) {
                    *err = (sel == 0) ? HOTKV_ERR_TIMEOUT : HOTKV_ERR_IO;
                    snprintf(errstr, HOTKV_ERRSTR_LEN,
                             sel == 0 ? "connection timed out" : "poll/select error");
                    hotkv_close_socket(fd);
                    fd = HOTKV_INVALID_FD;
                    freeaddrinfo(res);
                    return HOTKV_INVALID_FD;
                }

                /* Check for connection error. */
                int sockerr = 0;
                socklen_t sockerrlen = sizeof(sockerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&sockerr, &sockerrlen);
                if (sockerr != 0) {
                    hotkv_close_socket(fd);
                    fd = HOTKV_INVALID_FD;
                    continue;
                }
            }

            hotkv_set_blocking(fd);
        } else {
            /* Blocking connect. */
            if (connect(fd, rp->ai_addr, rp->ai_addrlen) != 0) {
                hotkv_close_socket(fd);
                fd = HOTKV_INVALID_FD;
                continue;
            }
        }

        break; /* connected */
    }

    freeaddrinfo(res);

    if (fd == HOTKV_INVALID_FD) {
        *err = HOTKV_ERR_IO;
        snprintf(errstr, HOTKV_ERRSTR_LEN, "unable to connect to %s:%d", host, port);
    }

    return fd;
}

/* =========================================================================
 * Context lifecycle
 * ========================================================================= */

static hotkv_context_t *context_alloc(void) {
    hotkv_context_t *ctx = (hotkv_context_t *)hotkv_calloc(1, sizeof(hotkv_context_t));
    if (!ctx) return NULL;
    ctx->fd = HOTKV_INVALID_FD;
    ctx->protocol = HOTKV_RESP2;
    return ctx;
}

hotkv_context_t *hotkv_connect(const char *host, int port) {
    hotkv_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.host = host;
    opts.port = port;
    return hotkv_connect_with_options(&opts);
}

hotkv_context_t *hotkv_connect_with_options(const hotkv_options_t *opts) {
    if (hotkv_net_init() != HOTKV_OK) return NULL;

    hotkv_context_t *ctx = context_alloc();
    if (!ctx) return NULL;

    const char *host = opts->host ? opts->host : "127.0.0.1";
    int port = opts->port > 0 ? opts->port : 6379;

    ctx->host = hotkv_strdup(host);
    ctx->port = port;
    ctx->protocol = (opts->protocol == HOTKV_RESP3) ? HOTKV_RESP3 : HOTKV_RESP2;
    ctx->connect_timeout = opts->connect_timeout;
    ctx->command_timeout = opts->command_timeout;

    if (!ctx->host) {
        hotkv_set_error(ctx, HOTKV_ERR_OOM, "out of memory");
        return ctx;
    }

    /* Allocate reader. */
    size_t rbufsz = opts->reader_bufsz > 0 ? opts->reader_bufsz : HOTKV_BUF_INITIAL_SIZE;
    ctx->reader = hotkv_reader_new_with_size(rbufsz);
    if (!ctx->reader) {
        hotkv_set_error(ctx, HOTKV_ERR_OOM, "out of memory");
        return ctx;
    }

    /* Allocate output buffer. */
    size_t wbufsz = opts->writer_bufsz > 0 ? opts->writer_bufsz : HOTKV_BUF_INITIAL_SIZE;
    if (hotkv_buf_init(&ctx->obuf, wbufsz) != HOTKV_OK) {
        hotkv_set_error(ctx, HOTKV_ERR_OOM, "out of memory");
        return ctx;
    }

    /* TCP connect. */
    const struct timeval *tov = (opts->connect_timeout.tv_sec > 0 ||
                                  opts->connect_timeout.tv_usec > 0)
                                 ? &opts->connect_timeout : NULL;

    ctx->fd = tcp_connect(host, port, tov, &ctx->err, ctx->errstr);
    if (ctx->fd == HOTKV_INVALID_FD) {
        return ctx; /* error already set */
    }

    hotkv_set_tcp_nodelay(ctx->fd);
    hotkv_set_keepalive(ctx->fd);
    hotkv_set_nosigpipe(ctx->fd);

    /* Set command timeout if specified. */
    if (opts->command_timeout.tv_sec > 0 || opts->command_timeout.tv_usec > 0) {
        hotkv_set_timeout(ctx, opts->command_timeout);
    }

#ifdef HOTKV_USE_SSL
    /* TLS first: the password below must never cross the network in clear. */
    if (opts->use_tls || opts->tls_ca || opts->tls_ca_dir || opts->tls_cert) {
        if (hotkv_ssl_connect(ctx, opts) != HOTKV_OK) {
            return ctx; /* error already set */
        }
    }
#endif

    /* AUTH if password provided. */
    if (opts->password && opts->password[0] != '\0') {
        hotkv_reply_t *reply;
        if (opts->username && opts->username[0] != '\0') {
            reply = hotkv_command(ctx, "AUTH %s %s", opts->username, opts->password);
        } else {
            reply = hotkv_command(ctx, "AUTH %s", opts->password);
        }
        if (reply) {
            if (reply->type == HOTKV_REPLY_ERROR) {
                hotkv_set_error(ctx, HOTKV_ERR_OTHER, "%.*s",
                                (int)reply->len, reply->str);
            }
            hotkv_free_reply(reply);
        }
        if (ctx->err) return ctx;
    }

    /* HELLO protocol negotiation. */
    if (opts->protocol == HOTKV_RESP3) {
        hotkv_reply_t *reply = hotkv_command(ctx, "HELLO %d", 3);
        if (reply) {
            if (reply->type == HOTKV_REPLY_ERROR) {
                /* Server doesn't support RESP3, fall back to RESP2. */
                ctx->protocol = HOTKV_RESP2;
            } else {
                ctx->protocol = HOTKV_RESP3;
                ctx->reader->protocol = HOTKV_RESP3;
            }
            hotkv_free_reply(reply);
        }
        /* Clear any non-fatal error from HELLO failure. */
        if (ctx->err == HOTKV_ERR_OTHER) {
            ctx->err = HOTKV_OK;
            ctx->errstr[0] = '\0';
        }
    }

    /* SELECT database if non-zero. */
    if (opts->db > 0 && ctx->err == HOTKV_OK) {
        hotkv_reply_t *reply = hotkv_select(ctx, opts->db);
        if (reply) {
            if (reply->type == HOTKV_REPLY_ERROR) {
                hotkv_set_error(ctx, HOTKV_ERR_OTHER, "%.*s",
                                (int)reply->len, reply->str);
            } else {
                ctx->db = opts->db;
            }
            hotkv_free_reply(reply);
        }
    }

    return ctx;
}

hotkv_context_t *hotkv_connect_url(const char *url, const hotkv_options_t *overrides) {
    hotkv_url_t u;
    hotkv_options_t opts;
    hotkv_context_t *ctx;
    int rc;

    if (hotkv_net_init() != HOTKV_OK) return NULL;
    memset(&opts, 0, sizeof(opts));
    if (overrides) opts = *overrides;

    rc = url ? hotkv_url_parse(url, &u) : HOTKV_ERR_PROTOCOL;
    if (rc != HOTKV_OK) {
        ctx = context_alloc();
        if (ctx) {
            hotkv_set_error(ctx, rc == HOTKV_OK ? HOTKV_ERR_OTHER : rc,
                            rc == HOTKV_ERR_OOM ? "out of memory" : "invalid connection URL");
        }
        return ctx;
    }

    opts.host = u.host;
    opts.port = u.port;
    opts.username = u.username;
    opts.password = u.password;
    opts.db = u.db;
    if (u.use_tls) {
#ifdef HOTKV_USE_SSL
        opts.use_tls = 1;
#else
        ctx = context_alloc();
        if (ctx) {
            hotkv_set_error(ctx, HOTKV_ERR_OTHER,
                            "the URL asks for TLS but libhotkv was built without HOTKV_USE_SSL");
        }
        hotkv_url_free(&u);
        return ctx;
#endif
    }

    ctx = hotkv_connect_with_options(&opts);
    hotkv_url_free(&u);
    return ctx;
}

void hotkv_disconnect(hotkv_context_t *ctx) {
    if (!ctx) return;

#ifdef HOTKV_USE_SSL
    if (ctx->ssl) {
        SSL_shutdown(ctx->ssl);
        SSL_free(ctx->ssl);
    }
    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
    }
#endif

    if (ctx->fd != HOTKV_INVALID_FD) {
        hotkv_close_socket(ctx->fd);
    }

    hotkv_reader_free(ctx->reader);
    hotkv_buf_destroy(&ctx->obuf);
    hotkv_free(ctx->host);
    hotkv_free(ctx);
}

int hotkv_context_err(const hotkv_context_t *ctx) {
    return ctx ? ctx->err : HOTKV_ERR_OTHER;
}

const char *hotkv_context_errstr(const hotkv_context_t *ctx) {
    return ctx ? ctx->errstr : "NULL context";
}

hotkv_fd_t hotkv_context_fd(const hotkv_context_t *ctx) {
    return ctx ? ctx->fd : HOTKV_INVALID_FD;
}

int hotkv_set_timeout(hotkv_context_t *ctx, struct timeval tv) {
    if (!ctx || ctx->fd == HOTKV_INVALID_FD) return HOTKV_ERR_IO;

    ctx->command_timeout = tv;

#ifdef _WIN32
    DWORD ms = (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    setsockopt(ctx->fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    setsockopt(ctx->fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
    setsockopt(ctx->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(ctx->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    return HOTKV_OK;
}

/* =========================================================================
 * I/O: flush and read
 * ========================================================================= */

#ifdef HOTKV_USE_SSL
/* Maps a failed SSL_read / SSL_write (ret <= 0) to a context error. */
static int hotkv_ssl_io_error(hotkv_context_t *ctx, int ret, const char *what) {
    int e = SSL_get_error(ctx->ssl, ret);
    if (e == SSL_ERROR_ZERO_RETURN) {
        hotkv_set_error(ctx, HOTKV_ERR_EOF, "server closed connection");
    } else if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
        hotkv_set_error(ctx, HOTKV_ERR_TIMEOUT, "%s timed out", what);
#ifdef _WIN32
    } else if (e == SSL_ERROR_SYSCALL && WSAGetLastError() == WSAETIMEDOUT) {
#else
    } else if (e == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
#endif
        hotkv_set_error(ctx, HOTKV_ERR_TIMEOUT, "%s timed out", what);
    } else if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0 && ret == 0) {
        hotkv_set_error(ctx, HOTKV_ERR_EOF, "server closed connection");
    } else {
        char buf[200];
        unsigned long err = ERR_get_error();
        if (err) {
            ERR_error_string_n(err, buf, sizeof(buf));
        } else {
            snprintf(buf, sizeof(buf), "TLS error %d", e);
        }
        hotkv_set_error(ctx, HOTKV_ERR_IO, "TLS %s error: %s", what, buf);
    }
    return ctx->err;
}
#endif

int hotkv_flush(hotkv_context_t *ctx) {
    hotkv_buf_t *obuf = &ctx->obuf;

    while (obuf->pos < obuf->len) {
        const char *data = obuf->data + obuf->pos;
        size_t remaining = obuf->len - obuf->pos;

        hotkv_ssize_t nwritten;
#ifdef HOTKV_USE_SSL
        if (ctx->ssl) {
            int chunk = remaining > 16384 ? 16384 : (int)remaining;
            int n = SSL_write(ctx->ssl, data, chunk);
            if (n <= 0) return hotkv_ssl_io_error(ctx, n, "write");
            nwritten = (hotkv_ssize_t)n;
        } else
#endif
        {
#ifdef _WIN32
            nwritten = (hotkv_ssize_t)send(ctx->fd, data, (int)remaining, 0);
#else
            do {
                nwritten = (hotkv_ssize_t)send(ctx->fd, data, remaining, MSG_NOSIGNAL);
            } while (nwritten < 0 && errno == EINTR);
#endif
        }

        if (nwritten <= 0) {
            if (nwritten == 0) {
                hotkv_set_error(ctx, HOTKV_ERR_EOF, "server closed connection");
            } else {
#ifdef _WIN32
                int e = WSAGetLastError();
                if (e == WSAETIMEDOUT) {
                    hotkv_set_error(ctx, HOTKV_ERR_TIMEOUT, "write timed out");
                } else {
                    hotkv_set_error(ctx, HOTKV_ERR_IO, "write error: %d", e);
                }
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    hotkv_set_error(ctx, HOTKV_ERR_TIMEOUT, "write timed out");
                } else {
                    hotkv_set_error(ctx, HOTKV_ERR_IO, "write error: %s",
                                    strerror(errno));
                }
#endif
            }
            return ctx->err;
        }

        obuf->pos += (size_t)nwritten;
    }

    /* Reset output buffer after full flush. */
    obuf->len = 0;
    obuf->pos = 0;

    return HOTKV_OK;
}

int hotkv_read(hotkv_context_t *ctx) {
    char tmp[4096];
    hotkv_ssize_t nread;

#ifdef HOTKV_USE_SSL
    if (ctx->ssl) {
        int n = SSL_read(ctx->ssl, tmp, (int)sizeof(tmp));
        if (n <= 0) return hotkv_ssl_io_error(ctx, n, "read");
        nread = (hotkv_ssize_t)n;
    } else
#endif
    {
        do {
            nread = (hotkv_ssize_t)recv(ctx->fd, tmp, (int)sizeof(tmp), 0);
#ifndef _WIN32
        } while (nread < 0 && errno == EINTR);
#else
        } while (0);
#endif
    }

    if (nread < 0) {
#ifdef _WIN32
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) {
            hotkv_set_error(ctx, HOTKV_ERR_TIMEOUT, "read timed out");
        } else {
            hotkv_set_error(ctx, HOTKV_ERR_IO, "read error: %d", e);
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            hotkv_set_error(ctx, HOTKV_ERR_TIMEOUT, "read timed out");
        } else {
            hotkv_set_error(ctx, HOTKV_ERR_IO, "read error: %s", strerror(errno));
        }
#endif
        return ctx->err;
    }

    if (nread == 0) {
        hotkv_set_error(ctx, HOTKV_ERR_EOF, "server closed connection");
        return HOTKV_ERR_EOF;
    }

    int rc = hotkv_reader_feed(ctx->reader, tmp, (size_t)nread);
    if (rc != HOTKV_OK) {
        hotkv_set_error(ctx, HOTKV_ERR_OOM, "out of memory feeding reader");
    }
    return rc;
}

/* =========================================================================
 * Command execution
 * ========================================================================= */

hotkv_reply_t *hotkv_blocking_command(hotkv_context_t *ctx) {
    /* Flush the output buffer. */
    int rc = hotkv_flush(ctx);
    if (rc != HOTKV_OK) return NULL;

    /* Read replies until we have one complete. */
    for (;;) {
        hotkv_reply_t *reply = NULL;
        rc = hotkv_reader_next(ctx->reader, &reply);
        if (rc != HOTKV_OK) {
            hotkv_set_error(ctx, rc, "%s", ctx->reader->errstr);
            return NULL;
        }
        if (reply != NULL) {
            if (ctx->pipeline_count > 0) ctx->pipeline_count--;
            return reply;
        }
        /* Need more data from the socket. */
        rc = hotkv_read(ctx);
        if (rc != HOTKV_OK) return NULL;
    }
}

hotkv_reply_t *hotkv_command(hotkv_context_t *ctx, const char *format, ...) {
    if (!ctx || ctx->err) return NULL;

    va_list ap;
    va_start(ap, format);
    int rc = hotkv_format_command(&ctx->obuf, format, ap);
    va_end(ap);

    if (rc != HOTKV_OK) {
        hotkv_set_error(ctx, rc, "failed to format command");
        return NULL;
    }

    return hotkv_blocking_command(ctx);
}

hotkv_reply_t *hotkv_command_argv(hotkv_context_t *ctx, int argc,
                                   const char **argv, const size_t *argvlen) {
    if (!ctx || ctx->err) return NULL;

    int rc = hotkv_format_command_argv(&ctx->obuf, argc, argv, argvlen);
    if (rc != HOTKV_OK) {
        hotkv_set_error(ctx, rc, "failed to format command");
        return NULL;
    }

    return hotkv_blocking_command(ctx);
}

/* =========================================================================
 * Pipelining
 * ========================================================================= */

int hotkv_append_command(hotkv_context_t *ctx, const char *format, ...) {
    if (!ctx) return HOTKV_ERR_OTHER;

    va_list ap;
    va_start(ap, format);
    int rc = hotkv_format_command(&ctx->obuf, format, ap);
    va_end(ap);

    if (rc == HOTKV_OK) ctx->pipeline_count++;
    return rc;
}

int hotkv_append_command_argv(hotkv_context_t *ctx, int argc,
                               const char **argv, const size_t *argvlen) {
    if (!ctx) return HOTKV_ERR_OTHER;

    int rc = hotkv_format_command_argv(&ctx->obuf, argc, argv, argvlen);
    if (rc == HOTKV_OK) ctx->pipeline_count++;
    return rc;
}

int hotkv_get_reply(hotkv_context_t *ctx, hotkv_reply_t **reply) {
    if (!ctx || !reply) return HOTKV_ERR_OTHER;
    *reply = NULL;

    /* First check if the reader already has a buffered complete reply. */
    int rc = hotkv_reader_next(ctx->reader, reply);
    if (rc != HOTKV_OK) {
        hotkv_set_error(ctx, rc, "%s", ctx->reader->errstr);
        return rc;
    }
    if (*reply) {
        if (ctx->pipeline_count > 0) ctx->pipeline_count--;
        return HOTKV_OK;
    }

    /* Flush output then read from socket. */
    rc = hotkv_flush(ctx);
    if (rc != HOTKV_OK) return rc;

    for (;;) {
        rc = hotkv_read(ctx);
        if (rc != HOTKV_OK) return rc;

        rc = hotkv_reader_next(ctx->reader, reply);
        if (rc != HOTKV_OK) {
            hotkv_set_error(ctx, rc, "%s", ctx->reader->errstr);
            return rc;
        }
        if (*reply) {
            if (ctx->pipeline_count > 0) ctx->pipeline_count--;
            return HOTKV_OK;
        }
    }
}
