/**
 * @file hotkv_resp.c
 * @brief RESP2/RESP3 protocol parser - state machine implementation.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 *
 * This is a hand-written incremental parser that handles partial reads
 * correctly. It supports all RESP2 types (+, -, :, $, *) and all RESP3
 * types (_, #, ,, (, =, ~, %, >, |). The parser has no dependencies on
 * the connection layer and can be unit tested independently.
 */

#include "hotkv_private.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Allocator indirection
 * ========================================================================= */

static hotkv_malloc_fn  s_malloc  = malloc;
static hotkv_calloc_fn  s_calloc  = calloc;
static hotkv_realloc_fn s_realloc = realloc;
static hotkv_free_fn    s_free    = free;

void hotkv_set_allocator(hotkv_malloc_fn malloc_fn,
                          hotkv_calloc_fn calloc_fn,
                          hotkv_realloc_fn realloc_fn,
                          hotkv_free_fn free_fn) {
    s_malloc  = malloc_fn  ? malloc_fn  : malloc;
    s_calloc  = calloc_fn  ? calloc_fn  : calloc;
    s_realloc = realloc_fn ? realloc_fn : realloc;
    s_free    = free_fn    ? free_fn    : free;
}

void *hotkv_malloc(size_t size) { return s_malloc(size); }
void *hotkv_calloc(size_t count, size_t size) { return s_calloc(count, size); }
void *hotkv_realloc(void *ptr, size_t size) { return s_realloc(ptr, size); }
void  hotkv_free(void *ptr) { s_free(ptr); }

char *hotkv_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dup = (char *)hotkv_malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len + 1);
    }
    return dup;
}

/* =========================================================================
 * Dynamic buffer
 * ========================================================================= */

int hotkv_buf_init(hotkv_buf_t *buf, size_t initial_cap) {
    if (initial_cap == 0) initial_cap = HOTKV_BUF_INITIAL_SIZE;
    buf->data = (char *)hotkv_malloc(initial_cap);
    if (!buf->data) return HOTKV_ERR_OOM;
    buf->len = 0;
    buf->cap = initial_cap;
    buf->pos = 0;
    return HOTKV_OK;
}

void hotkv_buf_destroy(hotkv_buf_t *buf) {
    hotkv_free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->pos = 0;
}

int hotkv_buf_ensure(hotkv_buf_t *buf, size_t additional) {
    size_t needed = buf->len + additional;
    if (needed <= buf->cap) return HOTKV_OK;

    /* Grow by at least 2x to amortise reallocations. */
    size_t newcap = buf->cap * 2;
    if (newcap < needed) newcap = needed;

    char *tmp = (char *)hotkv_realloc(buf->data, newcap);
    if (!tmp) return HOTKV_ERR_OOM;
    buf->data = tmp;
    buf->cap = newcap;
    return HOTKV_OK;
}

int hotkv_buf_append(hotkv_buf_t *buf, const char *data, size_t len) {
    int rc = hotkv_buf_ensure(buf, len);
    if (rc != HOTKV_OK) return rc;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return HOTKV_OK;
}

void hotkv_buf_consume(hotkv_buf_t *buf, size_t n) {
    buf->pos += n;
}

void hotkv_buf_compact(hotkv_buf_t *buf) {
    if (buf->pos == 0) return;
    if (buf->pos >= buf->len) {
        buf->len = 0;
        buf->pos = 0;
    } else {
        size_t remaining = buf->len - buf->pos;
        memmove(buf->data, buf->data + buf->pos, remaining);
        buf->len = remaining;
        buf->pos = 0;
    }
}

/* =========================================================================
 * Reply allocation / deallocation
 * ========================================================================= */

hotkv_reply_t *hotkv_reply_alloc(void) {
    hotkv_reply_t *r = (hotkv_reply_t *)hotkv_calloc(1, sizeof(hotkv_reply_t));
    return r;
}

hotkv_reply_t *hotkv_local_error_reply(const char *msg) {
    hotkv_reply_t *r = hotkv_reply_alloc();
    if (!r) return NULL;
    r->type = HOTKV_REPLY_ERROR;
    r->str = hotkv_strdup(msg);
    if (!r->str) {
        hotkv_free(r);
        return NULL;
    }
    r->len = strlen(msg);
    return r;
}

void hotkv_free_reply(hotkv_reply_t *reply) {
    if (!reply) return;

    /* Free child elements first. */
    if (reply->elements) {
        for (size_t i = 0; i < reply->nelem; i++) {
            hotkv_free_reply(reply->elements[i]);
        }
        hotkv_free(reply->elements);
    }

    /* Free string payload. */
    hotkv_free(reply->str);

    hotkv_free(reply);
}

/* =========================================================================
 * Paired reply helpers
 *
 * Commands like ZRANGE ... WITHSCORES, HRANDFIELD ... WITHVALUES and
 * ZRANDMEMBER ... WITHSCORES reply with (member, value) pairs, but the wire
 * shape of that reply is not the same under both protocols: RESP2 (and a
 * RESP3 connection for a command the server has no native paired reply
 * for) sends one flat array `[m1, v1, m2, v2, ...]`, while RESP3 sends an
 * array of nelem 2-element arrays `[[m1, v1], [m2, v2], ...]`. HGETALL and
 * CONFIG GET do not have this problem: HOTKV_REPLY_MAP is always stored
 * flattened to 2*N elements by parse_aggregate() above, matching RESP2.
 * These two helpers hide the WITHSCORES/WITHVALUES difference so callers
 * can iterate a paired reply the same way regardless of protocol.
 * ========================================================================= */

static int reply_is_nested_pairs(const hotkv_reply_t *reply) {
    return reply->nelem > 0 && reply->elements[0] != NULL &&
           reply->elements[0]->type == HOTKV_REPLY_ARRAY &&
           reply->elements[0]->nelem == 2;
}

size_t hotkv_reply_pair_count(const hotkv_reply_t *reply) {
    if (!reply) return 0;
    if (reply->type != HOTKV_REPLY_ARRAY && reply->type != HOTKV_REPLY_SET &&
        reply->type != HOTKV_REPLY_MAP) {
        return 0;
    }
    if (reply_is_nested_pairs(reply)) return reply->nelem;
    return reply->nelem / 2;
}

int hotkv_reply_pair_at(const hotkv_reply_t *reply, size_t index,
                         hotkv_reply_t **out_key, hotkv_reply_t **out_value) {
    if (!reply || !out_key || !out_value) return 0;
    if (index >= hotkv_reply_pair_count(reply)) return 0;

    if (reply_is_nested_pairs(reply)) {
        hotkv_reply_t *pair = reply->elements[index];
        *out_key = pair->elements[0];
        *out_value = pair->elements[1];
        return 1;
    }

    *out_key = reply->elements[index * 2];
    *out_value = reply->elements[index * 2 + 1];
    return 1;
}

/* =========================================================================
 * Reader error helpers
 * ========================================================================= */

void hotkv_reader_set_error(hotkv_reader_t *reader, int type, const char *fmt, ...) {
    reader->err = type;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reader->errstr, HOTKV_ERRSTR_LEN, fmt, ap);
    va_end(ap);
}

/* =========================================================================
 * Reader lifecycle
 * ========================================================================= */

hotkv_reader_t *hotkv_reader_new(void) {
    return hotkv_reader_new_with_size(HOTKV_BUF_INITIAL_SIZE);
}

hotkv_reader_t *hotkv_reader_new_with_size(size_t bufsz) {
    hotkv_reader_t *r = (hotkv_reader_t *)hotkv_calloc(1, sizeof(hotkv_reader_t));
    if (!r) return NULL;

    if (hotkv_buf_init(&r->buf, bufsz) != HOTKV_OK) {
        hotkv_free(r);
        return NULL;
    }

    r->protocol = HOTKV_RESP2;
    return r;
}

void hotkv_reader_free(hotkv_reader_t *reader) {
    if (!reader) return;
    hotkv_buf_destroy(&reader->buf);
    hotkv_free(reader);
}

int hotkv_reader_feed(hotkv_reader_t *reader, const char *buf, size_t len) {
    if (!reader || !buf) return HOTKV_ERR_OTHER;

    /* Compact dead prefix before appending to keep the buffer manageable. */
    if (reader->buf.pos > 4096) {
        hotkv_buf_compact(&reader->buf);
    }

    return hotkv_buf_append(&reader->buf, buf, len);
}

void hotkv_reader_reset(hotkv_reader_t *reader) {
    if (!reader) return;
    reader->buf.len = 0;
    reader->buf.pos = 0;
    reader->err = HOTKV_OK;
    reader->errstr[0] = '\0';
}

size_t hotkv_reader_buffered(const hotkv_reader_t *reader) {
    if (!reader) return 0;
    return hotkv_buf_readable(&reader->buf);
}

/* =========================================================================
 * Parser internals
 * ========================================================================= */

/**
 * Find \r\n starting from the read cursor. Returns offset relative to
 * buf->data, or SIZE_MAX if not found.
 */
static size_t find_crlf(const hotkv_buf_t *buf, size_t from) {
    const char *base = buf->data;
    size_t end = buf->len;

    for (size_t i = from; i + 1 < end; i++) {
        if (base[i] == '\r' && base[i + 1] == '\n') {
            return i;
        }
    }
    return SIZE_MAX;
}

/**
 * Parse a signed 64-bit integer from buf[start..end) without stdlib.
 * Returns 0 on success, -1 on error.
 */
static int parse_int64(const char *buf, size_t start, size_t end, int64_t *out) {
    if (start >= end) return -1;

    int negative = 0;
    size_t i = start;

    if (buf[i] == '-') {
        negative = 1;
        i++;
    } else if (buf[i] == '+') {
        i++;
    }

    if (i >= end) return -1;

    int64_t val = 0;
    for (; i < end; i++) {
        if (buf[i] < '0' || buf[i] > '9') return -1;
        int digit = buf[i] - '0';
        /* Overflow check. */
        if (val > (INT64_MAX - digit) / 10) return -1;
        val = val * 10 + digit;
    }

    *out = negative ? -val : val;
    return 0;
}

/**
 * Parse a double from a string range. Uses strtod for correctness with
 * special values (inf, -inf, nan).
 */
static int parse_double(const char *buf, size_t start, size_t end, double *out) {
    if (start >= end) return -1;
    size_t slen = end - start;

    /* Copy to a NUL-terminated buffer on the stack. */
    char tmp[64];
    if (slen >= sizeof(tmp)) return -1;
    memcpy(tmp, buf + start, slen);
    tmp[slen] = '\0';

    /* Handle RESP3 special values. */
    if (strcmp(tmp, "inf") == 0) { *out = HUGE_VAL; return 0; }
    if (strcmp(tmp, "-inf") == 0) { *out = -HUGE_VAL; return 0; }
    if (strcmp(tmp, "nan") == 0) { *out = NAN; return 0; }

    char *endptr = NULL;
    errno = 0;
    *out = strtod(tmp, &endptr);
    if (errno == ERANGE || endptr != tmp + slen) return -1;
    return 0;
}

/* Forward declaration for recursion. */
static int parse_resp(hotkv_reader_t *reader, hotkv_reply_t **out, size_t depth);

/**
 * Parse a simple line type: simple string (+), error (-), integer (:).
 */
static int parse_line(hotkv_reader_t *reader, hotkv_reply_t **out, int type) {
    const hotkv_buf_t *buf = &reader->buf;
    size_t pos = buf->pos;
    size_t crlf = find_crlf(buf, pos + 1);
    if (crlf == SIZE_MAX) {
        *out = NULL;
        return HOTKV_OK; /* incomplete */
    }

    hotkv_reply_t *reply = hotkv_reply_alloc();
    if (!reply) return HOTKV_ERR_OOM;

    size_t data_start = pos + 1; /* skip type prefix */
    size_t data_len = crlf - data_start;

    if (type == HOTKV_REPLY_INTEGER) {
        reply->type = HOTKV_REPLY_INTEGER;
        if (parse_int64(buf->data, data_start, crlf, &reply->integer) != 0) {
            hotkv_free_reply(reply);
            hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                    "invalid integer value");
            return HOTKV_ERR_PROTOCOL;
        }
    } else {
        reply->type = type;
        reply->str = (char *)hotkv_malloc(data_len + 1);
        if (!reply->str) {
            hotkv_free_reply(reply);
            return HOTKV_ERR_OOM;
        }
        memcpy(reply->str, buf->data + data_start, data_len);
        reply->str[data_len] = '\0';
        reply->len = data_len;
    }

    reader->buf.pos = crlf + 2; /* consume past \r\n */
    *out = reply;
    return HOTKV_OK;
}

/**
 * Parse a RESP2 bulk string ($).
 */
static int parse_bulk(hotkv_reader_t *reader, hotkv_reply_t **out) {
    const hotkv_buf_t *buf = &reader->buf;
    size_t pos = buf->pos;
    size_t crlf = find_crlf(buf, pos + 1);
    if (crlf == SIZE_MAX) {
        *out = NULL;
        return HOTKV_OK; /* incomplete */
    }

    int64_t length = 0;
    if (parse_int64(buf->data, pos + 1, crlf, &length) != 0) {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "invalid bulk string length");
        return HOTKV_ERR_PROTOCOL;
    }

    /* Null bulk string: $-1\r\n */
    if (length == -1) {
        hotkv_reply_t *reply = hotkv_reply_alloc();
        if (!reply) return HOTKV_ERR_OOM;
        reply->type = HOTKV_REPLY_NIL;
        reader->buf.pos = crlf + 2;
        *out = reply;
        return HOTKV_OK;
    }

    if (length < 0 || length > HOTKV_MAX_BULK_LEN) {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "bulk string length out of range");
        return HOTKV_ERR_PROTOCOL;
    }

    size_t blen = (size_t)length;
    size_t data_start = crlf + 2;
    size_t data_end = data_start + blen;

    /* Need data + \r\n */
    if (data_end + 2 > buf->len) {
        *out = NULL;
        return HOTKV_OK; /* incomplete */
    }

    /* Validate trailing \r\n. */
    if (buf->data[data_end] != '\r' || buf->data[data_end + 1] != '\n') {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "missing CRLF after bulk string data");
        return HOTKV_ERR_PROTOCOL;
    }

    hotkv_reply_t *reply = hotkv_reply_alloc();
    if (!reply) return HOTKV_ERR_OOM;

    reply->type = HOTKV_REPLY_BULK;
    reply->str = (char *)hotkv_malloc(blen + 1);
    if (!reply->str) {
        hotkv_free_reply(reply);
        return HOTKV_ERR_OOM;
    }
    memcpy(reply->str, buf->data + data_start, blen);
    reply->str[blen] = '\0';
    reply->len = blen;

    reader->buf.pos = data_end + 2;
    *out = reply;
    return HOTKV_OK;
}

/**
 * Parse an aggregate type (array, map, set, push, attribute).
 * Arrays (*) store N elements.
 * Maps (%) and Attributes (|) store 2*N elements (key-value pairs).
 * Sets (~) and Push (>) store N elements.
 */
static int parse_aggregate(hotkv_reader_t *reader, hotkv_reply_t **out,
                            int type, int pairs, size_t depth) {
    const hotkv_buf_t *buf = &reader->buf;
    size_t pos = buf->pos;
    size_t crlf = find_crlf(buf, pos + 1);
    if (crlf == SIZE_MAX) {
        *out = NULL;
        return HOTKV_OK; /* incomplete */
    }

    int64_t count = 0;
    if (parse_int64(buf->data, pos + 1, crlf, &count) != 0) {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "invalid aggregate element count");
        return HOTKV_ERR_PROTOCOL;
    }

    /* Null array: *-1\r\n */
    if (count == -1 && type == HOTKV_REPLY_ARRAY) {
        hotkv_reply_t *reply = hotkv_reply_alloc();
        if (!reply) return HOTKV_ERR_OOM;
        reply->type = HOTKV_REPLY_NIL;
        reader->buf.pos = crlf + 2;
        *out = reply;
        return HOTKV_OK;
    }

    if (count < 0 || count > HOTKV_MAX_ELEMENTS) {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "aggregate element count out of range");
        return HOTKV_ERR_PROTOCOL;
    }

    /* For maps and attributes, we store 2*count elements (key-value pairs). */
    size_t total = (size_t)count;
    if (pairs) total *= 2;

    reader->buf.pos = crlf + 2; /* consume the header line */

    hotkv_reply_t *reply = hotkv_reply_alloc();
    if (!reply) return HOTKV_ERR_OOM;
    reply->type = type;

    if (total > 0) {
        reply->elements = (hotkv_reply_t **)hotkv_calloc(total, sizeof(hotkv_reply_t *));
        if (!reply->elements) {
            hotkv_free_reply(reply);
            return HOTKV_ERR_OOM;
        }
    }
    reply->nelem = total;

    for (size_t i = 0; i < total; i++) {
        int rc = parse_resp(reader, &reply->elements[i], depth + 1);
        if (rc != HOTKV_OK) {
            /* Error: free what we've parsed so far. */
            reply->nelem = i; /* only free successfully parsed children */
            hotkv_free_reply(reply);
            return rc;
        }
        if (reply->elements[i] == NULL) {
            /* Incomplete: we need to roll back the entire aggregate.
             * Since we've partially consumed, store partial state.
             * For simplicity we rewind. The caller will retry. */
            reply->nelem = i;
            hotkv_free_reply(reply);
            reader->buf.pos = pos; /* rewind to before this aggregate */
            *out = NULL;
            return HOTKV_OK;
        }
    }

    *out = reply;
    return HOTKV_OK;
}

/**
 * Parse a RESP3 null type (_\r\n).
 */
static int parse_null(hotkv_reader_t *reader, hotkv_reply_t **out) {
    const hotkv_buf_t *buf = &reader->buf;
    size_t pos = buf->pos;
    size_t crlf = find_crlf(buf, pos + 1);
    if (crlf == SIZE_MAX) {
        *out = NULL;
        return HOTKV_OK;
    }

    hotkv_reply_t *reply = hotkv_reply_alloc();
    if (!reply) return HOTKV_ERR_OOM;
    reply->type = HOTKV_REPLY_NIL;
    reader->buf.pos = crlf + 2;
    *out = reply;
    return HOTKV_OK;
}

/**
 * Parse a RESP3 boolean (#t\r\n or #f\r\n).
 */
static int parse_bool(hotkv_reader_t *reader, hotkv_reply_t **out) {
    const hotkv_buf_t *buf = &reader->buf;
    size_t pos = buf->pos;
    size_t crlf = find_crlf(buf, pos + 1);
    if (crlf == SIZE_MAX) {
        *out = NULL;
        return HOTKV_OK;
    }

    if (crlf - pos - 1 != 1) {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "invalid boolean value");
        return HOTKV_ERR_PROTOCOL;
    }

    char c = buf->data[pos + 1];
    if (c != 't' && c != 'f') {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "invalid boolean value '%c'", c);
        return HOTKV_ERR_PROTOCOL;
    }

    hotkv_reply_t *reply = hotkv_reply_alloc();
    if (!reply) return HOTKV_ERR_OOM;
    reply->type = HOTKV_REPLY_BOOL;
    reply->integer = (c == 't') ? 1 : 0;
    reader->buf.pos = crlf + 2;
    *out = reply;
    return HOTKV_OK;
}

/**
 * Parse a RESP3 double (,value\r\n).
 */
static int parse_resp_double(hotkv_reader_t *reader, hotkv_reply_t **out) {
    const hotkv_buf_t *buf = &reader->buf;
    size_t pos = buf->pos;
    size_t crlf = find_crlf(buf, pos + 1);
    if (crlf == SIZE_MAX) {
        *out = NULL;
        return HOTKV_OK;
    }

    double val = 0.0;
    if (parse_double(buf->data, pos + 1, crlf, &val) != 0) {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "invalid double value");
        return HOTKV_ERR_PROTOCOL;
    }

    hotkv_reply_t *reply = hotkv_reply_alloc();
    if (!reply) return HOTKV_ERR_OOM;
    reply->type = HOTKV_REPLY_DOUBLE;
    reply->dval = val;
    reader->buf.pos = crlf + 2;
    *out = reply;
    return HOTKV_OK;
}

/**
 * Parse a RESP3 big number ((value\r\n). Stored as a string.
 */
static int parse_bignum(hotkv_reader_t *reader, hotkv_reply_t **out) {
    const hotkv_buf_t *buf = &reader->buf;
    size_t pos = buf->pos;
    size_t crlf = find_crlf(buf, pos + 1);
    if (crlf == SIZE_MAX) {
        *out = NULL;
        return HOTKV_OK;
    }

    size_t data_start = pos + 1;
    size_t data_len = crlf - data_start;

    hotkv_reply_t *reply = hotkv_reply_alloc();
    if (!reply) return HOTKV_ERR_OOM;
    reply->type = HOTKV_REPLY_BIGNUM;
    reply->str = (char *)hotkv_malloc(data_len + 1);
    if (!reply->str) {
        hotkv_free_reply(reply);
        return HOTKV_ERR_OOM;
    }
    memcpy(reply->str, buf->data + data_start, data_len);
    reply->str[data_len] = '\0';
    reply->len = data_len;

    reader->buf.pos = crlf + 2;
    *out = reply;
    return HOTKV_OK;
}

/**
 * Parse a RESP3 verbatim string (=N\r\nenc:data\r\n).
 * Same wire format as bulk string but with a 3-byte encoding prefix.
 */
static int parse_verbatim(hotkv_reader_t *reader, hotkv_reply_t **out) {
    const hotkv_buf_t *buf = &reader->buf;
    size_t pos = buf->pos;
    size_t crlf = find_crlf(buf, pos + 1);
    if (crlf == SIZE_MAX) {
        *out = NULL;
        return HOTKV_OK;
    }

    int64_t length = 0;
    if (parse_int64(buf->data, pos + 1, crlf, &length) != 0 || length < 4) {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "invalid verbatim string length");
        return HOTKV_ERR_PROTOCOL;
    }

    if (length > HOTKV_MAX_BULK_LEN) {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "verbatim string length out of range");
        return HOTKV_ERR_PROTOCOL;
    }

    size_t blen = (size_t)length;
    size_t data_start = crlf + 2;
    size_t data_end = data_start + blen;

    if (data_end + 2 > buf->len) {
        *out = NULL;
        return HOTKV_OK; /* incomplete */
    }

    if (buf->data[data_end] != '\r' || buf->data[data_end + 1] != '\n') {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "missing CRLF after verbatim string data");
        return HOTKV_ERR_PROTOCOL;
    }

    /* Format: "enc:data" where enc is 3 bytes. */
    if (buf->data[data_start + 3] != ':') {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "invalid verbatim string encoding prefix");
        return HOTKV_ERR_PROTOCOL;
    }

    hotkv_reply_t *reply = hotkv_reply_alloc();
    if (!reply) return HOTKV_ERR_OOM;
    reply->type = HOTKV_REPLY_VERB;

    /* Copy the 3-byte type prefix. */
    memcpy(reply->vtype, buf->data + data_start, 3);
    reply->vtype[3] = '\0';

    /* Copy the actual data (after "enc:"). */
    size_t str_len = blen - 4;
    reply->str = (char *)hotkv_malloc(str_len + 1);
    if (!reply->str) {
        hotkv_free_reply(reply);
        return HOTKV_ERR_OOM;
    }
    memcpy(reply->str, buf->data + data_start + 4, str_len);
    reply->str[str_len] = '\0';
    reply->len = str_len;

    reader->buf.pos = data_end + 2;
    *out = reply;
    return HOTKV_OK;
}

/**
 * Main recursive parser dispatch.
 */
static int parse_resp(hotkv_reader_t *reader, hotkv_reply_t **out, size_t depth) {
    if (depth > HOTKV_MAX_DEPTH) {
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "maximum nesting depth exceeded");
        return HOTKV_ERR_PROTOCOL;
    }

    const hotkv_buf_t *buf = &reader->buf;
    if (buf->pos >= buf->len) {
        *out = NULL;
        return HOTKV_OK; /* incomplete */
    }

    char prefix = buf->data[buf->pos];

    switch (prefix) {
    /* RESP2 types */
    case '+': return parse_line(reader, out, HOTKV_REPLY_STRING);
    case '-': return parse_line(reader, out, HOTKV_REPLY_ERROR);
    case ':': return parse_line(reader, out, HOTKV_REPLY_INTEGER);
    case '$': return parse_bulk(reader, out);
    case '*': return parse_aggregate(reader, out, HOTKV_REPLY_ARRAY, 0, depth);

    /* RESP3 types */
    case '_': return parse_null(reader, out);
    case '#': return parse_bool(reader, out);
    case ',': return parse_resp_double(reader, out);
    case '(': return parse_bignum(reader, out);
    case '=': return parse_verbatim(reader, out);
    case '~': return parse_aggregate(reader, out, HOTKV_REPLY_SET, 0, depth);
    case '%': return parse_aggregate(reader, out, HOTKV_REPLY_MAP, 1, depth);
    case '>': return parse_aggregate(reader, out, HOTKV_REPLY_PUSH, 0, depth);
    case '|': return parse_aggregate(reader, out, HOTKV_REPLY_ATTR, 1, depth);

    default:
        hotkv_reader_set_error(reader, HOTKV_ERR_PROTOCOL,
                                "unknown RESP type prefix '%c' (0x%02x)",
                                isprint((unsigned char)prefix) ? prefix : '?',
                                (unsigned char)prefix);
        return HOTKV_ERR_PROTOCOL;
    }
}

/* =========================================================================
 * Public reader API
 * ========================================================================= */

int hotkv_reader_next(hotkv_reader_t *reader, hotkv_reply_t **reply) {
    if (!reader || !reply) return HOTKV_ERR_OTHER;

    /* If there's a previous error, don't try to parse. */
    if (reader->err != HOTKV_OK) {
        *reply = NULL;
        return reader->err;
    }

    *reply = NULL;

    if (hotkv_buf_readable(&reader->buf) == 0) {
        return HOTKV_OK; /* nothing to parse */
    }

    size_t saved_pos = reader->buf.pos;
    int rc = parse_resp(reader, reply, 0);

    if (rc != HOTKV_OK) {
        /* On error, restore position. */
        reader->buf.pos = saved_pos;
        return rc;
    }

    if (*reply == NULL) {
        /* Incomplete - position was already restored by the parser. */
        reader->buf.pos = saved_pos;
        return HOTKV_OK;
    }

    /* Compact the buffer periodically to avoid unbounded growth. */
    if (reader->buf.pos > 4096) {
        hotkv_buf_compact(&reader->buf);
    }

    return HOTKV_OK;
}

/* =========================================================================
 * RESP command formatter
 * ========================================================================= */

/**
 * Append a single RESP bulk string to the buffer: $N\r\n<data>\r\n
 */
static int append_bulk_arg(hotkv_buf_t *obuf, const char *arg, size_t arglen) {
    char hdr[32];
    int hdrlen = snprintf(hdr, sizeof(hdr), "$%zu\r\n", arglen);
    if (hdrlen < 0) return HOTKV_ERR_OTHER;

    int rc = hotkv_buf_append(obuf, hdr, (size_t)hdrlen);
    if (rc != HOTKV_OK) return rc;

    rc = hotkv_buf_append(obuf, arg, arglen);
    if (rc != HOTKV_OK) return rc;

    return hotkv_buf_append(obuf, "\r\n", 2);
}

int hotkv_format_command_argv(hotkv_buf_t *obuf, int argc,
                               const char **argv, const size_t *argvlen) {
    if (argc <= 0 || !argv) return HOTKV_ERR_OTHER;

    /* Leave obuf untouched on failure: a half-written command would corrupt
     * the stream for every later command on the connection. */
    size_t start_len = obuf->len;

    /* Array header: *N\r\n */
    char hdr[32];
    int hdrlen = snprintf(hdr, sizeof(hdr), "*%d\r\n", argc);
    if (hdrlen < 0) return HOTKV_ERR_OTHER;

    int rc = hotkv_buf_append(obuf, hdr, (size_t)hdrlen);
    for (int i = 0; rc == HOTKV_OK && i < argc; i++) {
        if (!argv[i]) { rc = HOTKV_ERR_OTHER; break; }
        size_t len = argvlen ? argvlen[i] : strlen(argv[i]);
        rc = append_bulk_arg(obuf, argv[i], len);
    }

    if (rc != HOTKV_OK) obuf->len = start_len;
    return rc;
}

/*
 * printf-style command formatting.
 *
 * The format is split into words on ASCII whitespace; each word becomes one
 * argument. Inside a word, literal text and conversions concatenate, so
 * "user:%s:name" is a single argument. Conversions: %s (NUL-terminated
 * string), %b (const char *, size_t), %d (int), %lld (long long) and %% (a
 * literal percent sign). An unknown conversion, a trailing '%', a NULL %s/%b
 * pointer or a format without any word is rejected with HOTKV_ERR_OTHER and
 * nothing is appended to obuf.
 *
 * Arguments are serialised straight into a scratch buffer, so there is no
 * per-argument allocation to track and no limit on the argument count.
 */
static int fmt_flush_word(hotkv_buf_t *body, hotkv_buf_t *word, int *in_word, int *argc) {
    if (!*in_word) return HOTKV_OK;
    int rc = append_bulk_arg(body, word->data, word->len);
    word->len = 0;
    *in_word = 0;
    (*argc)++;
    return rc;
}

int hotkv_format_command(hotkv_buf_t *obuf, const char *format, va_list ap) {
    if (!format) return HOTKV_ERR_OTHER;

    hotkv_buf_t body, word;
    if (hotkv_buf_init(&body, 256) != HOTKV_OK) return HOTKV_ERR_OOM;
    if (hotkv_buf_init(&word, 64) != HOTKV_OK) {
        hotkv_buf_destroy(&body);
        return HOTKV_ERR_OOM;
    }

    int rc = HOTKV_OK;
    int argc = 0;
    int in_word = 0;
    const char *p = format;

    while (*p && rc == HOTKV_OK) {
        unsigned char ch = (unsigned char)*p;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            rc = fmt_flush_word(&body, &word, &in_word, &argc);
            p++;
            continue;
        }
        if (ch != '%') {
            rc = hotkv_buf_append(&word, p, 1);
            in_word = 1;
            p++;
            continue;
        }

        p++; /* skip '%' */
        in_word = 1;
        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) { rc = HOTKV_ERR_OTHER; break; }
            rc = hotkv_buf_append(&word, s, strlen(s));
            p++;
        } else if (*p == 'b') {
            const char *s = va_arg(ap, const char *);
            size_t l = va_arg(ap, size_t);
            if (!s && l) { rc = HOTKV_ERR_OTHER; break; }
            if (l) rc = hotkv_buf_append(&word, s, l);
            p++;
        } else if (*p == 'd') {
            char tmp[24];
            int n = snprintf(tmp, sizeof(tmp), "%d", va_arg(ap, int));
            rc = hotkv_buf_append(&word, tmp, (size_t)n);
            p++;
        } else if (p[0] == 'l' && p[1] == 'l' && p[2] == 'd') {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%lld", va_arg(ap, long long));
            rc = hotkv_buf_append(&word, tmp, (size_t)n);
            p += 3;
        } else if (*p == '%') {
            rc = hotkv_buf_append(&word, "%", 1);
            p++;
        } else {
            rc = HOTKV_ERR_OTHER; /* unknown conversion or trailing '%' */
        }
    }

    if (rc == HOTKV_OK) rc = fmt_flush_word(&body, &word, &in_word, &argc);
    if (rc == HOTKV_OK && argc == 0) rc = HOTKV_ERR_OTHER;

    if (rc == HOTKV_OK) {
        size_t start_len = obuf->len;
        char hdr[32];
        int hdrlen = snprintf(hdr, sizeof(hdr), "*%d\r\n", argc);
        rc = hotkv_buf_append(obuf, hdr, (size_t)hdrlen);
        if (rc == HOTKV_OK) rc = hotkv_buf_append(obuf, body.data, body.len);
        if (rc != HOTKV_OK) obuf->len = start_len;
    }

    hotkv_buf_destroy(&word);
    hotkv_buf_destroy(&body);
    return rc;
}
