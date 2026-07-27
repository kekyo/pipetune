import { execFile, spawn } from 'node:child_process';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const executable = process.argv[2];

if (executable === undefined) {
  throw new Error('pipetune-gtk executable path is required');
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

const mainWindowIds = (tree) =>
  [...tree.matchAll(/^\s+(0x[0-9a-f]+) "PipeTune":.*?\s(\d+)x(\d+)[+-]/gmu)]
    .filter((match) => Number(match[2]) >= 100 && Number(match[3]) >= 100)
    .map((match) => match[1]);

const waitForWindow = async () => {
  const deadline = Date.now() + 5000;
  let lastTree = '';
  while (Date.now() < deadline) {
    const { stdout } = await execFileAsync('xwininfo', ['-root', '-tree'], {
      encoding: 'utf8',
    });
    lastTree = stdout;
    if (mainWindowIds(stdout).length === 1) {
      return stdout;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(
    `hidden fallback window did not appear; exit=${applicationExit}; stderr=${stderr}; tree=${lastTree}`
  );
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
  const initialTree = await waitForWindow();
  const activation = await execFileAsync(executable, [], {
    encoding: 'utf8',
  });
  if (activation.stderr !== '') {
    throw new Error(`secondary activation failed: ${activation.stderr}`);
  }
  const { stdout: activatedTree } = await execFileAsync(
    'xwininfo',
    ['-root', '-tree'],
    { encoding: 'utf8' }
  );
  const initialWindows = mainWindowIds(initialTree);
  const activatedWindows = mainWindowIds(activatedTree);
  if (
    initialWindows.length !== 1 ||
    activatedWindows.length !== 1 ||
    initialWindows[0] !== activatedWindows[0]
  ) {
    throw new Error(
      `single-instance window identity differs: ${initialWindows.join(',')}/${activatedWindows.join(',')}`
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
