import { spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

test.each(['min', 'lin'])(
  'stored multi-point IRs drive %s correction and incomplete sets use the saved frequency response',
  async (phase) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-room-measured-'));
    try {
      const library = join(directory, 'effetune', 'measurement-backups');
      await mkdir(library, { recursive: true });
      const file = join(library, 'measured.json');
      const preset = join(directory, 'measured.effetune_preset');
      const impulses = [0, 2].map((pointId) => {
        const data = new Float32Array(4096);
        data[50 + pointId] = 1;
        data[103 + pointId] = 0.6;
        data[211 + pointId * 8] = -0.35;
        return {
          measurementId: 'measured',
          pointId,
          sampleRate: 44100,
          onsetIndex: 50 + pointId,
          refScale: 1 + pointId,
          data,
        };
      });
      const measurement = {
        id: 'measured',
        points: [{ pointId: 0 }, { pointId: 2 }],
        averageFrequencyResponse: [
          [20, 0],
          [1000, 18],
          [20000, -6],
        ],
      };
      const records = impulses.map((impulse) => ({
        ...impulse,
        data: Buffer.from(impulse.data.buffer).toString('base64'),
      }));
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'Room EQ',
              enabled: true,
              parameters: {
                ms: 'measured',
                pm: phase,
                tp: 8192,
                fl: 20,
                lt: '128',
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
      const frames = 8448;
      for (const complete of [true, false]) {
        await writeFile(
          file,
          JSON.stringify({
            ...measurement,
            impulseResponses: complete
              ? [...records].reverse()
              : records.slice(0, 1),
          })
        );
        const reference = upstream.designRoomEq({
          config: { sampleRate: 48000, taps: 8192, phase },
          sources: [
            {
              measurement: { ...measurement, id: `measured-${complete}` },
              impulses: complete ? impulses : [],
            },
          ],
        });
        const result = spawnSync(
          process.env.PIPETUNE_ASSET_DRIVER!,
          [preset, '48000', '2', String(frames), '1'],
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
        expect(active).toBe(1);
        expect(latency).toBe(128 + reference.latencyInfo.filterDelaySamples);
        let error = 0;
        for (let frame = 0; frame < frames; frame++) {
          const expected =
            frame >= 128 && frame < 8320
              ? reference.channels[0][frame - 128]
              : 0;
          error = Math.max(error, Math.abs(audio[frames + frame] - expected));
          expect(audio[frame]).toBe(0);
        }
        expect(error).toBeLessThan(0.0002);
      }
      for (const invalid of [
        'broken',
        Buffer.from([0, 0, 192, 127]).toString('base64'),
      ]) {
        await writeFile(
          file,
          JSON.stringify({
            ...measurement,
            impulseResponses: [{ ...records[0], data: invalid }, records[1]],
          })
        );
        const failed = spawnSync(
          process.env.PIPETUNE_ASSET_DRIVER!,
          [preset, '48000', '2', '256', '0'],
          {
            encoding: 'utf8',
            env: { ...process.env, XDG_CONFIG_HOME: directory },
          }
        );
        expect(failed.status).toBe(1);
        expect(failed.stderr).toContain('impulse');
      }
      await writeFile(
        file,
        JSON.stringify({
          ...measurement,
          points: [{ pointId: 0 }, { pointId: 0 }],
          impulseResponses: records,
        })
      );
      const repeated = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '2', '256', '0'],
        {
          encoding: 'utf8',
          env: { ...process.env, XDG_CONFIG_HOME: directory },
        }
      );
      expect(repeated.status).toBe(1);
      expect(repeated.stderr).toContain('point');
      await writeFile(
        file,
        JSON.stringify({
          ...measurement,
          impulseResponses: records.map((record) => ({
            ...record,
            sampleRate: 1,
          })),
        })
      );
      const expanded = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '2', '256', '0'],
        {
          encoding: 'utf8',
          env: {
            ...process.env,
            XDG_CONFIG_HOME: directory,
            PIPETUNE_TEST_ASSET_MEMORY_BYTES: String(256 * 1024 * 1024),
          },
        }
      );
      expect(expanded.status).toBe(1);
      expect(expanded.stderr).toContain('memory');
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
