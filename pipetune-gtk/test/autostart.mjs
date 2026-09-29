import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { createInterface } from 'node:readline';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const [
  executable,
  setupHelper,
  watcherExecutable,
  cmake,
  buildDirectory,
  autostartDirectory,
] = process.argv.slice(2);
if (
  executable === undefined ||
  setupHelper === undefined ||
  watcherExecutable === undefined ||
  cmake === undefined ||
  buildDirectory === undefined ||
  autostartDirectory === undefined
) {
  throw new Error('PipeTune GTK autostart test arguments are required');
}

const root = await mkdtemp(join(tmpdir(), 'pipetune-gtk-autostart-'));
const stage = join(root, 'stage');
const environment = {
  ...process.env,
  PATH: `${dirname(executable)}:${process.env.PATH ?? ''}`,
  HOME: join(root, 'home'),
  XDG_CONFIG_HOME: join(root, 'config'),
  XDG_DATA_HOME: join(root, 'data'),
  XDG_STATE_HOME: join(root, 'state'),
  PIPETUNE_GTK_E2E_PIPETUNE_EXECUTABLE: setupHelper,
  PIPETUNE_GTK_SETUP_HELPER_RECORD: join(root, 'setup-invocations'),
};
const execute = async (program, args, env = environment) =>
  await execFileAsync(program, args, {
    encoding: 'utf8',
    env,
    timeout: 15000,
    killSignal: 'SIGKILL',
  });
const busName = 'net.kekyo.pipetune_gtk';
const busCall = async (method, args) => {
  const result = await execute('gdbus', [
    'call',
    '--session',
    '--dest',
    'org.freedesktop.DBus',
    '--object-path',
    '/org/freedesktop/DBus',
    '--method',
    `org.freedesktop.DBus.${method}`,
    ...args,
  ]);
  return result.stdout;
};
const processId = async () => {
  const result = await busCall('GetConnectionUnixProcessID', [busName]);
  const value = result.match(/\buint32 (\d+)\b/u)?.[1];
  if (value === undefined) {
    throw new Error(`Cannot parse PipeTune GTK process ID: ${result}`);
  }
  return Number(value);
};
const waitForExit = async () => {
  const deadline = Date.now() + 5000;
  do {
    const result = await busCall('NameHasOwner', [busName]);
    if (result.includes('false')) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  } while (Date.now() < deadline);
  throw new Error('PipeTune GTK did not exit after --quit');
};
const windowIds = async () => {
  const result = await execute('xwininfo', ['-root', '-tree']);
  const pattern = /^\s+(0x[0-9a-f]+) "PipeTune":.*?\s(\d+)x(\d+)[+-]/gmu;
  return [...result.stdout.matchAll(pattern)]
    .filter((match) => Number(match[2]) >= 100 && Number(match[3]) >= 100)
    .map((match) => match[1]);
};
const expectWindowCount = async (expected) => {
  const deadline = Date.now() + 5000;
  do {
    const windows = await windowIds();
    if (windows.length === expected) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  } while (Date.now() < deadline);
  throw new Error(`Expected ${expected} PipeTune window(s)`);
};
const expectHidden = async () => {
  const deadline = Date.now() + 1000;
  do {
    const windows = await windowIds();
    if (windows.length !== 0) {
      throw new Error(`Autostart displayed PipeTune windows: ${windows}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  } while (Date.now() < deadline);
};

let launchedProcessId;
let watcher;
let watcherExited;
try {
  await Promise.all([
    mkdir(stage),
    mkdir(environment.HOME),
    mkdir(environment.XDG_CONFIG_HOME),
    mkdir(environment.XDG_DATA_HOME),
    mkdir(environment.XDG_STATE_HOME),
  ]);
  await execute(cmake, ['--install', buildDirectory], {
    ...environment,
    DESTDIR: stage,
  });
  const desktopFile = join(
    stage,
    autostartDirectory.replace(/^\/+/, ''),
    'net.kekyo.pipetune_gtk.desktop'
  );
  await execute('desktop-file-validate', [desktopFile]);
  await execute('gio', ['launch', desktopFile]);
  await execute('gdbus', ['wait', '--session', '--timeout', '10', busName]);
  launchedProcessId = await processId();
  await expectHidden();

  await execute('gio', ['launch', desktopFile]);
  if ((await processId()) !== launchedProcessId) {
    throw new Error('Autostart created a second PipeTune GTK process');
  }
  await expectHidden();

  await execute(executable, []);
  await expectWindowCount(1);
  if ((await processId()) !== launchedProcessId) {
    throw new Error('Normal launch created a second PipeTune GTK process');
  }
  const setupInvocations = (await readFile(
    environment.PIPETUNE_GTK_SETUP_HELPER_RECORD,
    'utf8'
  ))
    .split('\n')
    .filter((line) => line !== '');
  if (
    setupInvocations.length !== 1 ||
    setupInvocations[0] !== 'setup --no-launch-gtk'
  ) {
    throw new Error(`Per-user setup ran incorrectly: ${setupInvocations}`);
  }
  await execute(executable, ['--quit']);
  await waitForExit();
  launchedProcessId = undefined;

  watcher = spawn(watcherExecutable, ['--serve'], {
    env: environment,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  watcherExited = once(watcher, 'exit');
  const watcherLines = createInterface({ input: watcher.stdout })[
    Symbol.asyncIterator
  ]();
  let watcherError = '';
  watcher.stderr.setEncoding('utf8');
  watcher.stderr.on('data', (chunk) => {
    watcherError += chunk;
  });
  const expectWatcherLine = async (expected) => {
    const line = await watcherLines.next();
    if (line.done || line.value !== expected) {
      throw new Error(
        `Watcher sent ${String(line.value)} instead of ${expected}: ${watcherError}`
      );
    }
  };
  await expectWatcherLine('ready');
  await execute('gio', ['launch', desktopFile]);
  await execute('gdbus', ['wait', '--session', '--timeout', '10', busName]);
  launchedProcessId = await processId();
  await expectWatcherLine('registered');
  await expectHidden();

  watcher.stdin.write('quit\n');
  const [watcherCode] = await watcherExited;
  watcher = undefined;
  if (watcherCode !== 0) {
    throw new Error(`Watcher exited with ${watcherCode}: ${watcherError}`);
  }
  await expectWindowCount(1);
  await execute(executable, ['--quit']);
  await waitForExit();
  launchedProcessId = undefined;
} finally {
  if (watcher !== undefined) {
    watcher.kill('SIGTERM');
    await watcherExited;
  }
  if (launchedProcessId !== undefined) {
    try {
      process.kill(launchedProcessId, 'SIGTERM');
    } catch (error) {
      if (error.code !== 'ESRCH') {
        throw error;
      }
    }
  }
  await rm(root, { recursive: true, force: true });
}
