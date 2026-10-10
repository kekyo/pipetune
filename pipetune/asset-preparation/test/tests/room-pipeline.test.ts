import { spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

test.each(['min', 'lin'])(
  'registered shared %s measurement produces corrected PCM and FIR latency without Node',
  async (phase) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-room-'));
    try {
      const library = join(directory, 'effetune', 'measurement-backups');
      await mkdir(library, { recursive: true });
      const averageFrequencyResponse = Array.from(
        { length: 81 },
        (_, index) => {
          const frequency = 20 * 1000 ** (index / 80);
          return {
            frequency,
            magnitude:
              8 * Math.exp(-(Math.log2(frequency / 700) ** 2) / 0.8) -
              9 * Math.exp(-(Math.log2(frequency / 3100) ** 2) / 0.3),
          };
        }
      );
      const measurement = {
        id: 'room-shared',
        averageFrequencyResponse,
        points: [{ pointId: 0 }],
      };
      await writeFile(
        join(library, 'room-shared.json'),
        JSON.stringify(measurement)
      );
      const preset = join(directory, 'room.effetune_preset');
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'Room EQ',
              enabled: true,
              parameters: {
                ms: 'room-shared',
                pm: phase,
                tp: 8192,
                sm: 0.12,
                fl: 80,
                fh: 15000,
                cr: 70,
                mb: 4,
                lt: '128',
                bs: [
                  {
                    enabled: true,
                    type: 'pk',
                    frequency: 1800,
                    gain: -2,
                    q: 1.2,
                  },
                ],
              },
            },
          ],
        })
      );
      const upstream = await import(
        /* @vite-ignore */ pathToFileURL(
          process.env.PIPETUNE_EFFETUNE_SOURCE + '/js/room-eq/design-core.js'
        ).href
      );
      const reference = upstream.designRoomEq({
        config: {
          phase,
          sampleRate: 48000,
          taps: 8192,
          smoothing: 0.12,
          lowFrequency: 80,
          highFrequency: 15000,
          correctionAmount: 0.7,
          maxBoostDb: 4,
          eqBands: [
            { enabled: true, type: 'pk', frequency: 1800, gain: -2, q: 1.2 },
          ],
        },
        sources: [{ measurement }],
      });
      const frames = 8192 + 256;
      for (const backend of ['scalar', 'simd']) {
        const result = spawnSync(
          process.env.PIPETUNE_ASSET_DRIVER!,
          [preset, '48000', '2', String(frames), '0', backend],
          {
            encoding: 'utf8',
            maxBuffer: 4 * 1024 * 1024,
            env: {
              ...process.env,
              PATH: '/nonexistent',
              XDG_CONFIG_HOME: directory,
              XDG_CACHE_HOME: join(directory, 'cache'),
            },
          }
        );
        expect(result.status, result.stderr).toBe(0);
        const [active, latency, ...audio] = result.stdout
          .trim()
          .split(/\s+/)
          .map(Number);
        expect(active, result.stderr).toBe(1);
        expect(latency).toBe(128 + reference.latencyInfo.filterDelaySamples);
        expect(audio).toHaveLength(frames * 2);
        let maximum = 0;
        for (let frame = 0; frame < frames; frame++) {
          const expected =
            frame >= 128 && frame < 128 + 8192
              ? reference.channels[0][frame - 128]
              : 0;
          maximum = Math.max(maximum, Math.abs(audio[frame] - expected));
          expect(Math.abs(audio[frames + frame])).toBeLessThan(1e-7);
        }
        expect(maximum).toBeLessThan(0.0002);
        const magnitude = (frequency: number) => {
          let real = 0,
            imag = 0;
          for (let frame = 0; frame < frames; frame++) {
            const angle = (2 * Math.PI * frequency * frame) / 48000;
            real += audio[frame] * Math.cos(angle);
            imag -= audio[frame] * Math.sin(angle);
          }
          return Math.hypot(real, imag);
        };
        expect(magnitude(700)).toBeLessThan(magnitude(3100) * 0.6);
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);

test.each(['min', 'lin', 'full'])(
  'unassigned %s Room EQ retains native gain without a convolution delay',
  async (phase) => {
    const directory = await mkdtemp(
      join(tmpdir(), 'pipetune-room-unassigned-')
    );
    try {
      const preset = join(directory, 'empty.effetune_preset');
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'Room EQ',
              enabled: true,
              parameters: { pm: phase, tp: 131072, gn: -6, lt: '1024' },
            },
          ],
        })
      );
      const result = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '2', '256', '0'],
        {
          encoding: 'utf8',
          env: {
            ...process.env,
            PATH: '/nonexistent',
            XDG_CONFIG_HOME: directory,
            PIPETUNE_TEST_ASSET_MEMORY_BYTES: '1048576',
          },
        }
      );
      expect(result.status, result.stderr).toBe(0);
      expect(result.stderr).toContain('No measurement');
      const [active, latency, ...audio] = result.stdout
        .trim()
        .split(/\s+/)
        .map(Number);
      expect(active).toBe(1);
      expect(latency).toBe(0);
      expect(Math.abs(audio[0] - 10 ** (-6 / 20))).toBeLessThan(1e-7);
      expect(audio.slice(1).every((value) => value === 0)).toBe(true);
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);

test('assigned Room EQ source errors fail preparation and disabled nodes do not read them', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'pipetune-room-errors-'));
  try {
    const library = join(directory, 'effetune', 'measurement-backups');
    await mkdir(library, { recursive: true });
    const preset = join(directory, 'room.effetune_preset');
    const file = join(library, 'missing.json');
    const run = async (parameters: object, enabled = true) => {
      await writeFile(
        preset,
        JSON.stringify({ pipeline: [{ name: 'Room EQ', enabled, parameters }] })
      );
      return spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '2', '256', '0'],
        {
          encoding: 'utf8',
          env: {
            ...process.env,
            PATH: '/nonexistent',
            XDG_CONFIG_HOME: directory,
          },
        }
      );
    };
    expect((await run({ ms: 'missing' })).status).toBe(1);
    expect((await run({ ms: '../escape' })).status).toBe(1);
    await writeFile(file, '{broken');
    expect((await run({ ms: 'missing' })).status).toBe(1);
    await writeFile(
      file,
      JSON.stringify({ id: 'wrong', averageFrequencyResponse: [[100, 0]] })
    );
    expect((await run({ ms: 'missing' })).status).toBe(1);
    await writeFile(
      file,
      JSON.stringify({ id: 'missing', averageFrequencyResponse: [[100, 0]] })
    );
    expect((await run({ ms: 'missing', pm: 'full' })).status).toBe(1);
    const off = await run({ ms: '../escape' }, false);
    expect(off.status, off.stderr).toBe(0);
    expect(Number(off.stdout.split(/\s+/)[0])).toBe(0);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
