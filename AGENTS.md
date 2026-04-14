# AGENTS.md

## Project Overview

- `redis-compressed` is a Redis module written in C.
- It adds two commands:
  - `COMPRESSED.JSON.GET`: reads a RedisJSON document, returns a flagged payload, and also supports reading already-stored flagged string values.
  - `COMPRESSED.JSON.COMPRESS`: reads a RedisJSON document with optional formatting args and rewrites the key into a stored flagged string blob.
- Payload format:
  - `0x00` + raw JSON
  - `0x01` + Brotli-compressed JSON

## Code Layout

- `src/main.c`
  Module implementation.
- `include/redismodule.h`
  Redis Modules API header.
- `test/tests/integration.js`
  End-to-end tests using Docker and Redis Stack.
- `test/utils.js`
  Docker/test harness helpers.
- `README.md`
  User-facing behavior, config, and build/test instructions.

## Build And Test

- Preferred command on Windows with WSL:
  `wsl bash -lc "cd <repo-path> && make test"`
- Preferred command on other platforms:
  `make test`
