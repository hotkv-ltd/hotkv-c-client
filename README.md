# HotKV C Client (libhotkv)

[![CI](https://github.com/hotkv-ltd/hotkv-c-client/actions/workflows/ci.yml/badge.svg)](https://github.com/hotkv-ltd/hotkv-c-client/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)

A high-performance C client for [HotKV](https://hotkv.com), a RESP2/RESP3-compatible
in-memory data store with built-in AI and LLM-serving features (semantic and prompt
caching, agent memory, RAG, feature store, rate limiting, time series and more).

## Features

- **RESP2 + RESP3**: Full protocol support including all RESP3 types (maps, sets, doubles, booleans, big numbers, verbatim strings, attributes, push messages)
- **Zero dependencies**: Core library needs only a C11 compiler and a TCP stack
- **Cross-platform**: Linux, macOS, Windows (Winsock2)
- **Binary-safe**: All keys and values are length-prefixed, not NUL-terminated
- **Typed wrappers for the common commands, generic call for the rest**: 364 `hotkv_*`
  functions cover most of the core data-structure commands (208 of 301 core command
  names) and the enterprise/AI commands (144 of 177); every other command goes through
  `hotkv_command()` / `hotkv_command_argv()`. "Typed commands" below lists which is which.
- **Pipelining**: Batch multiple commands to reduce round-trips
- **Connection pooling**: Thread-safe pool with configurable min/max connections
- **TLS**: Optional OpenSSL support (compile with `-DHOTKV_USE_SSL=ON`)
- **Custom allocator**: Replace malloc/calloc/realloc/free globally
- **HotKV-exclusive commands**: NEXPIRE, NTTL, NEXPIREAT, TAG.\*, HOTKEYS, LATENCY, GCRA.\*, VSIM.\*
- **Enterprise and AI commands**: SCACHE.\* (semantic cache), PCACHE.\* (prompt cache),
  TOOL.CACHE.\*/NCACHE.\*/GUARD.\*/RERANK.\* (keyed caches), LLMSTATS.\*, LLMBUDGET.\*,
  PROMPT.\* (prompt registry), EMBED.DEDUP.\*, AMEM.\* (agent memory, incl. AMEM.SUMMARY.\*),
  RAG.\*, RATELIMIT.\*, TS.\* (time series), FEATURE.\* (feature store),
  OBSERVE / ALERTS
- **AI helpers**: `hotkv_promptcache_*` (deterministic cache-key builder plus a
  get-or-call wrapper around PCACHE.\*) and `hotkv_semanticcache_*` (a get-or-call
  wrapper around SCACHE.\* driven by an embedding vector you supply)
- **URL parsing and cluster key slots**: `hotkv_url_parse` (`hotkv://`, `hotkvs://`,
  `redis://`, `rediss://`) and `hotkv_key_slot` (CRC16 cluster hash slot, with
  `{hash tag}` support)

## Requirements

- C11 compiler (GCC 4.9+, Clang 3.5+, MSVC 2019+)
- CMake 3.16+
- Optional: OpenSSL 1.1+ (for TLS)

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### CMake options

| Option                 | Default | Description                                         |
| ---------------------- | ------- | --------------------------------------------------- |
| `HOTKV_USE_SSL`        | `OFF`   | Enable TLS via OpenSSL                              |
| `HOTKV_BUILD_TESTS`    | `ON`    | Build unit tests                                    |
| `HOTKV_BUILD_EXAMPLES` | `ON`    | Build example programs                              |
| `BUILD_SHARED_LIBS`    | `OFF`   | Build shared (.so/.dll) instead of static (.a/.lib) |

### Install

```bash
cmake --install . --prefix /usr/local
```

This installs:

- `lib/libhotkv.a`, or `libhotkv.so.1` plus the `libhotkv.so` link (`.dylib` on macOS,
  `bin/hotkv.dll` plus an import library on Windows) with `-DBUILD_SHARED_LIBS=ON`
- `include/hotkv.h`
- `lib/cmake/hotkv/` (`find_package(hotkv)` package, version `SameMajorVersion`)
- `lib/pkgconfig/libhotkv.pc` (relocatable: it locates the install tree from its own path)
- `share/doc/libhotkv/` (`LICENSE`, `README.md`)

Use it from CMake:

```cmake
find_package(hotkv 1 REQUIRED)
target_link_libraries(myapp PRIVATE hotkv::hotkv)
```

`examples/consumer/` is a complete project that does exactly this. A TLS build
(`-DHOTKV_USE_SSL=ON`) makes `find_package(hotkv)` look for OpenSSL itself.

The GitHub release archives (`libhotkv-<platform>-static` and `-shared`, `.tar.gz` for
Linux and macOS, `.zip` for Windows) are `cmake --install` trees. Linux and macOS
archives are built with TLS; the Windows archives are built without it (build from
source to get TLS on Windows).

## Quick start

```c
#include <hotkv.h>
#include <stdio.h>

int main(void) {
    hotkv_context_t *ctx = hotkv_connect("127.0.0.1", 6379);
    if (!ctx || hotkv_context_err(ctx)) {
        fprintf(stderr, "Error: %s\n", ctx ? hotkv_context_errstr(ctx) : "OOM");
        return 1;
    }

    /* SET */
    hotkv_reply_t *reply = hotkv_set(ctx, "key", 3, "value", 5);
    hotkv_free_reply(reply);

    /* GET */
    reply = hotkv_get(ctx, "key", 3);
    if (reply && reply->type == HOTKV_REPLY_BULK)
        printf("key = %s\n", reply->str);
    hotkv_free_reply(reply);

    hotkv_disconnect(ctx);
    return 0;
}
```

Compile:

```bash
cc -o myapp myapp.c -lhotkv
```

Or with pkg-config:

```bash
cc -o myapp myapp.c $(pkg-config --cflags --libs libhotkv)
```

## API overview

### Connection

```c
hotkv_context_t *hotkv_connect(const char *host, int port);
hotkv_context_t *hotkv_connect_with_options(const hotkv_options_t *opts);
void             hotkv_disconnect(hotkv_context_t *ctx);
```

### Generic commands

```c
hotkv_reply_t *hotkv_command(hotkv_context_t *ctx, const char *format, ...);
hotkv_reply_t *hotkv_command_argv(hotkv_context_t *ctx, int argc,
                                   const char **argv, const size_t *argvlen);
```

Format specifiers: `%s` (C string), `%b` (binary: `const char*, size_t`), `%d` (int),
`%lld` (long long), `%%` (literal %). The format is split into arguments on whitespace;
text and conversions inside one word are joined (`"GET user:%s"` is two arguments, the
second being `user:` followed by your string). An unknown conversion, a trailing `%` or
a NULL `%s` is rejected before anything is sent.

This is the way to run any command that has no typed wrapper, for example:

```c
hotkv_reply_t *r = hotkv_command(ctx, "BF.RESERVE %s 0.01 1000", "filter");
const char *argv[] = {"CLIENT", "TRACKING", "ON"};
r = hotkv_command_argv(ctx, 3, argv, NULL);
```

There is no printf conversion for floating point values on purpose: format a double
yourself or use a typed wrapper (see "Numbers on the wire").

### Pipelining

```c
int  hotkv_append_command(hotkv_context_t *ctx, const char *format, ...);
int  hotkv_get_reply(hotkv_context_t *ctx, hotkv_reply_t **reply);
```

### Typed commands

All typed helpers follow the same pattern: they take a context, keys and values with
explicit lengths (arrays are passed as parallel `const char **` / `const size_t *`
arrays plus a count), and return a `hotkv_reply_t*` (NULL on a transport error, see
`hotkv_context_err`). Ranges, score bounds, stream IDs and keywords are plain C strings.
The conventions for optional arguments are documented in the "Extended data-structure
commands" section of `hotkv.h`.

**Strings and bitmaps**: `hotkv_set`, `hotkv_get`, `hotkv_del`, `hotkv_mset`, `hotkv_mget`, `hotkv_incr`, `hotkv_decr`, `hotkv_incrby`, `hotkv_decrby`, `hotkv_incrbyfloat`, `hotkv_append`, `hotkv_strlen`, `hotkv_setnx`, `hotkv_setex`, `hotkv_psetex`, `hotkv_getdel`, `hotkv_getrange`, `hotkv_setrange`, `hotkv_getset`, `hotkv_getex`, `hotkv_msetnx`, `hotkv_lcs`, `hotkv_setbit`, `hotkv_getbit`, `hotkv_bitcount`, `hotkv_bitpos`, `hotkv_bitop`

**Lists**: `hotkv_lpush`, `hotkv_rpush`, `hotkv_lpushx`, `hotkv_rpushx`, `hotkv_lpop`, `hotkv_rpop`, `hotkv_lpop_count`, `hotkv_rpop_count`, `hotkv_llen`, `hotkv_lrange`, `hotkv_lindex`, `hotkv_lset`, `hotkv_lrem`, `hotkv_ltrim`, `hotkv_linsert`, `hotkv_lpos`, `hotkv_lmove`, `hotkv_rpoplpush`

**Sets**: `hotkv_sadd`, `hotkv_srem`, `hotkv_smembers`, `hotkv_sismember`, `hotkv_smismember`, `hotkv_scard`, `hotkv_sdiff`, `hotkv_sinter`, `hotkv_sunion`, `hotkv_sdiffstore`, `hotkv_sinterstore`, `hotkv_sunionstore`, `hotkv_sintercard`, `hotkv_smove`, `hotkv_spop`, `hotkv_spop_count`, `hotkv_srandmember`, `hotkv_srandmember_count`, `hotkv_sscan`

**Hashes**: `hotkv_hset`, `hotkv_hmset`, `hotkv_hsetnx`, `hotkv_hget`, `hotkv_hdel`, `hotkv_hgetdel`, `hotkv_hexists`, `hotkv_hlen`, `hotkv_hstrlen`, `hotkv_hkeys`, `hotkv_hvals`, `hotkv_hgetall`, `hotkv_hmget`, `hotkv_hincrby`, `hotkv_hincrbyfloat`, `hotkv_hrandfield`, `hotkv_hrandfield_count`, `hotkv_hscan`, and the field expiry family `hotkv_hexpire`, `hotkv_hpexpire`, `hotkv_httl`, `hotkv_hpttl`, `hotkv_hpersist`, `hotkv_hexpiretime`, `hotkv_hpexpiretime`

**Sorted sets**: `hotkv_zadd`, `hotkv_zincrby`, `hotkv_zrem`, `hotkv_zscore`, `hotkv_zmscore`, `hotkv_zrank`, `hotkv_zrevrank`, `hotkv_zcard`, `hotkv_zcount`, `hotkv_zlexcount`, `hotkv_zrange`, `hotkv_zrevrange`, `hotkv_zrangebyscore`, `hotkv_zrevrangebyscore`, `hotkv_zrangebylex`, `hotkv_zrevrangebylex`, `hotkv_zremrangebyscore`, `hotkv_zremrangebyrank`, `hotkv_zremrangebylex`, `hotkv_zunion`, `hotkv_zinter`, `hotkv_zdiff`, `hotkv_zunionstore`, `hotkv_zinterstore`, `hotkv_zdiffstore`, `hotkv_zintercard`, `hotkv_zpopmin`, `hotkv_zpopmax`, `hotkv_zrandmember`, `hotkv_zrandmember_count`, `hotkv_zscan`

**Geo and HyperLogLog**: `hotkv_geoadd`, `hotkv_geodist`, `hotkv_geopos`, `hotkv_geohash`, `hotkv_geosearch` (options in a `hotkv_geosearch_opts_t`), `hotkv_pfadd`, `hotkv_pfcount`, `hotkv_pfmerge`

**Streams**: `hotkv_xadd`, `hotkv_xlen`, `hotkv_xrange`, `hotkv_xrevrange`, `hotkv_xread`, `hotkv_xdel`, `hotkv_xtrim`, `hotkv_xinfo_stream`, `hotkv_xinfo_groups`, `hotkv_xinfo_consumers`, `hotkv_xgroup_create`, `hotkv_xgroup_destroy`, `hotkv_xgroup_createconsumer`, `hotkv_xgroup_delconsumer`, `hotkv_xgroup_setid`, `hotkv_xreadgroup`, `hotkv_xack`, `hotkv_xpending`, `hotkv_xpending_range`, `hotkv_xclaim`, `hotkv_xautoclaim`

**Keys**: `hotkv_exists`, `hotkv_unlink`, `hotkv_touch`, `hotkv_expire`, `hotkv_pexpire`, `hotkv_expireat`, `hotkv_pexpireat`, `hotkv_ttl`, `hotkv_pttl`, `hotkv_expiretime`, `hotkv_pexpiretime`, `hotkv_persist`, `hotkv_type`, `hotkv_rename`, `hotkv_renamenx`, `hotkv_copy`, `hotkv_move`, `hotkv_randomkey`, `hotkv_dump`, `hotkv_restore`, `hotkv_keys`, `hotkv_scan`

**Scripting**: `hotkv_eval`, `hotkv_eval_ro`, `hotkv_evalsha`, `hotkv_evalsha_ro`, `hotkv_script_load`, `hotkv_script_exists`, `hotkv_script_flush`, `hotkv_function_load`, `hotkv_fcall`, `hotkv_fcall_ro`

**Transactions**: `hotkv_multi`, `hotkv_exec`, `hotkv_discard`, `hotkv_watch`, `hotkv_unwatch`

**Pub/Sub**: `hotkv_publish`, `hotkv_subscribe`, `hotkv_psubscribe`, `hotkv_unsubscribe`, `hotkv_punsubscribe` (these consume the confirmation frames and return a status; read messages with `hotkv_get_reply`), `hotkv_pubsub_channels`, `hotkv_pubsub_numsub`, `hotkv_pubsub_numpat`

**Server**: `hotkv_ping`, `hotkv_echo`, `hotkv_dbsize`, `hotkv_flushdb`, `hotkv_flushall`, `hotkv_info`, `hotkv_select`, `hotkv_time`, `hotkv_lastsave`, `hotkv_config_get`

**HotKV-exclusive core commands**: `hotkv_nexpire`, `hotkv_nttl`, `hotkv_nexpireat`, `hotkv_tag_set`, `hotkv_tag_rem`, `hotkv_tag_get`, `hotkv_tag_members`, `hotkv_tag_inter`, `hotkv_tag_count`, `hotkv_tag_exists`, `hotkv_hotkeys_top`, `hotkv_hotkeys_reset`, `hotkv_latency_latest`, `hotkv_latency_history`, `hotkv_latency_reset`, `hotkv_gcra_limit`, `hotkv_gcra_reset`, `hotkv_vsim_*` (index, persistence, search)

**Not typed: use `hotkv_command()` / `hotkv_command_argv()`.** These families have no wrapper,
and the call works for all of them:

- Blocking list/zset commands: `BLPOP`, `BRPOP`, `BLMOVE`, `BRPOPLPUSH`, `BLMPOP`, `BZPOPMIN`, `BZPOPMAX`, `BZMPOP` (use a suitable `hotkv_set_timeout`), plus `LMPOP`, `ZMPOP`, `ZRANGESTORE`, `SORT`/`SORT_RO`, the legacy `GEORADIUS*` family, `GEOSEARCHSTORE`, `BITFIELD`, `SUBSTR`
- Probabilistic modules: `BF.*` (Bloom filter), `CMS.*` (count-min sketch), `TOPK.*`
- Connection, client and cluster administration: `AUTH`, `HELLO`, `RESET`, `QUIT`, `CLIENT` (including `CLIENT TRACKING`, `CACHING`, `GETREDIR`, `TRACKINGINFO`), `COMMAND`, `WAIT`, `WAITAOF`, `ACL`, `CLUSTER`, `ASKING`, `READONLY`, `READWRITE`, `MIGRATE`, `DEBUG`, `MODULE`, `MEMORY`, `OBJECT`, `SLOWLOG`, `MONITOR`, `ROLE`, `SAVE`, `BGSAVE`, `BGREWRITEAOF`, `SHUTDOWN`, `FAILOVER`, `REPLICAOF`/`SLAVEOF`, `SWAPDB`, `LOLWUT`
- Sharded pub/sub: `SPUBLISH`, `SSUBSCRIBE`, `SUNSUBSCRIBE`
- Newer hash and string commands: `HGETEX`, `HSETEX`, `HEXPIREAT`, `HPEXPIREAT`, `HIMPORT`, `MSETEX`, `DELEX`, `DIGEST`, `XCFGSET`, `XSETID`, `SFLUSH`, `CLUSTERSCAN`, `TRIMSLOTS`
- Enterprise administration: `TENANT.*`, `IAM.*`, `AUDIT.*`, `ENCRYPTION.*`, `REPLICATION.*`, `PSYNC`, the `*.SYNC` replication commands and `FEATURE.SETAT`

**Enterprise and AI** (see `hotkv.h` for the full, documented list; every wrapper
mirrors the server's argument order exactly): `hotkv_scache_*`, `hotkv_pcache_*`,
`hotkv_toolcache_*`, `hotkv_ncache_*`, `hotkv_guardcache_*`, `hotkv_rerankcache_*`,
`hotkv_llmstats_*`, `hotkv_llmbudget_*`, `hotkv_prompt_*`, `hotkv_embed_dedup_*`,
`hotkv_amem_*` (conversation, semantic, working, episodic memory, cross-memory
search, and `hotkv_amem_summary_*`), `hotkv_rag_*`, `hotkv_ratelimit_*`, `hotkv_ts_*`,
`hotkv_feature_*`, `hotkv_observe_*`, `hotkv_alerts_*`.
Enterprise commands need a licensed HotKV server; a server without the right
license replies with an error whose prefix is `NOLICENSE`, which
`hotkv_reply_is_nolicense()` detects (see also `hotkv_reply_is_moved`,
`hotkv_reply_is_ask`, `hotkv_reply_is_wrongtype`, `hotkv_reply_is_noauth`,
`hotkv_reply_is_readonly`, and `hotkv_reply_error_prefix` for other typed errors).

### Numbers on the wire

Every `double` and `float` argument is written as plain, locale independent decimal
text: no exponent (`1e-05` becomes `0.00001`), no decimal comma even if your program
called `setlocale(LC_ALL, "de_DE.UTF-8")`, and the shortest text that reads back as
exactly the same value. USD amounts (`hotkv_llmbudget_set`/`_consume`,
`hotkv_llmstats_record`, `hotkv_llmstats_price_set`) use the fixed point form the
server requires (`[0-9]+(.[0-9]{1,6})?`, rounded to the nearest micro-dollar);
a negative, NaN or infinite amount is refused locally and the wrapper returns an error
reply (`ERR invalid numeric argument ...`) without touching the connection. NaN is
likewise refused everywhere else.

### AI caching example

```c
#include <hotkv.h>

/* Called only on a cache miss. Setting out_usage->model attaches it to the
 * stored cache entry, so hotkv_promptcache_stats() reports the tokens and
 * cost a later hit saved. */
static int call_llm(void *userdata, char **out_response, hotkv_llm_usage_t *out_usage) {
    (void)userdata;
    *out_response = hotkv_strdup("Paris is the capital of France.");
    out_usage->model = "gpt-6-sol";
    out_usage->input_tokens = 12;
    out_usage->output_tokens = 8;
    out_usage->cost_usd = 0.0003;
    out_usage->latency_ms = 480;
    return HOTKV_OK;
}

int main(void) {
    hotkv_context_t *ctx = hotkv_connect("127.0.0.1", 6379);

    hotkv_promptcache_t pc;
    hotkv_promptcache_init(&pc, ctx, "chatbot", 3600 /* seconds */);
    hotkv_promptcache_set_stats_bucket(&pc, "chatbot-usage");
    hotkv_promptcache_create_namespace(&pc, 0 /* default max entries */, 0.0 /* cost/hit */);

    const char *roles[] = {"user"};
    const char *contents[] = {"What is the capital of France?"};
    char *response = NULL;
    int hit = 0;

    hotkv_promptcache_get_or_call(&pc, "openai", "gpt-4", NULL, roles, contents, 1, NULL, NULL, 0,
                                   call_llm, NULL, &response, &hit);
    printf("%s (cache hit: %s)\n", response, hit ? "yes" : "no");

    hotkv_pcache_stats_t stats;
    hotkv_promptcache_stats(&pc, &stats);
    printf("tokens saved so far: %lld\n", (long long)stats.usage.input_tokens);

    hotkv_free(response);
    hotkv_promptcache_destroy(&pc);
    hotkv_disconnect(ctx);
    return 0;
}
```

`hotkv_semanticcache_get_or_call` does the same for embedding based lookups, through
the matching `hotkv_text_call_fn` callback shape (`out_response`, `out_usage`).

### Token usage, cache savings and LLM cost tracking

`hotkv_pcache_set`, `hotkv_scache_set`, the keyed caches' `_set`
(`hotkv_toolcache_set`/`hotkv_ncache_set`/`hotkv_guardcache_set`/`hotkv_rerankcache_set`)
and `hotkv_llmbudget_consume` all take a trailing `const hotkv_token_usage_t *`
(`model` required, token counts optional, `NULL` to omit): recording the original
call's token usage lets a later cache hit attribute the tokens, and once the model
is priced, the cost, it did not have to spend. Their `hotkv_parse_*_stats` results
expose those savings as a `hotkv_usage_savings_t usage` field alongside the
existing counters.

`LLMSTATS.RECORD` with a model prices a call from HotKV's built-in price table
instead of a caller supplied cost; `hotkv_llmstats_price_set` adds or overrides a
model's price, and `hotkv_llmbudget_consume` with a model enforces a budget priced
the same way:

```c
/* Custom or fine-tuned model: 3 USD / 1M input tokens, 15 USD / 1M output. */
hotkv_free_reply(hotkv_llmstats_price_set(ctx, "acme-custom-v1", 14, 3.0, 15.0, NULL, NULL, NULL));

hotkv_free_reply(hotkv_llmstats_bucket(ctx, "chatbot-usage", 13));
hotkv_free_reply(hotkv_llmstats_record(ctx, "chatbot-usage", 13, 500, 120, 0, 0, 340, NULL,
                                        "acme-custom-v1", 0, 0));

hotkv_llmstats_get_t snap;
hotkv_reply_t *r = hotkv_llmstats_get(ctx, "chatbot-usage", 13);
hotkv_parse_llmstats_get(r, &snap);
hotkv_free_reply(r);
printf("spent $%.6f so far\n", snap.cost_usd);

hotkv_token_usage_t usage = {"acme-custom-v1", 500, 120, 0, 0, 0};
hotkv_reply_t *allowed = hotkv_llmbudget_consume(ctx, "chatbot-daily", 13, 0, 0.0, &usage);
if (allowed->integer == 0) {
    printf("daily LLM budget exceeded\n");
}
hotkv_free_reply(allowed);
```

### Reply types

| Constant              | Protocol    | Payload fields                        |
| --------------------- | ----------- | ------------------------------------- |
| `HOTKV_REPLY_STRING`  | `+`         | `str`, `len`                          |
| `HOTKV_REPLY_ERROR`   | `-`         | `str`, `len`                          |
| `HOTKV_REPLY_INTEGER` | `:`         | `integer`                             |
| `HOTKV_REPLY_BULK`    | `$`         | `str`, `len`                          |
| `HOTKV_REPLY_ARRAY`   | `*`         | `elements`, `nelem`                   |
| `HOTKV_REPLY_NIL`     | `$-1` / `_` | (none)                                |
| `HOTKV_REPLY_DOUBLE`  | `,`         | `dval`                                |
| `HOTKV_REPLY_BOOL`    | `#`         | `integer` (0/1)                       |
| `HOTKV_REPLY_MAP`     | `%`         | `elements`, `nelem` (key-value pairs) |
| `HOTKV_REPLY_SET`     | `~`         | `elements`, `nelem`                   |
| `HOTKV_REPLY_BIGNUM`  | `(`         | `str`, `len`                          |
| `HOTKV_REPLY_VERB`    | `=`         | `str`, `len`, `vtype`                 |
| `HOTKV_REPLY_PUSH`    | `>`         | `elements`, `nelem`                   |
| `HOTKV_REPLY_ATTR`    | `\|`        | `elements`, `nelem`                   |

### Connection pool

```c
hotkv_pool_t    *hotkv_pool_new(const hotkv_pool_options_t *opts);
void             hotkv_pool_free(hotkv_pool_t *pool);
hotkv_context_t *hotkv_pool_acquire(hotkv_pool_t *pool);
void             hotkv_pool_release(hotkv_pool_t *pool, hotkv_context_t *ctx);
size_t           hotkv_pool_active(const hotkv_pool_t *pool);
size_t           hotkv_pool_idle(const hotkv_pool_t *pool);
```

### Custom allocator

```c
hotkv_set_allocator(my_malloc, my_calloc, my_realloc, my_free);
```

Must be called before any other `hotkv_*` function. Not thread-safe.

## Testing

Unit tests need no server:

```bash
cmake -S . -B build -DHOTKV_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

| Test | What it covers | Needs |
| --- | --- | --- |
| `resp_parser`, `commands`, `pool`, `url_parser`, `crc16`, `enterprise_commands`, `ai_helpers` | parser, command encoding, pool, URL, CRC, enterprise helpers | nothing |
| `number_format` | locale independent number text, the printf-style command formatter | nothing |
| `wire_format` | exact bytes sent for numbers and a few commands, against a fake server (POSIX) | nothing |
| `integration` | broad smoke test of the typed wrappers, pipelining, pub/sub, RESP2 and RESP3 | `HOTKV_TEST_URL` |
| `live_core` | every typed core wrapper, reply values checked, run in RESP2 and RESP3 | `HOTKV_TEST_URL` |
| `live_enterprise` | every typed enterprise/AI wrapper, reply values checked (reports skipped on an unlicensed server) | `HOTKV_TEST_URL`, licensed server |

The live tests are skipped (CTest reports "Skipped", exit code 77) when `HOTKV_TEST_URL`
is not set:

```bash
export HOTKV_TEST_URL=hotkv://127.0.0.1:6379
ctest --test-dir build --output-on-failure
```

They create keys named `sdktest:<run id>:...` and namespaces derived from them. Point them at a
disposable server: `live_core` only exercises `FLUSHALL` when `HOTKV_TEST_FLUSHALL=1`. Set `HOTKV_SHOW=1` to print replies while debugging.

Run the suites under AddressSanitizer or valgrind with
`-DCMAKE_C_FLAGS="-fsanitize=address,undefined"` (they are clean as released).

## Examples

See the [examples/](examples/) directory:

- **basic.c** - Connect, SET, GET, INCR, DEL
- **pipeline.c** - Batch 1000 commands via pipelining
- **tls.c** - TLS connection with certificate verification
- **consumer/**: A CMake project that builds against an installed libhotkv with `find_package(hotkv)`

## Contributing

Bug reports and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Support

- Bugs and feature requests: [GitHub issues](https://github.com/hotkv-ltd/hotkv-c-client/issues)
- Questions and commercial support: support@hotkv.com
- Security vulnerabilities: see [SECURITY.md](SECURITY.md)

## License

Licensed under the Apache License, Version 2.0: see [LICENSE](LICENSE) and [NOTICE](NOTICE).

## Trademarks and affiliation

"HotKV" is a trademark of HotKV Ltd; the license does not grant rights to use it.
The HotKV server is a separate commercial product, and its enterprise commands need a HotKV license.

HotKV is an independent product of HotKV Ltd. It speaks the RESP protocol and implements
many Redis commands so that existing tools and client habits carry over, but it is not Redis,
Valkey, Dragonfly or KeyDB, and this SDK is built for and tested against HotKV. HotKV Ltd is not
affiliated with, endorsed by or sponsored by Redis Ltd., the Valkey project, DragonflyDB or KeyDB.
Redis is a registered trademark of Redis Ltd. Valkey, Dragonfly, KeyDB and all other product and
company names are trademarks of their respective owners; they are used here only to describe
protocol and command compatibility.
