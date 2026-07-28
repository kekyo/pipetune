import { execFile, spawn } from 'node:child_process';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const executable = process.argv[2];
const pipeTuneExecutable = process.argv[3];

if (executable === undefined || pipeTuneExecutable === undefined) {
  throw new Error('PipeTune GTK and CLI executable paths are required');
}

const pipeTuneVersion = await execFileAsync(
  pipeTuneExecutable,
  ['--version'],
  { encoding: 'utf8' }
);
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
const version = await execFileAsync(executable, ['--version'], {
  encoding: 'utf8',
});
if (version.stderr !== '' || version.stdout !== `${expectedVersionText}\n`) {
  throw new Error(
    `PipeTune GTK version differs: stdout=${version.stdout}; stderr=${version.stderr}`
  );
}

const application = spawn(executable, ['--hidden'], {
  stdio: ['ignore', 'pipe', 'pipe'],
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

const escapedVersionText = expectedVersionText.replace(
  /[.*+?^${}()|[\]\\]/gu,
  '\\$&'
);
const mainWindowPattern = new RegExp(
  `^\\s+(0x[0-9a-f]+) "${escapedVersionText}":.*?\\s(\\d+)x(\\d+)[+-]`,
  'gmu'
);
const mainWindowIds = (tree) =>
  [...tree.matchAll(mainWindowPattern)]
    .filter((match) => Number(match[2]) >= 100 && Number(match[3]) >= 100)
    .map((match) => match[1]);

const windowTree = async () => {
  const { stdout } = await execFileAsync('xwininfo', ['-root', '-tree'], {
    encoding: 'utf8',
  });
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

try {
  const hiddenActivation = await execFileAsync(executable, ['--hidden'], {
    encoding: 'utf8',
  });
  if (hiddenActivation.stderr !== '') {
    throw new Error(
      `secondary hidden activation failed: ${hiddenActivation.stderr}`
    );
  }
  await verifyWindowStaysHidden();

  const activation = await execFileAsync(executable, [], {
    encoding: 'utf8',
  });
  if (activation.stderr !== '') {
    throw new Error(`secondary activation failed: ${activation.stderr}`);
  }
  const activatedTree = await waitForWindow();
  const repeatedActivation = await execFileAsync(executable, [], {
    encoding: 'utf8',
  });
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
  const quit = await execFileAsync(executable, ['--quit'], {
    encoding: 'utf8',
  });
  if (quit.stderr !== '') {
    throw new Error(`remote quit failed: ${quit.stderr}`);
  }
  await waitForExit();
  await execFileAsync(executable, ['--quit'], {
    encoding: 'utf8',
  });
} finally {
  if (applicationExit === undefined) {
    application.kill('SIGTERM');
    await new Promise((resolve) => {
      application.once('exit', resolve);
      setTimeout(resolve, 2000);
    });
  }
}
