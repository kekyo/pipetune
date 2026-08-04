import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';

const [policy04Loader, policy04Script, policy05Config, policy05Script] = process.argv.slice(2);

const commandExists = command =>
  spawnSync('sh', ['-c', `command -v ${command}`], { stdio: 'ignore' }).status === 0;

if (!['pipewire', 'wireplumber', 'pw-metadata'].every(commandExists)) {
  console.log('PipeWire or WirePlumber test tools are unavailable; skipping policy integration test');
  process.exit(77);
}

const versionResult = spawnSync('wireplumber', ['--version'], { encoding: 'utf8' });
assert.equal(versionResult.status, 0, versionResult.stderr);
const versionMatch = `${versionResult.stdout}\n${versionResult.stderr}`.match(/(?:Compiled|Linked) with libwireplumber (\d+)\.(\d+)/);
assert.notEqual(versionMatch, null, 'cannot determine the WirePlumber runtime version');
const majorVersion = Number(versionMatch[1]);
if (majorVersion !== 0 || !['4', '5'].includes(versionMatch[2])) {
  console.log(`WirePlumber ${versionMatch[1]}.${versionMatch[2]} is outside the supported integration matrix`);
  process.exit(77);
}
const minorVersion = Number(versionMatch[2]);

const temporaryRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'pipetune-wireplumber-policy-'));
const runtimeDirectory = path.join(temporaryRoot, 'runtime');
const configDirectory = path.join(temporaryRoot, 'config');
await fs.mkdir(runtimeDirectory, { mode: 0o700 });

const copyPolicy = async (source, relativeDestination) => {
  const destination = path.join(configDirectory, 'wireplumber', relativeDestination);
  await fs.mkdir(path.dirname(destination), { recursive: true });
  await fs.copyFile(source, destination);
};

if (minorVersion === 4) {
  await copyPolicy(policy04Loader, 'policy.lua.d/85-pipetune.lua');
  await copyPolicy(policy04Script, 'scripts/pipetune/policy-0.4.lua');
} else {
  await copyPolicy(policy05Config, 'wireplumber.conf.d/90-pipetune.conf');
  await copyPolicy(policy05Script, 'scripts/pipetune/policy-0.5.lua');
}

const environment = {
  ...process.env,
  XDG_RUNTIME_DIR: runtimeDirectory,
  PIPEWIRE_RUNTIME_DIR: runtimeDirectory,
  XDG_CONFIG_HOME: configDirectory,
  WIREPLUMBER_DEBUG: '2',
};
delete environment.PIPEWIRE_REMOTE;

const children = [];
const start = (command, commandArguments) => {
  const child = spawn(command, commandArguments, {
    env: environment,
    stdio: ['ignore', 'ignore', 'pipe'],
  });
  children.push(child);
  return child;
};

const waitFor = async (predicate, timeoutMilliseconds) => {
  const deadline = Date.now() + timeoutMilliseconds;
  while (Date.now() < deadline) {
    if (await predicate()) return true;
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  return false;
};

const stopChildren = async () => {
  for (const child of children.toReversed()) {
    if (child.exitCode !== null || child.signalCode !== null) continue;
    child.kill('SIGTERM');
    await new Promise(resolve => child.once('exit', resolve));
  }
};

try {
  const pipewire = start('pipewire', []);
  let pipewireDiagnostic = '';
  pipewire.stderr.on('data', chunk => {
    pipewireDiagnostic += chunk;
  });
  assert.equal(
    await waitFor(() => fs.access(path.join(runtimeDirectory, 'pipewire-0')).then(() => true, () => false), 5000),
    true,
    `isolated PipeWire did not create its socket: ${pipewireDiagnostic}`,
  );

  const wireplumberArguments =
    minorVersion === 4
      ? ['--config-file', 'policy.conf']
      : ['--profile', 'policy'];
  const wireplumber = start('wireplumber', wireplumberArguments);
  let wireplumberDiagnostic = '';
  wireplumber.stderr.on('data', chunk => {
    wireplumberDiagnostic += chunk;
  });

  let metadataOutput = '';
  const metadataReady = await waitFor(() => {
    const result = spawnSync('pw-metadata', ['-n', 'pipetune-policy'], {
      env: environment,
      encoding: 'utf8',
    });
    metadataOutput = `${result.stdout ?? ''}\n${result.stderr ?? ''}`;
    return result.status === 0 && metadataOutput.includes('protocol.version');
  }, 10000);
  assert.equal(
    metadataReady,
    true,
    `WirePlumber did not publish the PipeTune policy handshake: ${wireplumberDiagnostic}`,
  );
  assert.match(metadataOutput, /protocol\.version[^\n]*1/);
  assert.match(metadataOutput, new RegExp(`wireplumber-0\\.${minorVersion}`));
} finally {
  await stopChildren();
  await fs.rm(temporaryRoot, { recursive: true, force: true });
}
