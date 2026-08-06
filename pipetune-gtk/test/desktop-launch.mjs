import { execFile } from 'node:child_process';
import { copyFile, mkdir, mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, dirname, join } from 'node:path';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const executable = process.argv[2];
const gtkLaunch = process.argv[3];
const desktopFile = process.argv[4];
const gdbus = process.argv[5];

if (
  executable === undefined ||
  gtkLaunch === undefined ||
  desktopFile === undefined ||
  gdbus === undefined
) {
  throw new Error(
    'PipeTune GTK, gtk-launch, desktop entry, and gdbus paths are required'
  );
}

const root = await mkdtemp(join(tmpdir(), 'pipetune-gtk-desktop-launch-'));
const dataDirectory = join(root, 'data');
const applicationDirectory = join(dataDirectory, 'applications');
const desktopFileName = basename(desktopFile);
const desktopId = desktopFileName.slice(0, -'.desktop'.length);
const environment = {
  ...process.env,
  PATH: `${dirname(executable)}:${process.env.PATH ?? ''}`,
  XDG_DATA_HOME: dataDirectory,
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

await mkdir(applicationDirectory, { recursive: true });
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
