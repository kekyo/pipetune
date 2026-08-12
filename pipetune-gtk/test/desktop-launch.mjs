import { execFile } from 'node:child_process';
import {
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, dirname, join } from 'node:path';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const executable = process.argv[2];
const gtkLaunch = process.argv[3];
const desktopFile = process.argv[4];
const setupHelper = process.argv[5];
const gdbus = process.argv[6];

if (
  executable === undefined ||
  gtkLaunch === undefined ||
  desktopFile === undefined ||
  setupHelper === undefined ||
  gdbus === undefined
) {
  throw new Error(
    'PipeTune GTK, gtk-launch, desktop entry, setup helper, and gdbus paths are required'
  );
}

const root = await mkdtemp(join(tmpdir(), 'pipetune-gtk-desktop-launch-'));
const dataDirectory = join(root, 'data');
const configDirectory = join(root, 'config');
const stateDirectory = join(root, 'state');
const homeDirectory = join(root, 'home');
const applicationDirectory = join(dataDirectory, 'applications');
const setupRecordPath = join(root, 'setup-invocations');
const desktopFileName = basename(desktopFile);
const desktopId = desktopFileName.slice(0, -'.desktop'.length);
const environment = {
  ...process.env,
  PATH: `${dirname(executable)}:${process.env.PATH ?? ''}`,
  HOME: homeDirectory,
  XDG_CONFIG_HOME: configDirectory,
  XDG_DATA_HOME: dataDirectory,
  XDG_STATE_HOME: stateDirectory,
  PIPETUNE_GTK_E2E_PIPETUNE_EXECUTABLE: setupHelper,
  PIPETUNE_GTK_SETUP_HELPER_RECORD: setupRecordPath,
};

const execute = async (program, commandArguments) =>
  await execFileAsync(program, commandArguments, {
    encoding: 'utf8',
    env: environment,
    timeout: 15_000,
    killSignal: 'SIGKILL',
  });

const mainWindowPattern = new RegExp(
  '^\\s+(0x[0-9a-f]+) "PipeTune":.*?\\s(\\d+)x(\\d+)[+-]',
  'gmu'
);
const mainWindowIds = (tree) =>
  [...tree.matchAll(mainWindowPattern)]
    .filter((match) => Number(match[2]) >= 100 && Number(match[3]) >= 100)
    .map((match) => match[1]);

const waitForWindow = async () => {
  const deadline = Date.now() + 5_000;
  let lastTree = '';
  while (Date.now() < deadline) {
    const tree = await execute('xwininfo', ['-root', '-tree']);
    lastTree = tree.stdout;
    if (mainWindowIds(lastTree).length === 1) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`desktop launcher did not show PipeTune: ${lastTree}`);
};

const applicationHasOwner = async () => {
  const reply = await execute(gdbus, [
    'call',
    '--session',
    '--dest',
    'org.freedesktop.DBus',
    '--object-path',
    '/org/freedesktop/DBus',
    '--method',
    'org.freedesktop.DBus.NameHasOwner',
    desktopId,
  ]);
  return reply.stdout.includes('true');
};

const waitForExit = async () => {
  const deadline = Date.now() + 5_000;
  while (Date.now() < deadline) {
    if (!(await applicationHasOwner())) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error('desktop-launched PipeTune did not exit');
};

const waitForSetup = async () => {
  const deadline = Date.now() + 5_000;
  while (Date.now() < deadline) {
    try {
      const invocations = (await readFile(setupRecordPath, 'utf8'))
        .split('\n')
        .filter((line) => line !== '');
      if (
        invocations.length === 1 &&
        invocations[0] === 'setup --no-launch-gtk'
      ) {
        return;
      }
    } catch (error) {
      if (error.code !== 'ENOENT') {
        throw error;
      }
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error('desktop-launched PipeTune did not run per-user setup');
};

await Promise.all([
  mkdir(applicationDirectory, { recursive: true }),
  mkdir(configDirectory),
  mkdir(stateDirectory),
  mkdir(homeDirectory),
]);
await copyFile(desktopFile, join(applicationDirectory, desktopFileName));
try {
  const launched = await execute(gtkLaunch, [desktopId]);
  if (launched.stderr.includes('g_dbus_connection_call_sync_internal')) {
    throw new Error(`gtk-launch used an invalid object path: ${launched.stderr}`);
  }
  await execute(gdbus, [
    'wait',
    '--session',
    '--timeout',
    '10',
    desktopId,
  ]);
  await waitForWindow();
  await waitForSetup();
  const quit = await execute(executable, ['--quit']);
  if (quit.stderr !== '') {
    throw new Error(`remote quit failed: ${quit.stderr}`);
  }
  await waitForExit();
} finally {
  try {
    if (await applicationHasOwner()) {
      await execute(executable, ['--quit']);
    }
  } catch (error) {
    process.stderr.write(`desktop launch cleanup failed: ${error.message}\n`);
  }
  await rm(root, { recursive: true, force: true });
}
