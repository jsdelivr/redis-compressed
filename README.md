# redis-compressed

A Redis module that adds `COMPRESSED.JSON.GET`, a read-only command that wraps `JSON.GET` and returns a Brotli-compressed version of the data.

## Why

For large documents, response times may easily become limited by network transfer. Applying light compression during transport may improve the overall performance significantly despite the small associated CPU overhead.

For plain strings (e.g., using `SET`), this would be better handled in the client by storing compressed data directly. However, with JSON, if we want to keep the ability to work with individual fields using the existing RedisJSON commands, the data must be stored in its original form. This module therefore implements only in-transit compression for when the data is read.

Currently, the only supported function is `COMPRESSED.JSON.GET`,
acting as a proxy to `JSON.GET`. Support for other data types or functions may be added later.

## Behavior

`COMPRESSED.JSON.GET key [path ...]` forwards its arguments to `JSON.GET` and encodes the reply as:

- `0x00` + raw JSON when the payload is smaller than `compressed.threshold-bytes`
- `0x01` + Brotli-compressed JSON when the payload is at or above the threshold

The command preserves RedisJSON error behavior for missing keys and invalid access.

## Module Config

The module registers two runtime config values:

- `compressed.level`
  Brotli quality level, default `1`, allowed range `0..11`
- `compressed.threshold-bytes`
  Minimum JSON reply size before compression is applied, default `10240`

Example:

```redis
CONFIG SET compressed.level 4
CONFIG SET compressed.threshold-bytes 4096
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
```

Client-side decoding rules:

- If the first byte is `0x00`, treat the remaining bytes as UTF-8 JSON.
- If the first byte is `0x01`, Brotli-decompress the remaining bytes and then parse the JSON.
