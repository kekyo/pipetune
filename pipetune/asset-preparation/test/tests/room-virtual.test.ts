import { spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

test.each(['min', 'lin', 'full'])(
  'virtual channel IR identities drive %s design',
  async (phase) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-room-virtual-'));
    try {
      const library = join(directory, 'effetune', 'measurement-backups');
      await mkdir(library, { recursive: true });
      const file = join(library, 'virtual.json'),
        preset = join(directory, 'virtual.effetune_preset');
      const pairs = [0, 2].map((pointId) =>
        ['left', 'right'].map((channel, side) => {
          const data = new Float32Array(2048);
          data[32] = 1;
          data[87 + pointId] = side ? -0.55 : 0.3;
          return {
            channel,
            originalPoint: pointId,
            measurementId: 'virtual',
            pointId: 30 + pointId * 2 + side,
            sampleRate: 48000,
            onsetIndex: 32,
            refScale: 1,
            data,
          };
        })
      );
      const measurement = {
        id: 'virtual',
        outputChannels: ['left', 'right'],
        points: pairs.map((pair) => ({
          pointId: pair[0].originalPoint,
          channels: pair.map((record) => ({
            channel: record.channel,
            irId: record.pointId,
          })),
        })),
        channelResponses: ['left', 'right', 'ghost'].map((channel) => ({
          channel,
          averageFrequencyResponse: [
            [20, 0],
            [20000, 0],
          ],
        })),
      };
      const records = pairs.flat().map((record) => ({
        ...record,
        data: Buffer.from(record.data.buffer).toString('base64'),
      }));
      const writePreset = async (parameters: object) =>
        await writeFile(
          preset,
          JSON.stringify({
            pipeline: [
              {
                name: 'Room EQ',
                enabled: true,
                channel: '34',
                parameters: {
                  ms0: 'virtual::ch=left',
                  ms1: 'virtual::ch=right',
                  pm: phase,
                  tp: 8192,
                  fl: 20,
                  rp: 3,
                  ...parameters,
                },
              },
            ],
          })
        );
      const run = () =>
        spawnSync(
          process.env.PIPETUNE_ASSET_DRIVER!,
          [preset, '48000', '6', '8448', '3'],
          {
            encoding: 'utf8',
            maxBuffer: 6 * 1024 * 1024,
            env: {
              ...process.env,
              PATH: '/nonexistent',
              XDG_CONFIG_HOME: directory,
              XDG_CACHE_HOME: join(directory, 'cache'),
            },
          }
        );
      const upstream = await import(
        /* @vite-ignore */ pathToFileURL(
          process.env.PIPETUNE_EFFETUNE_SOURCE + '/js/room-eq/design-core.js'
        ).href
      );
      await writePreset({});
      for (const duplicate of [false, true]) {
        const points = structuredClone(measurement.points);
        if (duplicate) points[1].channels[1].irId = points[0].channels[1].irId;
        await writeFile(
          file,
          JSON.stringify({
            ...measurement,
            points,
            impulseResponses: [...records].reverse(),
          })
        );
        const result = run();
        if (duplicate && phase === 'full') {
          expect(result.status).toBe(1);
          expect(result.stderr).toContain('complete impulse');
          continue;
        }
        expect(result.status, result.stderr).toBe(0);
        const reference = upstream.designRoomEq({
          config: { sampleRate: 48000, taps: 8192, phase, referencePoint: 3 },
          sources: [
            {
              measurement: {
                id: 'right-' + duplicate,
                averageFrequencyResponse: [
                  [20, 0],
                  [20000, 0],
                ],
              },
              impulses: duplicate
                ? []
                : pairs.map((pair) => ({
                    ...pair[1],
                    pointId: pair[1].originalPoint,
                  })),
            },
          ],
        });
        const [active, latency, ...audio] = result.stdout
          .trim()
          .split(/\s+/)
          .map(Number);
        expect(active).toBe(1);
        expect(latency).toBe(128 + reference.latencyInfo.filterDelaySamples);
        let error = 0;
        for (let index = 0; index < 8448; index++) {
          const expected =
            index >= 128 && index < 8320
              ? reference.channels[0][index - 128]
              : 0;
          error = Math.max(error, Math.abs(audio[3 * 8448 + index] - expected));
        }
        expect(error).toBeLessThan(0.0002);
      }
      await writeFile(
        file,
        JSON.stringify({ ...measurement, impulseResponses: records })
      );
      await writePreset({ ms1: 'virtual::ch=ghost' });
      expect(run().status).toBe(1);
      await writePreset({ ms1: 'virtual' });
      expect(run().status).toBe(1);
      await writePreset({ ms1: 'missing' });
      expect(run().status).toBe(1);
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
