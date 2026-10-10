/**
 * @file hotkv_url.c
 * @brief Connection URL parsing (hotkv://, hotkvs://, redis://, rediss://).
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "hotkv.h"
#include "hotkv_private.h"

#include <stdlib.h>

/* strncmp() alone only proves the first `n` bytes match; it does not prove
 * the candidate scheme is exactly `n` bytes long, so "hotkv" would
 * wrongly "match" the first 5 bytes of "hotkvs" too. Check both. */
static int hkv_scheme_is(const char *url, size_t scheme_len, const char *candidate) {
    return scheme_len == strlen(candidate) && strncmp(url, candidate, scheme_len) == 0;
}

static char *dupn(const char *s, size_t n) {
    char *r = (char *)hotkv_malloc(n + 1);
    if (!r) {
        return NULL;
    }
    if (n > 0) {
        memcpy(r, s, n);
    }
    r[n] = '\0';
    return r;
}

/* Percent-decode in place semantics: writes into a freshly allocated buffer
 * of at most the input length. Unknown/invalid escapes are copied verbatim. */
static char *url_unescape(const char *s, size_t n) {
    char *out = (char *)hotkv_malloc(n + 1);
    size_t o = 0;
    size_t i;
    if (!out) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n + 1 && i + 2 <= n) {
            char hi = s[i + 1];
            char lo = s[i + 2];
            int hv = (hi >= '0' && hi <= '9') ? hi - '0'
                     : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                     : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                                                 : -1;
            int lv = (lo >= '0' && lo <= '9') ? lo - '0'
                     : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                     : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                                                 : -1;
            if (hv >= 0 && lv >= 0) {
                out[o++] = (char)((hv << 4) | lv);
                i += 2;
                continue;
            }
        }
        out[o++] = s[i];
    }
    out[o] = '\0';
    return out;
}

void hotkv_url_free(hotkv_url_t *out) {
    if (!out) {
        return;
    }
    hotkv_free(out->host);
    hotkv_free(out->username);
    hotkv_free(out->password);
    out->host = NULL;
    out->username = NULL;
    out->password = NULL;
}

int hotkv_url_parse(const char *url, hotkv_url_t *out) {
    const char *p;
    const char *scheme_end;
    const char *authority;
    const char *authority_end;
    const char *at;
    const char *host_start;
    const char *host_end;
    const char *path;
    size_t urllen;

    if (!url || !out) {
        return HOTKV_ERR_OTHER;
    }
    memset(out, 0, sizeof(*out));
    out->port = 6379;
    out->db = 0;

    urllen = strlen(url);
    scheme_end = strstr(url, "://");
    if (!scheme_end) {
        return HOTKV_ERR_PROTOCOL;
    }

    {
        size_t slen = (size_t)(scheme_end - url);
        if (hkv_scheme_is(url, slen, "hotkvs") || hkv_scheme_is(url, slen, "rediss")) {
            out->use_tls = 1;
        } else if (hkv_scheme_is(url, slen, "hotkv") || hkv_scheme_is(url, slen, "redis")) {
            out->use_tls = 0;
        } else {
            return HOTKV_ERR_PROTOCOL;
        }
    }

    authority = scheme_end + 3;
    path = strchr(authority, '/');
    authority_end = path ? path : (url + urllen);

    /* [username[:password]@]host[:port] */
    at = NULL;
    for (p = authority; p < authority_end; p++) {
        if (*p == '@') {
            at = p;
        }
    }
    host_start = authority;
    if (at) {
        const char *colon = memchr(authority, ':', (size_t)(at - authority));
        if (colon && colon > authority) {
            /* user:pass@host */
            char *rawuser = dupn(authority, (size_t)(colon - authority));
            char *rawpass = dupn(colon + 1, (size_t)(at - colon - 1));
            if (!rawuser || !rawpass) {
                hotkv_free(rawuser);
                hotkv_free(rawpass);
                return HOTKV_ERR_OOM;
            }
            out->username = url_unescape(rawuser, strlen(rawuser));
            out->password = url_unescape(rawpass, strlen(rawpass));
            hotkv_free(rawuser);
            hotkv_free(rawpass);
        } else {
            /* "pass@host" (legacy single-password form) or ":pass@host"
             * (explicit empty username): both mean "no username". */
            const char *pass_start = colon ? colon + 1 : authority;
            char *rawpass = dupn(pass_start, (size_t)(at - pass_start));
            if (!rawpass) {
                return HOTKV_ERR_OOM;
            }
            out->password = url_unescape(rawpass, strlen(rawpass));
            hotkv_free(rawpass);
        }
        host_start = at + 1;
    }

    host_end = memchr(host_start, ':', (size_t)(authority_end - host_start));
    if (host_end) {
        char portbuf[16];
        size_t portlen = (size_t)(authority_end - host_end - 1);
        if (portlen == 0 || portlen >= sizeof(portbuf)) {
            hotkv_url_free(out);
            return HOTKV_ERR_PROTOCOL;
        }
        memcpy(portbuf, host_end + 1, portlen);
        portbuf[portlen] = '\0';
        out->port = atoi(portbuf);
    } else {
        host_end = authority_end;
    }

    if (host_end == host_start) {
        out->host = hotkv_strdup("127.0.0.1");
    } else {
        out->host = dupn(host_start, (size_t)(host_end - host_start));
    }
    if (!out->host) {
        hotkv_url_free(out);
        return HOTKV_ERR_OOM;
    }

    if (path && path + 1 < url + urllen) {
        out->db = atoi(path + 1);
    }

    return HOTKV_OK;
}
