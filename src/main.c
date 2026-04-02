#define REDISMODULE_MAIN
#include "redismodule.h"

#include <brotli/encode.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

static long long g_brotli_quality = 1;
static long long g_compression_threshold = 10 * 1024;

static long long GetCompressionLevelConfig(const char* name, void* privdata) {
	REDISMODULE_NOT_USED(name);
	REDISMODULE_NOT_USED(privdata);
	return g_brotli_quality;
}

static int SetCompressionLevelConfig(const char* name, long long value, void* privdata, RedisModuleString** err) {
	REDISMODULE_NOT_USED(name);
	REDISMODULE_NOT_USED(privdata);
	REDISMODULE_NOT_USED(err);
	g_brotli_quality = value;
	return REDISMODULE_OK;
}

static long long GetCompressionThresholdConfig(const char* name, void* privdata) {
	REDISMODULE_NOT_USED(name);
	REDISMODULE_NOT_USED(privdata);
	return g_compression_threshold;
}

static int SetCompressionThresholdConfig(const char* name, long long value, void* privdata, RedisModuleString** err) {
	REDISMODULE_NOT_USED(name);
	REDISMODULE_NOT_USED(privdata);
	REDISMODULE_NOT_USED(err);
	g_compression_threshold = value;
	return REDISMODULE_OK;
}

static int CompressedJsonGetCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
	RedisModule_AutoMemory(ctx);

	if (argc < 2) {
		return RedisModule_WrongArity(ctx);
	}

	RedisModuleCallReply* json_reply = RedisModule_Call(ctx, "JSON.GET", "v", argv + 1, argc - 1);
	if (json_reply == NULL) {
		return RedisModule_ReplyWithError(ctx, "ERR failed to call JSON.GET");
	}

	int reply_type = RedisModule_CallReplyType(json_reply);
	if (reply_type == REDISMODULE_REPLY_ERROR) {
		RedisModule_ReplyWithCallReply(ctx, json_reply);
		return REDISMODULE_OK;
	}

	if (reply_type == REDISMODULE_REPLY_NULL) {
		RedisModule_ReplyWithNull(ctx);
		return REDISMODULE_OK;
	}

	if (reply_type != REDISMODULE_REPLY_STRING) {
		return RedisModule_ReplyWithError(ctx, "ERR unexpected JSON.GET reply type");
	}

	size_t input_len = 0;
	const uint8_t* input = (const uint8_t*)RedisModule_CallReplyStringPtr(json_reply, &input_len);

	if (input_len < (size_t)g_compression_threshold) {
		size_t reply_len = input_len + 1;
		uint8_t* reply = RedisModule_Alloc(reply_len);
		reply[0] = 0x00;
		if (input_len > 0) {
			memcpy(reply + 1, input, input_len);
		}
		RedisModule_ReplyWithStringBuffer(ctx, (const char*)reply, reply_len);
		RedisModule_Free(reply);
		return REDISMODULE_OK;
	}

	size_t max_compressed_len = BrotliEncoderMaxCompressedSize(input_len);
	if (max_compressed_len == 0) {
		return RedisModule_ReplyWithError(ctx, "ERR brotli max size calculation failed");
	}

	uint8_t* compressed = RedisModule_Alloc(max_compressed_len + 1);
	size_t compressed_len = max_compressed_len;

	BROTLI_BOOL ok = BrotliEncoderCompress(
		(int)g_brotli_quality,
		BROTLI_DEFAULT_WINDOW,
		BROTLI_MODE_TEXT,
		input_len,
		input,
		&compressed_len,
		compressed + 1
	);

	if (!ok) {
		RedisModule_Free(compressed);
		return RedisModule_ReplyWithError(ctx, "ERR brotli compression failed");
	}

	compressed[0] = 0x01;
	RedisModule_ReplyWithStringBuffer(ctx, (const char*)compressed, compressed_len + 1);

	RedisModule_Free(compressed);
	return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
	REDISMODULE_NOT_USED(argv);
	REDISMODULE_NOT_USED(argc);

	if (RedisModule_Init(ctx, "compressed", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	if (RedisModule_RegisterNumericConfig(
		ctx,
		"level",
		1,
		REDISMODULE_CONFIG_DEFAULT,
		0,
		11,
		GetCompressionLevelConfig,
		SetCompressionLevelConfig,
		NULL,
		NULL) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	if (RedisModule_RegisterNumericConfig(
		ctx,
		"threshold-bytes",
		10 * 1024,
		REDISMODULE_CONFIG_DEFAULT | REDISMODULE_CONFIG_MEMORY,
		0,
		LLONG_MAX,
		GetCompressionThresholdConfig,
		SetCompressionThresholdConfig,
		NULL,
		NULL) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	if (RedisModule_LoadConfigs(ctx) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	if (RedisModule_CreateCommand(
		ctx,
		"COMPRESSED.JSON.GET",
		CompressedJsonGetCommand,
		"readonly",
		1,
		1,
		1) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	return REDISMODULE_OK;
}
