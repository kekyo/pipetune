import { spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

test.each(
  [false, true].flatMap((extension) =>
    [0, 35, 100].flatMap((amount) =>
      [0, 100].map((reverb) => ({ amount, extension, reverb }))
    )
  )
)(
  'Correction amount $amount, extension $extension and reverb $reverb reach native playback without Node',
  async ({ amount, extension, reverb }) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-room-phase-'));
    try {
      const library = join(directory, 'effetune', 'measurement-backups');
      await mkdir(library, { recursive: true });
      const file = join(library, 'phase.json'),
        preset = join(directory, 'phase.effetune_preset');
      const data = new Float32Array(8192);
      const coefficient = -0.995,
        onsetIndex = 57;
      data[onsetIndex] = coefficient;
      for (let index = 1; index < 7000; index++)
        data[onsetIndex + index] =
          (1 - coefficient ** 2) * (-coefficient) ** (index - 1);
      const impulse = {
        measurementId: 'phase',
        pointId: 3,
        sampleRate: 48000,
        onsetIndex,
        refScale: 1,
        data,
      };
      const measurement = {
        id: 'phase',
        points: [{ pointId: 3 }],
        averageFrequencyResponse: [
          [20, 0],
          [20000, 0],
        ],
      };
      await writeFile(
        file,
        JSON.stringify({
          ...measurement,
          impulseResponses: [
            { ...impulse, data: Buffer.from(data.buffer).toString('base64') },
          ],
        })
      );
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'Room EQ',
              enabled: true,
              parameters: {
                ms: 'phase',
                pm: 'full',
                tp: 8192,
                fl: 20,
                dw: extension ? 6 : 40,
                pa: false,
                pl: extension ? 500 : 25,
                le: extension,
                rv: reverb,
                cr: 0,
                pr: amount,
                pq: false,
                ps: 0.4,
                rp: 4,
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
          sampleRate: 48000,
          taps: 8192,
          phase: 'full',
          directWindowMs: extension ? 6 : 40,
          phaseLowFrequency: extension ? 500 : 25,
          lowFrequencyPhaseExtension: extension,
          reverbAmount: reverb / 100,
          correctionAmount: 0,
          phaseCorrectionAmount: amount / 100,
          phaseSmoothing: 0.4,
          referencePoint: 4,
        },
        sources: [{ measurement, impulses: [impulse] }],
      });
      const frames = 8448;
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
      if (reverb) {
        expect(result.stderr).toContain('Room EQ reverb correction:');
        const effective = result.stderr.match(/effectiveWindowMs=([0-9.]+)/);
        expect(effective).not.toBeNull();
        expect(Number(effective![1])).toBeCloseTo(
          reference.diagnostics.reverbCorrection[0].effectiveWindowMs,
          4
        );
      }
      const lowDiagnostic = reference.diagnostics.lowFrequencyPhaseExtension[0];
      if (lowDiagnostic.state === 'reduced') {
        expect(result.stderr).toContain(
          'Room EQ low-frequency extension: reduced'
        );
        expect(result.stderr).toContain(lowDiagnostic.reason);
      }
      const [active, latency, ...audio] = result.stdout
        .trim()
        .split(/\s+/)
        .map(Number);
      expect(active).toBe(1);
      expect(latency).toBe(4224);
      let error = 0;
      for (let frame = 0; frame < frames; frame++) {
        const expected =
          frame >= 128 && frame < 8320 ? reference.channels[0][frame - 128] : 0;
        error = Math.max(error, Math.abs(audio[frames + frame] - expected));
        expect(audio[frame]).toBe(0);
      }
      expect(error).toBeLessThan(0.0002);
      await writeFile(file, JSON.stringify(measurement));
      const missing = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '2', '256', '0'],
        {
          encoding: 'utf8',
          env: { ...process.env, XDG_CONFIG_HOME: directory },
        }
      );
      expect(missing.status).toBe(1);
      expect(missing.stderr).toContain('complete impulse');
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
