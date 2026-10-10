/**
 * @file hotkv_ssl.c
 * @brief TLS support via OpenSSL (compile-time opt-in via HOTKV_USE_SSL).
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#ifdef HOTKV_USE_SSL

#include "hotkv_private.h"

#include <stdio.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

static int s_ssl_initialized = 0;

int hotkv_ssl_init(void) {
    if (s_ssl_initialized) return HOTKV_OK;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    s_ssl_initialized = 1;
    return HOTKV_OK;
}

/* True when `host` is an IPv4 or IPv6 literal. Such a name is matched against
 * IP subject alternative names and must not be sent as SNI (RFC 6066). */
static int is_ip_literal(const char *host) {
    unsigned char buf[16];
    if (!host) return 0;
    return inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1;
}

static int ssl_fail(hotkv_context_t *ctx, SSL *ssl, SSL_CTX *ssl_ctx, const char *what) {
    char errbuf[256];
    unsigned long e = ERR_get_error();
    long vr = ssl ? SSL_get_verify_result(ssl) : X509_V_OK;
    if (e) {
        ERR_error_string_n(e, errbuf, sizeof(errbuf));
    } else {
        snprintf(errbuf, sizeof(errbuf), "connection closed during the handshake");
    }
    if (vr != X509_V_OK) {
        hotkv_set_error(ctx, HOTKV_ERR_IO, "%s: %s (certificate: %s)", what, errbuf,
                        X509_verify_cert_error_string(vr));
    } else {
        hotkv_set_error(ctx, HOTKV_ERR_IO, "%s: %s", what, errbuf);
    }
    if (ssl) SSL_free(ssl);
    if (ssl_ctx) SSL_CTX_free(ssl_ctx);
    return HOTKV_ERR_IO;
}

int hotkv_ssl_connect(hotkv_context_t *ctx, const hotkv_options_t *opts) {
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    const char *name;
    int rc;

    if (!ctx || ctx->fd == HOTKV_INVALID_FD) return HOTKV_ERR_IO;
    if (ctx->ssl) return HOTKV_OK; /* already encrypted */

    hotkv_ssl_init();

    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) {
        hotkv_set_error(ctx, HOTKV_ERR_IO, "SSL_CTX_new failed");
        return HOTKV_ERR_IO;
    }

    /* Set minimum TLS version to 1.2. */
    SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);

    /* Trust anchors: the given CA file / directory, else the system store. */
    if (opts->tls_ca || opts->tls_ca_dir) {
        if (opts->tls_ca && SSL_CTX_load_verify_locations(ssl_ctx, opts->tls_ca, NULL) != 1) {
            hotkv_set_error(ctx, HOTKV_ERR_IO, "failed to load CA certificate: %s", opts->tls_ca);
            SSL_CTX_free(ssl_ctx);
            return HOTKV_ERR_IO;
        }
        if (opts->tls_ca_dir && SSL_CTX_load_verify_locations(ssl_ctx, NULL, opts->tls_ca_dir) != 1) {
            hotkv_set_error(ctx, HOTKV_ERR_IO, "failed to load CA directory: %s", opts->tls_ca_dir);
            SSL_CTX_free(ssl_ctx);
            return HOTKV_ERR_IO;
        }
    } else if (!opts->tls_insecure_skip_verify) {
        SSL_CTX_set_default_verify_paths(ssl_ctx);
    }

    /* Client certificate for servers with tls-auth-clients. */
    if (opts->tls_cert) {
        if (SSL_CTX_use_certificate_chain_file(ssl_ctx, opts->tls_cert) != 1) {
            hotkv_set_error(ctx, HOTKV_ERR_IO, "failed to load client certificate: %s", opts->tls_cert);
            SSL_CTX_free(ssl_ctx);
            return HOTKV_ERR_IO;
        }
        if (!opts->tls_key ||
            SSL_CTX_use_PrivateKey_file(ssl_ctx, opts->tls_key, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ssl_ctx) != 1) {
            hotkv_set_error(ctx, HOTKV_ERR_IO, "failed to load a matching client private key%s%s",
                            opts->tls_key ? ": " : "", opts->tls_key ? opts->tls_key : "");
            SSL_CTX_free(ssl_ctx);
            return HOTKV_ERR_IO;
        }
    }

    ssl = SSL_new(ssl_ctx);
    if (!ssl) {
        hotkv_set_error(ctx, HOTKV_ERR_IO, "SSL_new failed");
        SSL_CTX_free(ssl_ctx);
        return HOTKV_ERR_IO;
    }

    name = opts->tls_servername ? opts->tls_servername : (ctx->host ? ctx->host : "127.0.0.1");
    if (is_ip_literal(name)) {
        /* IP literal: verify against IP SANs, send no SNI. */
        if (!opts->tls_insecure_skip_verify &&
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name) != 1) {
            return ssl_fail(ctx, ssl, ssl_ctx, "cannot verify the server address");
        }
    } else {
        SSL_set_tlsext_host_name(ssl, name);
        if (!opts->tls_insecure_skip_verify && SSL_set1_host(ssl, name) != 1) {
            return ssl_fail(ctx, ssl, ssl_ctx, "cannot verify the server name");
        }
    }
    SSL_set_verify(ssl, opts->tls_insecure_skip_verify ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, NULL);

    SSL_set_fd(ssl, (int)ctx->fd);

    rc = SSL_connect(ssl);
    if (rc != 1) {
        return ssl_fail(ctx, ssl, ssl_ctx, "SSL handshake failed");
    }

    ctx->ssl = ssl;
    ctx->ssl_ctx = ssl_ctx;
    return HOTKV_OK;
}

#endif /* HOTKV_USE_SSL */
