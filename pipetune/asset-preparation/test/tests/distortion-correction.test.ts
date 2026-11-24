import { spawnSync } from 'node:child_process';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

test('Distortion Correction plays the measured physical speaker selected in the preset', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'pipetune-distortion-'));
  try {
    const upstream = process.env.PIPETUNE_EFFETUNE_SOURCE!;
    const { encodeSpeakerModel } = await import(
      /* @vite-ignore */ pathToFileURL(
        join(upstream, 'features/measurement/dataStorage.js')
      ).href
    );
    const { createSpeakerModel } = await import(
      /* @vite-ignore */ pathToFileURL(
        join(upstream, 'tests/helpers/speaker-model-fixture.mjs')
      ).href
    );
    const { validateSpeakerModel } = await import(
      /* @vite-ignore */ pathToFileURL(
        join(upstream, 'js/utils/measurement-dsp/speaker-distortion/index.js')
      ).href
    );
    const { speakerDistortionReference, speakerDistortionTiming } =
      await import(
        /* @vite-ignore */ pathToFileURL(
          join(
            upstream,
            'tests/helpers/speaker-distortion-kernel-reference.mjs'
          )
        ).href
      );
    const model = createSpeakerModel(2);
    const speaker = model.channels[1];
    speaker.weights.short.fill(0.004);
    speaker.weights.long.fill(-0.007);
    speaker.odd = {
      weights: new Float32Array(32).fill(0.02),
      bounds: new Float32Array(32).fill(0.5),
      on: { alone: true, short: false, long: true },
      fitReason: null,
      lambda: 1,
      figL: { o1: -3, o2: -2, o3: null, oddAll: -2.5 },
      figL6: -1,
      worst: null,
      reason: { alone: null, short: 'notChecked', long: null },
      checkFig: { alone: null, short: null, long: null },
      loud: null,
      sweepResponse: null,
    };
    model.lastCheck = { at: 20, dv: -6, outcome: 'lower' };
    expect(validateSpeakerModel(model)).toBe(true);
    const library = join(dir, 'effetune', 'measurement-backups');
    await mkdir(library, { recursive: true });
    await writeFile(
      join(library, 'speakers.json'),
      JSON.stringify({
        id: 'speakers',
        speakerModel: encodeSpeakerModel(model),
      })
    );
    const preset = join(dir, 'speakers.effetune_preset');
    const frames = 2048;
    for (const config of [
      {
        fl: 'Short',
        md: 'Reduce',
        cn: 2,
        co: 0,
        co3: 0,
        even: true,
        odd: false,
        rate: 48000,
        dc: 20,
      },
      {
        fl: 'Long',
        md: 'Add',
        cn: 2,
        co: 0,
        co3: 0,
        even: true,
        odd: true,
        rate: 96000,
        dc: 0,
      },
      {
        fl: 'Short',
        md: 'Add',
        cn: 0,
        co: 0,
        co3: 0,
        even: false,
        odd: true,
        rate: 44100,
        dc: 20,
      },
      {
        fl: 'Long',
        md: 'Reduce',
        cn: 2,
        co: 2,
        co3: 2,
        even: false,
        odd: false,
        rate: 48000,
        dc: 20,
      },
    ]) {
      const { fl, md, cn, co, co3, rate, dc } = config;
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'Distortion Correction',
              enabled: true,
              channel: 'R',
              parameters: { ms: 'speakers', fl, md, cn, co, co3, dv: -3, dc },
            },
          ],
        })
      );
      const signal = Float32Array.from(
        { length: frames },
        (_, i) => 0.3 * Math.sin((2 * Math.PI * 730 * i) / rate)
      );
      const step = fl === 'Long' ? 1 : 0;
      // Give the independent whole-signal reference just the weights expected to play.
      const coefficients = new Float32Array(8 + 16 * 1247);
      coefficients[8 + 1182] = step;
      const right = coefficients.subarray(8 + 1247);
      if (config.even) right.set(speaker.weights[fl.toLowerCase()]);
      if (config.odd) {
        right.set(speaker.odd.weights, 1183);
        right.set(speaker.odd.bounds, 1215);
      }
      const expected =
        config.even || config.odd
          ? speakerDistortionReference({
              bytes: coefficients,
              sampleRate: rate,
              dv: dc === 0 ? -6 : -3,
              mode: md,
              input: [new Float32Array(frames), signal],
            })[1]
          : signal;
      const delay =
        config.even || config.odd
          ? speakerDistortionTiming(rate, step).latency
          : 0;
      for (const backend of ['scalar', 'simd']) {
        const rendered = spawnSync(
          process.env.PIPETUNE_ASSET_DRIVER!,
          [preset, String(rate), '2', String(frames), '1', backend, '730'],
          {
            encoding: 'utf8',
            env: { ...process.env, XDG_CONFIG_HOME: dir, PATH: '/nonexistent' },
          }
        );
        expect(rendered.status, rendered.stderr).toBe(0);
        const [active, latency, ...pcm] = rendered.stdout
          .trim()
          .split(/\s+/)
          .map(Number);
        expect(active, rendered.stderr).toBe(1);
        expect(latency).toBe(delay);
        expect(rendered.stderr).toContain('dependencies=1');
        expect(pcm).toHaveLength(frames * 2);
        let error = 0;
        for (let i = 0; i < frames; ++i)
          error = Math.max(
            error,
            Math.abs(pcm[i]),
            Math.abs(pcm[frames + i] - expected[i])
          );
        expect(error).toBeLessThan(0.0002);
      }
    }
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test('an unassigned speaker passes audio and an unreadable assigned model reports the measurement', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'pipetune-speaker-source-'));
  try {
    const library = join(dir, 'effetune', 'measurement-backups');
    await mkdir(library, { recursive: true });
    const preset = join(dir, 'speaker.effetune_preset');
    for (const id of ['', 'missing', 'invalid']) {
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'Distortion Correction',
              enabled: true,
              parameters: { ms: id },
            },
          ],
        })
      );
      if (id === 'invalid')
        await writeFile(
          join(library, 'invalid.json'),
          JSON.stringify({
            id,
            speakerModel: {
              channels: [
                {
                  index: 0,
                  on: { short: true },
                  weights: { short: { f32: 'AAAA' } },
                },
              ],
            },
          })
        );
      const result = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '2', '4', '0'],
        { encoding: 'utf8', env: { ...process.env, XDG_CONFIG_HOME: dir } }
      );
      if (id === '') {
        expect(result.status, result.stderr).toBe(0);
        expect(result.stdout.trim().split(/\s+/).map(Number)).toEqual([
          1, 0, 1, 0, 0, 0, 0, 0, 0, 0,
        ]);
      } else {
        expect(result.status).toBe(1);
        expect(result.stderr).toContain('Distortion Correction');
        expect(result.stderr).toContain(id);
      }
    }
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});
