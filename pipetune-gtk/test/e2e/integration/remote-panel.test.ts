import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, writeFile } from 'node:fs/promises';
import { createServer } from 'node:net';
import { join } from 'node:path';
import { promisify } from 'node:util';
import {
  createGtkAppLauncher,
  type GtkElementOfKind,
  type GtkWidgetKind,
} from 'gestament';
import { toPass } from 'gestament/testing';
import jsQR from 'jsqr';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';

const run = promisify(execFile);

it('pairs a remote editor with the running PipeTune daemon from the Japanese dialog', async () => {
  const binary = process.env.PIPETUNE_GTK_REMOTE_PANEL_DAEMON!;
  const runtime = process.env.XDG_RUNTIME_DIR!;
  const config = join(process.env.XDG_CONFIG_HOME!, 'pipetune');
  const home = await mkdtemp(join(runtime, 'home-'));
  await mkdir(config, { recursive: true });
  await writeFile(join(config, 'gtk.conf'), '[ui]\nlanguage=ja\n');
  const cli = async (...args: string[]): Promise<string> =>
    (await run(binary, args)).stdout.trim();
  const daemon = spawn(binary, ['daemon'], { stdio: 'ignore' });
  const stopped = once(daemon, 'exit');
  const launcher = createGtkAppLauncher({
    appPath: process.env.PIPETUNE_GTK_BINARY!,
    display: 'xvfb',
    accessibilitySession: 'minimal',
    xvfbScreen: '1280x1024x24',
    xvfbTrayHost: true,
    gsettings: 'memory',
    theme: 'Adwaita',
    timeoutMs: 10_000,
    env: {
      HOME: home,
      XDG_RUNTIME_DIR: runtime,
      XDG_CONFIG_HOME: process.env.XDG_CONFIG_HOME,
      XDG_DATA_HOME: process.env.XDG_DATA_HOME,
      XDG_STATE_HOME: process.env.XDG_STATE_HOME,
      PIPETUNE_GTK_E2E_PIPETUNE_EXECUTABLE:
        process.env.PIPETUNE_GTK_E2E_SETUP_HELPER,
    },
  });
  try {
    await toPass(
      async () => {
        await cli('--status');
      },
      { timeoutMs: 10_000 }
    );
    const initial = await cli('remote', 'enable', '127.0.0.1', '0');
    const app = await launcher.launch();
    const widget = async <K extends GtkWidgetKind>(
      id: string,
      kind: K
    ): Promise<GtkElementOfKind<K>> => {
      const element = await app.getById(id);
      expect(element.kind).toBe(kind);
      return element as GtkElementOfKind<K>;
    };
    const tab = await (
      await widget('settingsSwitcher', 'container')
    ).childAt(4);
    if (
      tab?.kind !== 'radio' &&
      tab?.kind !== 'toggleButton' &&
      tab?.kind !== 'button'
    ) {
      throw new Error('Missing remote control tab');
    }
    expect((await tab.info()).name).toBe('リモートコントロール');
    await tab.click();
    const enabled = await widget('remoteEnabled', 'switch');
    const tls = await widget('remoteTls', 'checkbox');
    const status = await widget('remoteStatus', 'label');
    const url = await widget('remoteUrl', 'label');
    const verifyPairing = async (expected: string): Promise<void> => {
      await toPass(async () => {
        expect(await url.text()).toBe(expected);
        const capture = await (await app.getById('remoteQr')).capture();
        expect(capture.clipped).toBe(false);
        const { data, width, height } = PNG.sync.read(capture.image);
        expect(jsQR(new Uint8ClampedArray(data), width, height)?.data).toBe(
          expected
        );
      });
      expect(await enabled.isChecked()).toBe(true);
    };
    const expectOff = async (): Promise<void> => {
      await toPass(async () => {
        expect(await enabled.isChecked()).toBe(false);
        expect(await status.text()).toBe('リモート操作はオフです。');
        expect(await url.text()).toBe('');
      });
    };
    await verifyPairing(initial);
    expect(await (await widget('remotePort', 'spinButton')).value()).toBe(
      Number(new URL(initial).port)
    );
    await (await widget('remoteCopy', 'button')).click();
    expect((await (await widget('remoteCopy', 'button')).info()).name).toBe(
      'コピーしました'
    );
    const clipboard = await run(
      process.env.PIPETUNE_GTK_CLIPBOARD_READER!,
      ['--read-clipboard'],
      {
        env: await launcher.environment(),
      }
    );
    expect(clipboard.stdout).toBe(initial);
    await toPass(async () => {
      expect((await (await widget('remoteCopy', 'button')).info()).name).toBe(
        'URLをコピー'
      );
    });

    const peer = new WebSocket(initial.replace(/^http/u, 'ws'));
    await once(peer, 'open', { signal: AbortSignal.timeout(5000) });
    await toPass(async () => {
      expect(await status.text()).toBe('待ち受け中（接続中の端末 1 台）');
    });
    const window = await widget('mainWindow', 'window');
    await mkdir('test-results', { recursive: true });
    await writeFile(
      'test-results/remote-control-panel.png',
      (await window.capture()).image
    );
    const compact = await window.resizeTo(900, 560);
    expect(compact.width).toBe(900);
    expect(compact.height).toBe(560);
    const lastTab = await (
      await widget('settingsSwitcher', 'container')
    ).childAt(5);
    const compactBounds = await window.bounds();
    await writeFile(
      'test-results/remote-control-compact.png',
      (await window.capture()).image
    );
    await toPass(async () => {
      for (const element of [
        lastTab!,
        enabled,
        await app.getById('remotePort'),
      ]) {
        const bounds = (await element.capture()).bounds;
        expect(
          bounds.x + bounds.width,
          (await element.info()).name || element.kind
        ).toBeLessThanOrEqual(compactBounds.x + compactBounds.width);
      }
    });
    await verifyPairing(initial);
    const scroll = await widget('remoteScrollbar', 'scrollbar');
    await scroll.setValue((await scroll.valueInfo()).maximum);
    await toPass(async () => {
      const page = (await (await app.getById('remotePage')).capture()).bounds;
      const buttons = (await (await widget('remoteRotate', 'button')).capture())
        .bounds;
      expect(buttons.y + buttons.height).toBeLessThanOrEqual(
        page.y + page.height
      );
    });
    await scroll.setValue(0);
    await window.resizeTo(1080, 680);
    const closed = once(peer, 'close', { signal: AbortSignal.timeout(5000) });
    await (await widget('remoteRotate', 'button')).click();
    expect((await closed)[0].code).toBe(4401);
    const rotated = await cli('remote', 'link');
    expect(rotated).not.toBe(initial);
    await verifyPairing(rotated);
    await verifyPairing(await cli('remote', 'rotate'));
    await enabled.toggle();
    await expectOff();
    await expect(cli('remote', 'link')).rejects.toThrow();

    // A rejected bind leaves the switch off and preserves the inputs for retry.
    const occupied = createServer();
    occupied.listen(Number(new URL(initial).port), '127.0.0.1');
    await once(occupied, 'listening');
    try {
      await toPass(async () => {
        expect((await enabled.info()).states).toContain('sensitive');
      });
      const bounds = (await enabled.capture()).visibleBounds;
      await app.input.moveMouseTo(
        bounds.x + Math.floor(bounds.width / 2),
        bounds.y + Math.floor(bounds.height / 2)
      );
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      await toPass(async () => {
        expect(
          (await (await widget('remoteError', 'label')).text()).length
        ).toBeGreaterThan(0);
        expect(await enabled.isChecked()).toBe(false);
      });
    } finally {
      const released = once(occupied, 'close');
      occupied.close();
      await released;
    }
    await enabled.toggle();
    await toPass(async () => {
      expect(await url.text()).toBe(await cli('remote', 'link'));
    });
    await verifyPairing(await cli('remote', 'link'));
    await cli('remote', 'disable');
    await expectOff();
    await tls.toggle();
    await toPass(async () => {
      expect((await enabled.info()).states).not.toContain('sensitive');
      expect(await status.text()).toContain('証明書と秘密鍵');
    });

    const cert = join(home, 'cert.pem');
    const key = join(home, 'key.pem');
    await run('openssl', [
      'req',
      '-x509',
      '-newkey',
      'rsa:2048',
      '-nodes',
      '-days',
      '1',
      '-subj',
      '/CN=127.0.0.1',
      '-keyout',
      key,
      '-out',
      cert,
    ]);
    const https = await cli('remote', 'enable', '127.0.0.1', '0', cert, key);
    expect(new URL(https).protocol).toBe('https:');
    await verifyPairing(https);
    expect(await tls.isChecked()).toBe(true);
    await enabled.toggle();
    await expectOff();
    await enabled.toggle();
    await toPass(async () => {
      expect(await url.text()).toMatch(/^https:\/\//u);
    });
    await verifyPairing(await cli('remote', 'link'));
    daemon.kill('SIGTERM');
    await stopped;
    await toPass(async () => {
      expect(await status.text()).toBe('PipeTuneに接続していません。');
      expect(await url.text()).toBe('');
      expect((await enabled.info()).states).not.toContain('sensitive');
    });
  } finally {
    daemon.kill('SIGTERM');
    await stopped;
    await launcher.release();
  }
});
