#define REDISMODULE_MAIN
#include "redismodule.h"

#include <brotli/encode.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

static long long g_brotli_quality = 1;
static long long g_compression_threshold = 10 * 1024;

typedef enum {
	ENCODE_PAYLOAD_OK = 0,
	ENCODE_PAYLOAD_ERR_INPUT_TOO_LARGE,
	ENCODE_PAYLOAD_ERR_BROTLI_FAILED,
} EncodePayloadStatus;

static EncodePayloadStatus EncodePayload(const uint8_t* input, size_t input_len, uint8_t** encoded_out, size_t* encoded_len_out) {
	if (input_len < (size_t)g_compression_threshold) {
		size_t encoded_len = input_len + 1;
		uint8_t* encoded = RedisModule_Alloc(encoded_len);
		encoded[0] = 0x00;
		if (input_len > 0) {
			memcpy(encoded + 1, input, input_len);
		}
		*encoded_out = encoded;
		*encoded_len_out = encoded_len;
		return ENCODE_PAYLOAD_OK;
	}

	size_t max_compressed_len = BrotliEncoderMaxCompressedSize(input_len);
	if (max_compressed_len == 0) {
		return ENCODE_PAYLOAD_ERR_INPUT_TOO_LARGE;
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
		return ENCODE_PAYLOAD_ERR_BROTLI_FAILED;
	}

	compressed[0] = 0x01;
	*encoded_out = compressed;
	*encoded_len_out = compressed_len + 1;
	return ENCODE_PAYLOAD_OK;
}

static int ReplyWithEncodePayloadError(RedisModuleCtx* ctx, EncodePayloadStatus status, size_t input_len) {
	switch (status) {
		case ENCODE_PAYLOAD_ERR_INPUT_TOO_LARGE:
			return RedisModule_ReplyWithErrorFormat(ctx, "ERR brotli input too large (%zu bytes)", input_len);
		case ENCODE_PAYLOAD_ERR_BROTLI_FAILED:
			return RedisModule_ReplyWithError(ctx, "ERR brotli compression failed");
		case ENCODE_PAYLOAD_OK:
		default:
			return RedisModule_ReplyWithError(ctx, "ERR unexpected payload encoding status");
	}
}

static int IsCompressedPayload(const uint8_t* input, size_t input_len) {
	return input_len >= 1 && (input[0] == 0x00 || input[0] == 0x01);
}

static int IsFormattingKeyword(const char* arg, size_t arg_len) {
	return (arg_len == 6 && strncasecmp(arg, "INDENT", 6) == 0) ||
		(arg_len == 7 && strncasecmp(arg, "NEWLINE", 7) == 0) ||
		(arg_len == 5 && strncasecmp(arg, "SPACE", 5) == 0);
}

static int ValidateCompressArguments(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
	if ((argc - 2) % 2 != 0) {
		RedisModule_ReplyWithError(ctx, "ERR unsupported arguments; only INDENT, NEWLINE, and SPACE are supported");
		return REDISMODULE_ERR;
	}

	for (int i = 2; i < argc; i += 2) {
		size_t arg_len = 0;
		const char* arg = RedisModule_StringPtrLen(argv[i], &arg_len);
		if (!IsFormattingKeyword(arg, arg_len)) {
			RedisModule_ReplyWithError(ctx, "ERR unsupported arguments; only INDENT, NEWLINE, and SPACE are supported");
			return REDISMODULE_ERR;
		}
	}

	return REDISMODULE_OK;
}

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

	RedisModuleKey* key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
	int key_type = RedisModule_KeyType(key);
	if (key_type == REDISMODULE_KEYTYPE_STRING) {
		size_t input_len = 0;
		const char* input = RedisModule_StringDMA(key, &input_len, REDISMODULE_READ);
		if (input != NULL && IsCompressedPayload((const uint8_t*)input, input_len)) {
			if (argc != 2) {
				return RedisModule_ReplyWithError(ctx, "ERR path and formatting arguments are not supported for stored compressed values");
			}
			RedisModule_ReplyWithStringBuffer(ctx, input, input_len);
			return REDISMODULE_OK;
		}

		return RedisModule_ReplyWithError(ctx, "ERR the string does not contain a compressed payload");
	}

	RedisModule_CloseKey(key);

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
	uint8_t* encoded = NULL;
	size_t encoded_len = 0;
	EncodePayloadStatus encode_status = EncodePayload(input, input_len, &encoded, &encoded_len);
	if (encode_status != ENCODE_PAYLOAD_OK) {
		return ReplyWithEncodePayloadError(ctx, encode_status, input_len);
	}

	RedisModule_ReplyWithStringBuffer(ctx, (const char*)encoded, encoded_len);
	RedisModule_Free(encoded);
	return REDISMODULE_OK;
}

static int CompressedJsonCompressCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
	RedisModule_AutoMemory(ctx);

	if (argc < 2) {
		return RedisModule_WrongArity(ctx);
	}

	if (ValidateCompressArguments(ctx, argv, argc) == REDISMODULE_ERR) {
		return REDISMODULE_OK;
	}

	RedisModuleKey* key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
	int key_type = RedisModule_KeyType(key);
	mstime_t abs_expire = RedisModule_GetAbsExpire(key);
	if (key_type == REDISMODULE_KEYTYPE_EMPTY) {
		RedisModule_ReplyWithNull(ctx);
		return REDISMODULE_OK;
	}

	if (key_type == REDISMODULE_KEYTYPE_STRING) {
		size_t input_len = 0;
		const char* input = RedisModule_StringDMA(key, &input_len, REDISMODULE_READ);
		if (input != NULL && IsCompressedPayload((const uint8_t*)input, input_len)) {
			RedisModule_ReplyWithSimpleString(ctx, "OK");
			return REDISMODULE_OK;
		}
	}

	RedisModule_CloseKey(key);

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
	uint8_t* encoded = NULL;
	size_t encoded_len = 0;
	EncodePayloadStatus encode_status = EncodePayload(input, input_len, &encoded, &encoded_len);

	if (encode_status != ENCODE_PAYLOAD_OK) {
		return ReplyWithEncodePayloadError(ctx, encode_status, input_len);
	}

	key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
	RedisModuleString* value = RedisModule_CreateString(ctx, (const char*)encoded, encoded_len);
	RedisModule_Free(encoded);

	if (RedisModule_StringSet(key, value) == REDISMODULE_ERR) {
		return RedisModule_ReplyWithError(ctx, "ERR failed to store the compressed value");
	}

	if (abs_expire != REDISMODULE_NO_EXPIRE && RedisModule_SetAbsExpire(key, abs_expire) == REDISMODULE_ERR) {
		return RedisModule_ReplyWithError(ctx, "ERR failed to restore key expiry");
	}

	RedisModule_ReplicateVerbatim(ctx);
	RedisModule_ReplyWithSimpleString(ctx, "OK");
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
		"readonly deny-oom",
		1,
		1,
		1) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	if (RedisModule_CreateCommand(
		ctx,
		"COMPRESSED.JSON.COMPRESS",
		CompressedJsonCompressCommand,
		"write deny-oom",
		1,
		1,
		1) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	return REDISMODULE_OK;
}
