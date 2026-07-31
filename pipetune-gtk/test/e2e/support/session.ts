import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, rm, stat, writeFile } from 'node:fs/promises';
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
 * Holds one isolated fake daemon and GTK application session.
 */
export interface PipeTuneGtkTestSession {
  /** Launched GTK application. */
  readonly app: GtkApp;
  /** Reusable gestament launcher for diagnostics and cleanup. */
  readonly launcher: GtkAppLauncher;
  /** Isolated filesystem root. */
  readonly root: string;
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
  runtimeDirectory: string
): Promise<FakeDaemon> => {
  const executable = requiredEnvironment('PIPETUNE_GTK_E2E_DAEMON');
  const child = spawn(executable, [], {
    env: environment,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  let stderr = '';
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', (chunk: string) => {
    stderr += chunk;
  });
  await waitForResult(
    async () => {
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

/**
 * Starts one isolated gestament session backed by the deterministic daemon.
 *
 * @returns A connected GTK session with a visible main window.
 */
export const launchPipeTuneGtk = async (): Promise<PipeTuneGtkTestSession> => {
  const root = await mkdtemp(join(tmpdir(), 'pipetune-gtk-e2e-'));
  const runtimeDirectory = join(root, 'runtime');
  const configDirectory = join(root, 'config');
  const homeDirectory = join(root, 'home');
  await mkdir(runtimeDirectory, { mode: 0o700 });
  await mkdir(join(configDirectory, 'pipetune'), { recursive: true });
  await mkdir(homeDirectory);
  await writeFile(
    join(configDirectory, 'pipetune', 'environment'),
    [
      '# Managed by PipeTune.',
      'PIPETUNE_PRESET="/tmp/e2e.effetune_preset"',
      'PIPETUNE_TARGET="alsa_output.usb-long-studio-dac.analog-stereo"',
      'PIPETUNE_DSP_BACKEND=simd',
      'PIPETUNE_DSP_SIMD_VARIANT=x86-64-v3',
      'PIPETUNE_DSP_IDLE_POLICY=exact',
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
    },
  });
  let daemon: FakeDaemon | undefined;
  let app: GtkApp | undefined;
  try {
    const environment = await launcher.environment();
    daemon = await startFakeDaemon(environment, runtimeDirectory);
    app = await launcher.launch();
  } catch (error) {
    if (daemon !== undefined) {
      await stopFakeDaemon(daemon);
    }
    await launcher.release();
    await rm(root, { recursive: true, force: true });
    throw error;
  }

  const release = async (): Promise<void> => {
    await launcher.release();
    await stopFakeDaemon(daemon);
    const daemonError = daemon.stderr();
    await rm(root, { recursive: true, force: true });
    if (daemonError !== '') {
      throw new Error(`fake daemon stderr was not empty: ${daemonError}`);
    }
  };
  return { app, launcher, root, release };
};
