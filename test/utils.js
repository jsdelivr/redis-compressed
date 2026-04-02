import { spawnSync } from 'node:child_process';

export function commandExists (command) {
	const result = spawnSync(command, [ '--version' ], { stdio: 'ignore' });
	return !result.error;
}

export function run (command, args, options = {}) {
	const result = spawnSync(command, args, {
		encoding: 'utf8',
		...options,
	});

	if (result.error) {
		throw result.error;
	}

	return result;
}

export function runChecked (command, args, options = {}) {
	const result = run(command, args, options);
	if (result.status !== 0) {
		const stderr = toTrimmedString(result.stderr);
		const stdout = toTrimmedString(result.stdout);
		throw new Error(stderr || stdout || `${command} exited with status ${result.status}`);
	}
	return result;
}

export function toTrimmedString (value) {
	if (value == null) {
		return '';
	}

	if (Buffer.isBuffer(value)) {
		return value.toString('utf8').trim();
	}

	return String(value).trim();
}

export function sleep (ms) {
	Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

export function createRedisStackHarness ({ containerName, redisStackImage, moduleDir, moduleFile }) {
	let containerStarted = false;

	function docker (args, options = {}) {
		return run('docker', args, options);
	}

	function dockerChecked (args, options = {}) {
		return runChecked('docker', args, options);
	}

	function cli (args, options = {}) {
		return docker([ 'exec', containerName, 'redis-cli', '--raw', ...args ], options);
	}

	function cliChecked (args, options = {}) {
		return dockerChecked([ 'exec', containerName, 'redis-cli', '--raw', ...args ], options);
	}

	function cliHex (args) {
		const result = cliChecked(args, { encoding: null });
		let output = result.stdout;
		if (output.length > 0 && output[output.length - 1] === 0x0a) {
			output = output.subarray(0, output.length - 1);
		}
		return output.toString('hex');
	}

	function getContainerState () {
		const inspect = docker([
			'inspect',
			'--format',
			'{{.State.Status}}',
			containerName,
		]);

		if (inspect.status !== 0) {
			return null;
		}

		return inspect.stdout.trim();
	}

	function getContainerLogs () {
		const logs = docker([ 'logs', containerName ]);
		return `${logs.stdout || ''}${logs.stderr || ''}`.trim();
	}

	function startContainer () {
		dockerChecked([
			'run',
			'-d',
			'--rm',
			'--name',
			containerName,
			'-v',
			`${moduleDir}:/module:ro`,
			redisStackImage,
			'redis-stack-server',
			'--save',
			'',
			'--appendonly',
			'no',
			'--loadmodule',
			`/module/${moduleFile}`,
		]);
		containerStarted = true;

		for (let i = 0; i < 30; i += 1) {
			const state = getContainerState();
			if (state === 'exited' || state === 'dead') {
				const logs = getContainerLogs();
				throw new Error(`redis stack container exited during startup${logs ? `\n${logs}` : ''}`);
			}

			const ping = docker([ 'exec', containerName, 'redis-cli', 'ping' ]);
			if (ping.status === 0 && ping.stdout.trim() === 'PONG') {
				return;
			}

			sleep(1000);
		}

		const state = getContainerState();
		const logs = getContainerLogs();
		throw new Error(`redis stack container did not become ready (state: ${state ?? 'missing'})${logs ? `\n${logs}` : ''}`);
	}

	function cleanup (exitCode) {
		if (!containerStarted) {
			return;
		}

		const inspect = docker([ 'ps', '-a', '--format', '{{.Names}}' ]);
		if (inspect.status === 0) {
			const names = inspect.stdout.trim().split('\n').filter(Boolean);
			if (names.includes(containerName)) {
				if (exitCode !== 0) {
					const logs = docker([ 'logs', containerName ]);
					process.stderr.write(logs.stdout || '');
					process.stderr.write(logs.stderr || '');
				}
				docker([ 'rm', '-f', containerName ], { stdio: 'ignore' });
			}
		}
	}

	return {
		cli,
		cliChecked,
		cliHex,
		cleanup,
		startContainer,
	};
}
