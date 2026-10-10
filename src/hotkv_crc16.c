/**
 * @file hotkv_crc16.c
 * @brief CRC16/XMODEM and cluster key-slot calculation (CRC16 mod 16384),
 * including `{hash tag}` support, matching CLUSTER KEYSLOT semantics.
 *
 * Copyright (c) 2026 HotKV Ltd
 * Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
 */

#include "hotkv.h"
#include "hotkv_private.h"

#define HOTKV_CLUSTER_SLOTS 16384

uint16_t hotkv_crc16(const char *buf, size_t len) {
    uint16_t crc = 0;
    size_t i;
    int j;

    for (i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ (uint16_t)((unsigned char)buf[i] << 8));
        for (j = 0; j < 8; j++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)(((unsigned)crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t hotkv_key_slot(const char *key, size_t len) {
    const char *brace_open;
    const char *brace_close;
    const char *hashed = key;
    size_t hashed_len = len;

    brace_open = (const char *)memchr(key, '{', len);
    if (brace_open) {
        size_t remaining = len - (size_t)(brace_open - key) - 1;
        brace_close = remaining ? (const char *)memchr(brace_open + 1, '}', remaining) : NULL;
        if (brace_close && brace_close > brace_open + 1) {
            hashed = brace_open + 1;
            hashed_len = (size_t)(brace_close - brace_open - 1);
        }
    }

    return (uint16_t)(hotkv_crc16(hashed, hashed_len) % HOTKV_CLUSTER_SLOTS);
}
