import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test.each([48000, 44100])(
  'a registered L/R pair at %i and 48000 Hz preserves all four routes',
  async (leftRate) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-ir-pair-'));
    try {
      const library = join(directory, 'effetune', 'ir-library');
      await mkdir(library, { recursive: true });
      const sources = [leftRate, 48000].map((rate, side) => {
        const channels = [
          new Float32Array(512 + side * 512),
          new Float32Array(512 + side * 512),
        ];
        channels[0][0] = 1;
        channels[1][0] = 0.5;
        channels[0][Math.round(rate * 0.004)] = 0.25;
        channels[1][Math.round(rate * 0.006)] = 0.125;
        return { channels, rate, bytes: encodeWave(channels, rate) };
      });
      const digests = sources.map((source) =>
        createHash('sha256').update(source.bytes).digest()
      );
      const irId = createHash('sha256')
        .update(Buffer.concat(digests))
        .digest('hex')
        .slice(0, 24);
      const originals = sources.map((source, side) => ({
        role: side === 0 ? 'L' : 'R',
        fileName: side === 0 ? 'Hall Left.wav' : 'Hall Right.wav',
        storageName: irId + (side === 0 ? '.L.wav' : '.R.wav'),
        sha256: digests[side].toString('hex'),
        byteLength: source.bytes.length,
      }));
      for (let side = 0; side < sources.length; side++)
        await writeFile(
          join(library, originals[side].storageName),
          sources[side].bytes
        );
      await writeFile(
        join(library, 'index.json'),
        JSON.stringify({
          version: 1,
          entries: {
            [irId]: {
              irId,
              composition: 'pair',
              originals,
              bytes: sources[0].bytes.length + sources[1].bytes.length,
              channels: 4,
              frames: 1024,
              sampleRate: 48000,
              topology: 'true-stereo',
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
      const preset = join(directory, 'pair.effetune_preset');
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'IR Reverb',
              parameters: {
                ir: irId,
                cm: 'auto',
                cr: 'full',
                lt: '128',
                dw: 0,
                de: false,
              },
            },
          ],
        })
      );
      let reference: Float32Array[] | undefined;
      if (leftRate === 48000) {
        const root = process.env.PIPETUNE_EFFETUNE_SOURCE;
        const pairUrl = pathToFileURL(
          root + '/js/ir-library/ir-true-stereo-pair.js'
        ).href;
        const preparationUrl = pathToFileURL(
          root + '/js/ir-library/ir-preparation.js'
        ).href;
        const pairing = await import(/* @vite-ignore */ pairUrl);
        const preparation = await import(/* @vite-ignore */ preparationUrl);
        const merged = pairing.mergeTrueStereoPair(
          sources.map((source, side) => ({
            name: originals[side].fileName,
            pcm: { channels: source.channels, sampleRate: source.rate },
          }))
        );
        reference = preparation.prepareIr({
          ...merged,
          options: { topology: 3 },
        }).channels;
      }
      for (const input of [0, 1]) {
        const rendered = spawnSync(
          process.env.PIPETUNE_ASSET_DRIVER!,
          [preset, '48000', '2', '2048', String(input)],
          {
            encoding: 'utf8',
            env: {
              ...process.env,
              XDG_CONFIG_HOME: directory,
              XDG_CACHE_HOME: join(directory, 'cache'),
            },
          }
        );
        expect(rendered.status, rendered.stderr).toBe(0);
        const [active, latency, ...audio] = rendered.stdout
          .trim()
          .split(/\s+/)
          .map(Number);
        expect(active).toBe(1);
        expect(latency).toBe(128);
        if (reference) {
          let error = 0;
          for (let output = 0; output < 2; output++)
            for (let frame = 0; frame < 2048; frame++) {
              const expected =
                frame >= latency
                  ? (reference[input * 2 + output][frame - latency] ?? 0)
                  : 0;
              error = Math.max(
                error,
                Math.abs(audio[output * 2048 + frame] - expected)
              );
            }
          expect(error).toBeLessThan(0.0002);
        } else {
          expect(audio[128]).toBeGreaterThan(0.5);
          expect(Math.abs(audio[2048 + 128] / audio[128] - 0.5)).toBeLessThan(
            0.002
          );
          // Both original rates must place the reflections at their original physical times.
          const peak = (output: number, center: number): number => {
            let maximum = 0;
            let location = 0;
            for (let frame = center - 4; frame <= center + 4; frame++) {
              if (Math.abs(audio[output * 2048 + frame]) > maximum) {
                maximum = Math.abs(audio[output * 2048 + frame]);
                location = frame;
              }
            }
            expect(maximum).toBeGreaterThan(0.05);
            return location;
          };
          expect(
            Math.abs(peak(0, 128 + 192) - (128 + 192))
          ).toBeLessThanOrEqual(1);
          expect(
            Math.abs(peak(1, 128 + 288) - (128 + 288))
          ).toBeLessThanOrEqual(1);
        }
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
