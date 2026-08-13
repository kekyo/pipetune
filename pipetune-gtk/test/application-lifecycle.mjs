import { execFile, spawn } from 'node:child_process';
import { mkdir, mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const executable = process.argv[2];
const pipeTuneExecutable = process.argv[3];
const setupHelper = process.argv[4];
const gdbus = process.argv[5];

if (
  executable === undefined ||
  pipeTuneExecutable === undefined ||
  setupHelper === undefined ||
  gdbus === undefined
) {
  throw new Error(
    'PipeTune GTK, CLI, setup helper, and gdbus executable paths are required'
  );
}

const root = await mkdtemp(join(tmpdir(), 'pipetune-gtk-lifecycle-'));
const configDirectory = join(root, 'config');
const dataDirectory = join(root, 'data');
const stateDirectory = join(root, 'state');
const homeDirectory = join(root, 'home');
const setupRecordPath = join(root, 'setup-invocations');
await Promise.all([
  mkdir(configDirectory),
  mkdir(dataDirectory),
  mkdir(stateDirectory),
  mkdir(homeDirectory),
]);
const environment = {
  ...process.env,
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
    timeout: 15000,
    killSignal: 'SIGKILL',
  });

const pipeTuneVersion = await execute(pipeTuneExecutable, ['--version']);
const versionMatch = pipeTuneVersion.stdout.match(
  /^PipeTune ([^,\n]+), EffeTune DSP ([^\n]+)\n$/u
);
if (pipeTuneVersion.stderr !== '' || versionMatch === null) {
  throw new Error(
    `PipeTune CLI version differs: stdout=${pipeTuneVersion.stdout}; stderr=${pipeTuneVersion.stderr}`
  );
}
const expectedVersionText =
  `PipeTune GTK ${versionMatch[1]}, EffeTune DSP ${versionMatch[2]}`;
const version = await execute(executable, ['--version']);
if (version.stderr !== '' || version.stdout !== `${expectedVersionText}\n`) {
  throw new Error(
    `PipeTune GTK version differs: stdout=${version.stdout}; stderr=${version.stderr}`
  );
}

const application = spawn(executable, ['--hidden'], {
  env: environment,
  stdio: ['ignore', 'ignore', 'pipe'],
});
let stderr = '';
let applicationExit = undefined;
application.stderr.setEncoding('utf8');
application.stderr.on('data', (chunk) => {
  stderr += chunk;
});
application.on('exit', (code, signal) => {
  applicationExit = `${code ?? 'null'}/${signal ?? 'none'}`;
});

const mainWindowPattern = new RegExp(
  '^\\s+(0x[0-9a-f]+) "PipeTune":.*?\\s(\\d+)x(\\d+)[+-]',
  'gmu'
);
const mainWindowIds = (tree) =>
  [...tree.matchAll(mainWindowPattern)]
    .filter((match) => Number(match[2]) >= 100 && Number(match[3]) >= 100)
    .map((match) => match[1]);

const windowTree = async () => {
  const { stdout } = await execute('xwininfo', ['-root', '-tree']);
  return stdout;
};

const waitForWindow = async () => {
  const deadline = Date.now() + 5000;
  let lastTree = '';
  while (Date.now() < deadline) {
    lastTree = await windowTree();
    if (mainWindowIds(lastTree).length === 1) {
      return lastTree;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(
    `activated window did not appear; exit=${applicationExit}; stderr=${stderr}; tree=${lastTree}`
  );
};

const verifyWindowStaysHidden = async () => {
  const deadline = Date.now() + 1000;
  let lastTree = '';
  while (Date.now() < deadline) {
    lastTree = await windowTree();
    if (mainWindowIds(lastTree).length !== 0) {
      throw new Error(
        `hidden startup displayed the main window; exit=${applicationExit}; stderr=${stderr}; tree=${lastTree}`
      );
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
};

const waitForExit = async () => {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline && applicationExit === undefined) {
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  if (applicationExit === undefined) {
    throw new Error('remote --quit did not terminate the primary instance');
  }
};

const readSetupInvocations = async () => {
  try {
    return (await readFile(setupRecordPath, 'utf8'))
      .split('\n')
      .filter((line) => line !== '');
  } catch (error) {
    if (error.code === 'ENOENT') {
      return [];
    }
    throw error;
  }
};

const waitForInitialSetup = async () => {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const invocations = await readSetupInvocations();
    if (invocations.length === 1) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error('primary instance did not run per-user setup');
};

try {
  await execute(gdbus, [
    'wait',
    '--session',
    '--timeout',
    '10',
    'net.kekyo.pipetune_gtk',
  ]);
  await waitForInitialSetup();

  const hiddenActivation = await execute(executable, ['--hidden']);
  if (hiddenActivation.stderr !== '') {
    throw new Error(
      `secondary hidden activation failed: ${hiddenActivation.stderr}`
    );
  }
  await verifyWindowStaysHidden();

  const activation = await execute(executable, []);
  if (activation.stderr !== '') {
    throw new Error(`secondary activation failed: ${activation.stderr}`);
  }
  const activatedTree = await waitForWindow();
  const repeatedActivation = await execute(executable, []);
  if (repeatedActivation.stderr !== '') {
    throw new Error(
      `repeated secondary activation failed: ${repeatedActivation.stderr}`
    );
  }
  const repeatedTree = await waitForWindow();
  const activatedWindows = mainWindowIds(activatedTree);
  const repeatedWindows = mainWindowIds(repeatedTree);
  if (
    activatedWindows.length !== 1 ||
    repeatedWindows.length !== 1 ||
    activatedWindows[0] !== repeatedWindows[0]
  ) {
    throw new Error(
      `single-instance window identity differs: ${activatedWindows.join(',')}/${repeatedWindows.join(',')}`
    );
  }
  const setupInvocations = await readSetupInvocations();
  if (
    setupInvocations.length !== 1 ||
    setupInvocations[0] !== 'setup --no-launch-gtk'
  ) {
    throw new Error(
      `per-user setup must run once in the primary instance: ${setupInvocations.join(',')}`
    );
  }
  const quit = await execute(executable, ['--quit']);
  if (quit.stderr !== '') {
    throw new Error(`remote quit failed: ${quit.stderr}`);
  }
  await waitForExit();
  await execute(executable, ['--quit']);
} finally {
  if (applicationExit === undefined) {
    application.kill('SIGTERM');
    await new Promise((resolve) => {
      const timeout = setTimeout(resolve, 2000);
      application.once('exit', () => {
        clearTimeout(timeout);
        resolve();
      });
    });
    if (applicationExit === undefined) {
      application.kill('SIGKILL');
    }
  }
  await rm(root, { recursive: true, force: true });
}
