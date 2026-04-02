import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import { createRedisStackHarness, dockerExists } from '../utils.js';

const modulePath = process.env.REDIS_COMPRESSED_MODULE || process.argv[2];
const redisStackImage = process.env.REDIS_STACK_IMAGE || 'redis/redis-stack-server:7.4.0-v1';

if (!modulePath) {
	console.error(`usage: ${process.argv[1]} <module-path>`);
	process.exit(1);
}

if (!fs.existsSync(modulePath)) {
	console.error(`module not found: ${modulePath}`);
	process.exit(1);
}

if (!dockerExists()) {
	console.error('docker is required to run the integration test');
	process.exit(1);
}

const moduleDir = path.dirname(path.resolve(modulePath));
const moduleFile = path.basename(modulePath);
const containerName = `redis-compressed-test-${process.pid}-${Math.floor(Math.random() * 1_000_000)}`;

const {
	cli,
	cliBuffer,
	cliChecked,
	cleanup,
	startContainer
} = createRedisStackHarness({
	containerName,
	redisStackImage,
	moduleDir,
	moduleFile,
});

process.on('exit', (exitCode) => {
	try {
		cleanup(exitCode == null ? (process.exitCode ?? 0) : exitCode);
	} catch {
		// Best-effort cleanup only; don't mask the real test failure.
	}
});
process.on('SIGINT', () => process.exit(1));
process.on('SIGTERM', () => process.exit(1));

before(async () => {
	await startContainer();
});

after(() => {
	cleanup(0);
});

function decodeCompressedJsonReply (reply) {
	assert.ok(reply.length >= 1, 'reply should include a prefix byte');

	const prefix = reply[0];
	const payload = reply.subarray(1);

	if (prefix === 0x00) {
		return payload.toString('utf8');
	}

	if (prefix === 0x01) {
		return zlib.brotliDecompressSync(payload).toString('utf8');
	}

	assert.fail(`unexpected reply prefix: 0x${prefix.toString(16).padStart(2, '0')}`);
}

function assertTransportPrefix (reply, expectedPrefix) {
	assert.equal(
		reply[0].toString(16).padStart(2, '0'),
		expectedPrefix,
		'reply prefix should match the expected transport mode',
	);
}

function assertJsonGetMatchesCompressedJsonGetAsString (args, expectedPrefix) {
	const rawGet = cliBuffer([ 'JSON.GET', ...args ]);
	const compressedGet = cliBuffer([ 'COMPRESSED.JSON.GET', ...args ]);

	assertTransportPrefix(compressedGet, expectedPrefix);

	assert.equal(
		decodeCompressedJsonReply(compressedGet),
		rawGet.toString('utf8'),
	);
}

function assertJsonGetMatchesCompressedJsonGetAsObject (args, expectedPrefix) {
	const rawGet = cliBuffer([ 'JSON.GET', ...args ]);
	const compressedGet = cliBuffer([ 'COMPRESSED.JSON.GET', ...args ]);

	assertTransportPrefix(compressedGet, expectedPrefix);

	assert.deepEqual(
		JSON.parse(decodeCompressedJsonReply(compressedGet)),
		JSON.parse(rawGet.toString('utf8')),
	);
}

test('wrong arity is reported', () => {
	const result = cli([ 'COMPRESSED.JSON.GET' ]);

	assert.equal(
		result.stdout.trim(),
		'ERR wrong number of arguments for \'COMPRESSED.JSON.GET\' command',
	);
});

test('reading a plain string through JSON.GET fails', () => {
	assert.equal(cliChecked([ 'SET', 'plain-key', 'plain-value' ]).stdout.trim(), 'OK', 'setting a plain string key succeeds');

	const result = cli([ 'COMPRESSED.JSON.GET', 'plain-key' ]);

	assert.match(
		result.stdout.trim(),
		/^(Existing key has wrong Redis type|WRONGTYPE|ERR)/,
	);
});

test('missing keys propagate as null replies', () => {
	assert.equal(
		cliChecked([ 'COMPRESSED.JSON.GET', 'missing-key' ]).stdout.trim(),
		'',
	);
});

test('small JSON replies are returned uncompressed with a 0x00 prefix', () => {
	assert.equal(
		cliChecked([ 'CONFIG', 'SET', 'compressed.threshold-bytes', '4096' ]).stdout.trim(),
		'OK',
	);

	assert.equal(
		cliChecked([ 'JSON.SET', 'small-doc', '$', '{"message":"hello"}' ]).stdout.trim(),
		'OK',
	);

	const rawGet = cliBuffer([ 'JSON.GET', 'small-doc' ]);
	const compressedGet = cliBuffer([ 'COMPRESSED.JSON.GET', 'small-doc' ]);

	assert.deepEqual(
		compressedGet,
		Buffer.concat([ Buffer.from([ 0x00 ]), rawGet ]),
	);
});

test('large JSON replies are returned compressed with a 0x01 prefix', () => {
	assert.equal(
		cliChecked([ 'CONFIG', 'SET', 'compressed.threshold-bytes', '1' ]).stdout.trim(),
		'OK',
	);

	const largePayload = 'x'.repeat(2048);
	const largeJson = JSON.stringify({ payload: largePayload });

	assert.equal(cliChecked([ 'JSON.SET', 'large-doc', '$', largeJson ]).stdout.trim(), 'OK');

	const rawGet = cliBuffer([ 'JSON.GET', 'large-doc' ]);
	const compressedGet = cliBuffer([ 'COMPRESSED.JSON.GET', 'large-doc' ]);

	assert.equal(
		decodeCompressedJsonReply(compressedGet),
		rawGet.toString('utf8'),
	);

	const largeJsonBytes = rawGet.length;
	const largeReplyBytes = compressedGet.length - 1;

	assert.ok(largeReplyBytes < largeJsonBytes, `compressed payload should be smaller than the original JSON\njson-bytes: ${largeJsonBytes}\nbrotli-bytes: ${largeReplyBytes}`);
});

test('JSON.GET formatting options are forwarded unchanged', () => {
	assert.equal(
		cliChecked([ 'CONFIG', 'SET', 'compressed.threshold-bytes', '4096' ]).stdout.trim(),
		'OK',
	);

	assert.equal(
		cliChecked([ 'JSON.SET', 'formatted-doc', '$', '{"outer":{"message":"hello","value":1}}' ]).stdout.trim(),
		'OK',
	);

	const args = [
		'formatted-doc',
		'INDENT', '  ',
		'NEWLINE', '\n',
		'SPACE', ' ',
	];

	assertJsonGetMatchesCompressedJsonGetAsString(args, '00');
});

test('JSON.GET formatting options are forwarded unchanged for compressed replies', () => {
	assert.equal(
		cliChecked([ 'CONFIG', 'SET', 'compressed.threshold-bytes', '1' ]).stdout.trim(),
		'OK',
	);

	assert.equal(
		cliChecked([ 'JSON.SET', 'formatted-large-doc', '$', '{"outer":{"message":"' + 'x'.repeat(2048) + '","value":1}}' ]).stdout.trim(),
		'OK',
	);

	const args = [
		'formatted-large-doc',
		'INDENT', '  ',
		'NEWLINE', '\n',
		'SPACE', ' ',
	];

	assertJsonGetMatchesCompressedJsonGetAsString(args, '01');
});

test('JSON.GET path arguments are forwarded unchanged', () => {
	assert.equal(
		cliChecked([ 'CONFIG', 'SET', 'compressed.threshold-bytes', '4096' ]).stdout.trim(),
		'OK',
	);

	assert.equal(
		cliChecked([ 'JSON.SET', 'path-doc', '$', '{"outer":{"first":1,"second":2},"items":[{"id":1},{"id":2}]}' ]).stdout.trim(),
		'OK',
	);

	const singlePathArgs = [ 'path-doc', '$.outer.first' ];
	assertJsonGetMatchesCompressedJsonGetAsObject(singlePathArgs, '00');

	const multiPathArgs = [ 'path-doc', '$.outer.first', '$.items[*].id' ];
	assertJsonGetMatchesCompressedJsonGetAsObject(multiPathArgs, '00');
});

test('JSON.GET path arguments are forwarded unchanged for compressed replies', () => {
	assert.equal(
		cliChecked([ 'CONFIG', 'SET', 'compressed.threshold-bytes', '1' ]).stdout.trim(),
		'OK',
	);

	const largePayload = 'x'.repeat(2048);
	const pathDoc = JSON.stringify({
		outer: {
			first: largePayload,
			second: largePayload,
		},
		items: [
			{ id: largePayload },
			{ id: largePayload },
		],
	});

	assert.equal(
		cliChecked([ 'JSON.SET', 'path-large-doc', '$', pathDoc ]).stdout.trim(),
		'OK',
	);

	const singlePathArgs = [ 'path-large-doc', '$.outer.first' ];
	assertJsonGetMatchesCompressedJsonGetAsObject(singlePathArgs, '01');

	const multiPathArgs = [ 'path-large-doc', '$.outer.first', '$.items[*].id' ];
	assertJsonGetMatchesCompressedJsonGetAsObject(multiPathArgs, '01');
});
