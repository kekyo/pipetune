import { spawnSync } from 'node:child_process';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

test.each([
  { phase: 'min', amount: 80, limitPeakGain: false, target: true, rate: 44100 },
  { phase: 'lin', amount: -90, limitPeakGain: true, target: true, rate: 48000 },
  {
    phase: 'lin',
    amount: 100,
    limitPeakGain: true,
    target: false,
    rate: 96000,
  },
])(
  'Headphone EQ renders the saved curves at $rate Hz ($phase, $amount%)',
  async ({ phase, amount, limitPeakGain, target, rate }) => {
    const dir = await mkdtemp(join(tmpdir(), 'pipetune-headphone-'));
    try {
      const steps = Math.ceil(Math.log2(1000) * 24);
      const grid = Array.from(
        { length: steps + 1 },
        (_, i) => 20 * 1000 ** (i / steps)
      );
      const measurement = grid.map(
        (f) => 9 * Math.exp(-(Math.log2(f / 800) ** 2))
      );
      const goal = grid.map((f) => 3 * Math.exp(-(Math.log2(f / 3200) ** 2)));
      const preset = join(dir, 'headphone.effetune_preset');
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'Headphone EQ',
              enabled: true,
              channel: 'R',
              parameters: {
                mr: measurement,
                mt: target ? goal : [],
                tp: 8192,
                pm: phase,
                ca: amount,
                ah: limitPeakGain,
                mb: 4,
                uf: 14000,
                lt: '128',
                ta1: 'ls',
                ga1: -3,
                qa1: 0.7,
              },
            },
          ],
        })
      );
      const { designRoomEq } = await import(
        /* @vite-ignore */ pathToFileURL(
          join(
            process.env.PIPETUNE_EFFETUNE_SOURCE!,
            'js/room-eq/design-core.js'
          )
        ).href
      );
      const reference = designRoomEq({
        config: {
          phase,
          taps: 8192,
          sampleRate: rate,
          lowFrequency: 20,
          highFrequency: 14000,
          correctionAmount: amount / 100,
          smoothing: 0.17,
          maxBoostDb: 4,
          limitPeakGain,
          eqBands: [
            { enabled: true, type: 'ls', frequency: 316, gain: -3, q: 0.7 },
          ],
        },
        sources: [
          target
            ? {
                measurement: {
                  id: 'headphone-eq',
                  averageFrequencyResponse: grid.map((f, i) => [
                    f,
                    measurement[i] - goal[i],
                  ]),
                },
              }
            : null,
        ],
      });
      const frames = 8192 + 128;
      for (const backend of ['scalar', 'simd']) {
        const result = spawnSync(
          process.env.PIPETUNE_ASSET_DRIVER!,
          [preset, String(rate), '2', String(frames), '1', backend],
          {
            encoding: 'utf8',
            env: { ...process.env, XDG_CONFIG_HOME: dir, PATH: '/nonexistent' },
          }
        );
        expect(result.status, result.stderr).toBe(0);
        const [active, latency, ...pcm] = result.stdout
          .trim()
          .split(/\s+/)
          .map(Number);
        expect(active, result.stderr).toBe(1);
        expect(latency).toBe(128 + reference.latencyInfo.filterDelaySamples);
        expect(pcm).toHaveLength(frames * 2);
        let error = 0;
        for (let i = 0; i < frames; ++i) {
          const expected = i < 128 ? 0 : reference.channels[0][i - 128];
          error = Math.max(
            error,
            Math.abs(pcm[i]),
            Math.abs(pcm[frames + i] - expected)
          );
        }
        expect(error).toBeLessThan(0.0002);
      }
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  }
);
