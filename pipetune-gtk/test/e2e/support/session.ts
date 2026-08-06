import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { once } from 'node:events';
import {
  mkdir,
  mkdtemp,
  readFile,
  rm,
  stat,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkAppEnvironment,
  type GtkAppLauncher,
} from 'gestament';
import { waitForResult } from 'gestament/testing';

interface FakeDaemon {
  readonly process: ChildProcessWithoutNullStreams;
  readonly stderr: () => string;
}

/**
 * One JSON control request captured by the deterministic daemon.
 */
export interface FakeControlRequest {
  /** Stable command field from the production control protocol. */
  readonly command: string;
  /** Additional command-specific protocol fields. */
  readonly [key: string]: unknown;
}

/**
 * Machine-readable startup configuration loaded by production code.
 */
export interface StartupConfigSnapshot {
  /** Absolute preset path, or null for bypass. */
  readonly preset: string | null;
  /** Automatic graph-following or fixed sample-rate mode. */
  readonly rateMode: string;
  /** Fixed rate, or zero in automatic mode. */
  readonly fixedRate: number;
  /** Suggest or force graph-rate enforcement. */
  readonly rateEnforcement: string;
  /** Scalar or SIMD backend. */
  readonly dspBackend: string;
  /** Automatic or pinned SIMD variant. */
  readonly dspSimdVariant: string;
}

/**
 * Options for one isolated PipeTune GTK test session.
 */
export interface PipeTuneGtkTestOptions {
  /** Fake-daemon command name to reject, or undefined to accept all commands. */
  readonly rejectedCommand: string | undefined;
}

/**
 * Holds one isolated fake daemon and GTK application session.
 */
export interface PipeTuneGtkTestSession {
  /** Launched GTK application. */
  readonly app: GtkApp;
  /** Reusable gestament launcher for diagnostics and cleanup. */
  readonly launcher: GtkAppLauncher;
  /** Isolated filesystem root. */
  readonly root: string;
  /** Absolute startup configuration path used by the application. */
  readonly configPath: string;
  /** Absolute dedicated PipeTune GTK language preference path. */
  readonly languageConfigPath: string;
  /** Reads all daemon requests captured since the last clear. */
  readonly readRequests: () => Promise<readonly FakeControlRequest[]>;
  /** Truncates the deterministic daemon request history. */
  readonly clearRequests: () => Promise<void>;
  /** Loads startup settings through the production configuration parser. */
  readonly inspectConfig: () => Promise<StartupConfigSnapshot>;
  /** Makes atomic configuration replacement succeed or fail. */
  readonly setConfigDirectoryWritable: (writable: boolean) => Promise<void>;
  /** Replaces the language preference path with a directory to reject saves. */
  readonly blockLanguagePreferenceSave: () => Promise<void>;
  /** Restarts only the GTK application in the same isolated session. */
  readonly restartApplication: () => Promise<void>;
  /** Stops the fake daemon while leaving the GTK application running. */
  readonly disconnectDaemon: () => Promise<void>;
  /** Starts a fresh fake daemon from the current persisted configuration. */
  readonly reconnectDaemon: () => Promise<void>;
  /** Stops all processes and removes the isolated filesystem root. */
  readonly release: () => Promise<void>;
}

const requiredEnvironment = (name: string): string => {
  const value = process.env[name];
  if (value === undefined || value === '') {
    throw new Error(`${name} is required`);
  }
  return value;
};

const startFakeDaemon = async (
  environment: GtkAppEnvironment,
  runtimeDirectory: string,
  requestLogPath: string,
  rejectedCommand: string | undefined
): Promise<FakeDaemon> => {
  const executable = requiredEnvironment('PIPETUNE_GTK_E2E_DAEMON');
  const child = spawn(executable, [], {
    env: {
      ...environment,
      PIPETUNE_E2E_REQUEST_LOG: requestLogPath,
      PIPETUNE_E2E_REJECT_COMMAND: rejectedCommand,
    },
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  let stderr = '';
  let spawnError = '';
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', (chunk: string) => {
    stderr += chunk;
  });
  child.once('error', (error) => {
    spawnError = error.message;
  });
  await waitForResult(
    async () => {
      if (spawnError !== '') {
        throw new Error(`fake daemon could not start: ${spawnError}`);
      }
      if (child.exitCode !== null) {
        throw new Error(`fake daemon exited with ${child.exitCode}: ${stderr}`);
      }
      await stat(join(runtimeDirectory, 'pipetune', 'control.sock'));
      return true;
    },
    {
      timeoutMs: 10_000,
      message: 'Fake PipeTune control socket did not become ready.',
    }
  );
  return { process: child, stderr: () => stderr };
};

const stopFakeDaemon = async (daemon: FakeDaemon): Promise<void> => {
  if (daemon.process.exitCode !== null) {
    return;
  }
  daemon.process.stdin.end();
  await once(daemon.process, 'exit');
};

const parseRequests = (contents: string): readonly FakeControlRequest[] =>
  contents
    .split('\n')
    .filter((line) => line !== '')
    .map((line) => JSON.parse(line) as FakeControlRequest);

const inspectStartupConfig = async (
  configPath: string
): Promise<StartupConfigSnapshot> => {
  const executable = requiredEnvironment('PIPETUNE_GTK_E2E_DAEMON');
  const child = spawn(executable, ['--inspect-config', configPath], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stdout = '';
  let stderr = '';
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk: string) => {
    stdout += chunk;
  });
  child.stderr.on('data', (chunk: string) => {
    stderr += chunk;
  });
  const [exitCode] = (await once(child, 'exit')) as [number | null];
  if (exitCode !== 0) {
    throw new Error(`configuration inspection failed: ${stderr}`);
  }
  return JSON.parse(stdout) as StartupConfigSnapshot;
};

/**
 * Starts one isolated gestament session backed by the deterministic daemon.
 *
 * @param options Per-session fake-daemon behavior.
 * @returns A connected GTK session with a visible main window.
 */
export const launchPipeTuneGtk = async (
  options: PipeTuneGtkTestOptions = { rejectedCommand: undefined }
): Promise<PipeTuneGtkTestSession> => {
  const root = await mkdtemp(join(tmpdir(), 'pipetune-gtk-e2e-'));
  const runtimeDirectory = join(root, 'runtime');
  const configDirectory = join(root, 'config');
  const pipeTuneConfigDirectory = join(configDirectory, 'pipetune');
  const configPath = join(pipeTuneConfigDirectory, 'environment');
  const languageConfigPath = join(pipeTuneConfigDirectory, 'gtk.conf');
  const requestLogPath = join(root, 'requests.jsonl');
  const persistenceGuardPath = join(root, 'persistence-guard');
  const homeDirectory = join(root, 'home');
  await mkdir(runtimeDirectory, { mode: 0o700 });
  await mkdir(pipeTuneConfigDirectory, { recursive: true });
  await mkdir(homeDirectory);
  await writeFile(requestLogPath, '', { mode: 0o600 });
  await writeFile(persistenceGuardPath, 'allow\n', { mode: 0o600 });
  await writeFile(
    configPath,
    [
      '# Managed by PipeTune.',
      'PIPETUNE_PRESET="/tmp/e2e.effetune_preset"',
      'PIPETUNE_DSP_BACKEND=simd',
      'PIPETUNE_DSP_SIMD_VARIANT=x86-64-v3',
      'PIPETUNE_RATE=192000',
      'PIPETUNE_RATE_ENFORCEMENT=force',
      '',
    ].join('\n'),
    { mode: 0o600 }
  );

  const launcher = createGtkAppLauncher({
    appPath: requiredEnvironment('PIPETUNE_GTK_BINARY'),
    display: 'xvfb',
    accessibilitySession: 'minimal',
    xvfbScreen: '1280x800x24',
    xvfbTrayHost: true,
    gsettings: 'memory',
    theme: 'Adwaita',
    timeoutMs: 10_000,
    env: {
      XDG_RUNTIME_DIR: runtimeDirectory,
      XDG_CONFIG_HOME: configDirectory,
      HOME: homeDirectory,
      PIPETUNE_GTK_E2E_PERSISTENCE_GUARD: persistenceGuardPath,
    },
  });
  let daemon: FakeDaemon | undefined;
  let app: GtkApp | undefined;
  try {
    const environment = await launcher.environment();
    daemon = await startFakeDaemon(
      environment,
      runtimeDirectory,
      requestLogPath,
      options.rejectedCommand
    );
    app = await launcher.launch();
  } catch (error) {
    if (daemon !== undefined) {
      await stopFakeDaemon(daemon);
    }
    await launcher.release();
    await rm(root, { recursive: true, force: true });
    throw error;
  }

  const readRequests = async (): Promise<readonly FakeControlRequest[]> =>
    parseRequests(await readFile(requestLogPath, 'utf8'));
  const clearRequests = async (): Promise<void> => {
    await writeFile(requestLogPath, '', { mode: 0o600 });
  };
  const setConfigDirectoryWritable = async (
    writable: boolean
  ): Promise<void> => {
    await writeFile(persistenceGuardPath, writable ? 'allow\n' : 'deny\n', {
      mode: 0o600,
    });
  };
  const blockLanguagePreferenceSave = async (): Promise<void> => {
    await mkdir(languageConfigPath);
  };
  const restartApplication = async (): Promise<void> => {
    if (app === undefined) {
      throw new Error('GTK application is unavailable');
    }
    await app.release();
    app = await launcher.launch();
  };
  const disconnectDaemon = async (): Promise<void> => {
    if (daemon === undefined) {
      return;
    }
    await stopFakeDaemon(daemon);
    daemon = undefined;
  };
  const reconnectDaemon = async (): Promise<void> => {
    if (daemon !== undefined) {
      throw new Error('fake daemon is already running');
    }
    const environment = await launcher.environment();
    daemon = await startFakeDaemon(
      environment,
      runtimeDirectory,
      requestLogPath,
      options.rejectedCommand
    );
  };
  const release = async (): Promise<void> => {
    await launcher.release();
    const activeDaemon = daemon;
    if (activeDaemon !== undefined) {
      await stopFakeDaemon(activeDaemon);
      daemon = undefined;
    }
    const daemonError = activeDaemon === undefined ? '' : activeDaemon.stderr();
    await rm(root, { recursive: true, force: true });
    if (daemonError !== '') {
      throw new Error(`fake daemon stderr was not empty: ${daemonError}`);
    }
  };
  return {
    get app() {
      if (app === undefined) {
        throw new Error('GTK application is unavailable');
      }
      return app;
    },
    launcher,
    root,
    configPath,
    languageConfigPath,
    readRequests,
    clearRequests,
    inspectConfig: async () => inspectStartupConfig(configPath),
    setConfigDirectoryWritable,
    blockLanguagePreferenceSave,
    restartApplication,
    disconnectDaemon,
    reconnectDaemon,
    release,
  };
};
