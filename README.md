# redis-compressed

A Redis module that adds `COMPRESSED.JSON.GET`, a read-only command that wraps `JSON.GET` and returns a Brotli-compressed version of the data, and `COMPRESSED.JSON.COMPRESS`, a write command that rewrites a RedisJSON value into a stored flagged blob.

## Why

For large documents, response times may easily become limited by network transfer. Applying light compression during transport may improve the overall performance significantly despite the small associated CPU overhead.

For plain strings (e.g., using `SET`), this would be better handled in the client by storing compressed data directly. However, with JSON, if we want to keep the ability to work with individual fields using the existing RedisJSON commands, the data must be stored in its original form. This module therefore implements in-transit compression for when the data is read. Additionally, it allows rewriting JSON keys to compressed strings once data manipulation is no longer needed.

## Behavior

`COMPRESSED.JSON.GET key [path ...]` behaves as follows:

- if `key` is a RedisJSON value, it forwards its arguments to `JSON.GET` and encodes the reply
- if `key` is already a plain string beginning with the module's flag byte, it only accepts `COMPRESSED.JSON.GET key` and returns that stored value unchanged

The encoding format is:
- `0x00` + raw JSON when the payload is smaller than `compressed.transport-threshold-bytes`
- `0x01` + Brotli-compressed JSON when the payload is at or above the threshold

The command preserves RedisJSON error behavior for missing keys and invalid access.

`COMPRESSED.JSON.COMPRESS key [INDENT indent] [NEWLINE newline] [SPACE space]` fetches the JSON document with `JSON.GET` using the same optional formatting arguments, encodes that reply with the same flag format, and stores it back into `key` as a Redis string.

After `COMPRESSED.JSON.COMPRESS`, the key is no longer a RedisJSON value. `JSON.GET`, `JSON.SET`, and other RedisJSON commands will treat it as a plain string key. `COMPRESSED.JSON.GET` still works for that key, but only in the bare `COMPRESSED.JSON.GET key` form; path and formatting arguments are rejected because the stored value is a full pre-encoded payload rather than a live RedisJSON document. Repeated `COMPRESSED.JSON.COMPRESS` calls on an already stored payload are treated as a no-op, even if formatting arguments are provided.

## Module Config

The module registers four runtime config values:

- `compressed.transport-level`
  Brotli quality level used by `COMPRESSED.JSON.GET`, default `1`, allowed range `0..11`
- `compressed.storage-level`
  Brotli quality level used by `COMPRESSED.JSON.COMPRESS`, default `2`, allowed range `0..11`
- `compressed.transport-threshold-bytes`
  Minimum JSON reply size before `COMPRESSED.JSON.GET` compresses the response, default `10240`
- `compressed.storage-threshold-bytes`
  Minimum JSON reply size before `COMPRESSED.JSON.COMPRESS` stores a compressed payload, default `1024`

Example:

```redis
CONFIG SET compressed.transport-level 4
CONFIG SET compressed.storage-level 5
CONFIG SET compressed.transport-threshold-bytes 4096
CONFIG SET compressed.storage-threshold-bytes 4096
```

## Build and Test

Most local workflows go through `make`:

```sh
make release
make debug
make test
make test-debug
```

Build artifacts are written to `build/release/libredis-compressed.so` and `build/debug/libredis-compressed.so`.

## Load In Redis Stack

This module expects RedisJSON to be available because it calls `JSON.GET`.

Example with `redis-stack-server`:

```sh
redis-stack-server --loadmodule /path/to/libredis-compressed.so
```

Example usage:

```redis
JSON.SET doc $ "{\"message\":\"hello\"}"
COMPRESSED.JSON.GET doc
COMPRESSED.JSON.COMPRESS doc
COMPRESSED.JSON.GET doc
```

Client-side decoding rules:

- If the first byte is `0x00`, treat the remaining bytes as UTF-8 JSON.
- If the first byte is `0x01`, Brotli-decompress the remaining bytes and then parse the JSON.
