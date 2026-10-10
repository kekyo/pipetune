import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test('a registered original IR produces normalized wet audio without a PCM sidecar', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'pipetune-ir-'));
  try {
    const library = join(directory, 'effetune', 'ir-library');
    await mkdir(library, { recursive: true });
    const impulse = new Float32Array(512);
    impulse[0] = 1;
    impulse[64] = 0.5;
    const original = encodeWave([impulse], 48000);
    const sha256 = createHash('sha256').update(original).digest('hex');
    const irId = sha256.slice(0, 24);
    await writeFile(join(library, `${irId}.wav`), original);
    await writeFile(
      join(library, 'index.json'),
      JSON.stringify({
        version: 1,
        entries: {
          [irId]: {
            irId,
            displayName: 'Native impulse',
            composition: 'single',
            originals: [
              {
                role: 'single',
                fileName: 'impulse.wav',
                storageName: `${irId}.wav`,
                sha256,
                byteLength: original.length,
              },
            ],
            bytes: original.length,
            channels: 1,
            frames: 512,
            sampleRate: 48000,
            topology: 'mono',
            pathSummary: [],
            importedAt: '2026-10-09T00:00:00.000Z',
            analysis: {
              storageName: `${irId}.analysis`,
              onsetFrame: 0,
              rt60: null,
              peakDb: 0,
            },
          },
        },
      })
    );
    const preset = join(directory, 'wet.effetune_preset');
    await writeFile(
      preset,
      JSON.stringify({
        pipeline: [
          {
            name: 'IR Reverb',
            enabled: true,
            parameters: {
              ir: irId,
              dw: 0,
              de: false,
              pd: 0,
              lt: '128',
              cr: 'full',
              cm: 'auto',
            },
          },
        ],
      })
    );
    const driver = process.env.PIPETUNE_ASSET_DRIVER;
    expect(driver, 'the product driver must be provided by CTest').toBeTruthy();
    const result = spawnSync(driver!, [preset, '48000', '2', '1024', '0'], {
      encoding: 'utf8',
      env: {
        ...process.env,
        XDG_CONFIG_HOME: directory,
        XDG_CACHE_HOME: join(directory, 'cache'),
      },
    });
    expect(result.status, result.stderr).toBe(0);
    const [active, latency, ...audio] = result.stdout
      .trim()
      .split(/\s+/)
      .map(Number);
    expect(active, result.stderr).toBe(1);
    expect(latency).toBe(128);
    expect(audio).toHaveLength(2048);
    for (let frame = 0; frame < audio.length; frame++) {
      const expected =
        frame === 128
          ? 1 / Math.sqrt(1.25)
          : frame === 192
            ? 0.5 / Math.sqrt(1.25)
            : 0;
      expect(Math.abs(audio[frame] - expected), `sample ${frame}`).toBeLessThan(
        0.0002
      );
    }
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
