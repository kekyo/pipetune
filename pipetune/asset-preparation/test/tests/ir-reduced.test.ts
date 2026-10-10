import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test.each([
  { rate: 48000, original: 48000, cr: 'auto', divider: 1, head: 128 },
  { rate: 88200, original: 44100, cr: 'auto', divider: 2, head: 128 },
  { rate: 96000, original: 48000, cr: 'auto', divider: 2, head: 128 },
  { rate: 96000, original: 48000, cr: 'half', divider: 2, head: 256 },
  { rate: 192000, original: 48000, cr: 'quarter', divider: 4, head: 128 },
  { rate: 384000, original: 96000, cr: 'quarter', divider: 4, head: 128 },
  { rate: 48000, original: 48000, cr: 'quarter', divider: 1, head: 0 },
])(
  '$rate Hz $cr head $head preserves wet response and dry alignment',
  async (scenario) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-ir-rate-'));
    try {
      const library = join(directory, 'effetune', 'ir-library');
      await mkdir(library, { recursive: true });
      const response = new Float32Array(512);
      response[0] = 1;
      response[64] = 0.5;
      const original = encodeWave([response], scenario.original);
      const sha256 = createHash('sha256').update(original).digest('hex');
      const irId = sha256.slice(0, 24);
      await writeFile(join(library, irId + '.wav'), original);
      await writeFile(
        join(library, 'index.json'),
        JSON.stringify({
          version: 1,
          entries: {
            [irId]: {
              irId,
              composition: 'single',
              originals: [
                {
                  role: 'single',
                  fileName: 'input.wav',
                  storageName: irId + '.wav',
                  sha256,
                  byteLength: original.length,
                },
              ],
              bytes: original.length,
              channels: 1,
              frames: 512,
              sampleRate: scenario.original,
              topology: 'mono',
              pathSummary: [],
              importedAt: '2026-10-10T00:00:00.000Z',
              analysis: {
                storageName: irId + '.analysis',
                onsetFrame: 0,
                rt60: null,
                peakDb: 0,
              },
            },
          },
        })
      );
      const expectedLatency =
        scenario.head * scenario.divider + 126 * (scenario.divider - 1);
      const preset = join(directory, 'rate.effetune_preset');
      for (const dry of [false, true]) {
        await writeFile(
          preset,
          JSON.stringify({
            pipeline: [
              {
                name: 'IR Reverb',
                parameters: {
                  ir: irId,
                  lt: String(scenario.head),
                  cr: scenario.cr,
                  cm: 'auto',
                  dw: dry ? -96 : 0,
                  de: dry,
                  dl: 0,
                },
              },
            ],
          })
        );
        for (const backend of ['scalar', 'simd']) {
          const processed = spawnSync(
            process.env.PIPETUNE_ASSET_DRIVER!,
            [preset, String(scenario.rate), '2', '4096', '0', backend],
            {
              encoding: 'utf8',
              env: {
                ...process.env,
                XDG_CONFIG_HOME: directory,
                XDG_CACHE_HOME: join(directory, 'cache'),
                PATH: '/nonexistent',
              },
            }
          );
          expect(processed.status, processed.stderr).toBe(0);
          const [active, latency, ...audio] = processed.stdout
            .trim()
            .split(/\s+/)
            .map(Number);
          expect(active).toBe(1);
          expect(latency).toBe(expectedLatency);
          expect(audio).toHaveLength(8192);
          expect(audio.every(Number.isFinite)).toBe(true);
          expect(Math.max(...audio.slice(4096).map(Math.abs))).toBeLessThan(
            1e-6
          );
          if (dry) {
            let error = 0;
            for (let frame = 0; frame < 4096; frame++)
              error = Math.max(
                error,
                Math.abs(audio[frame] - (frame === latency ? 1 : 0))
              );
            expect(error).toBeLessThan(0.0001);
          } else {
            // The application kernel applies sqrt(divider) to reduced-rate wet audio.
            expect(
              Math.abs(
                audio.slice(0, 4096).reduce((sum, value) => sum + value, 0) -
                  (1.5 / Math.sqrt(1.25)) * Math.sqrt(scenario.divider)
              )
            ).toBeLessThan(0.0002);
            let peak = 0;
            for (let frame = 0; frame < 4096; frame++)
              if (Math.abs(audio[frame]) > Math.abs(audio[peak])) peak = frame;
            expect(Math.abs(peak - latency)).toBeLessThanOrEqual(3);
            expect(Math.abs(audio[peak])).toBeGreaterThan(0.15);
          }
        }
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
