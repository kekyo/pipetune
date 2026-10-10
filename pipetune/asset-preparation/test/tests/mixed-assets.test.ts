import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test('all nine asset DSPs process together after restart, rate and channel rebuilding', async () => {
  const exported = process.env.PIPETUNE_ASSET_FIXTURE_OUTPUT;
  const directory =
    exported ?? (await mkdtemp(join(tmpdir(), 'pipetune-mixed-assets-')));
  try {
    const root = join(directory, 'effetune');
    const measurements = join(root, 'measurement-backups'),
      library = join(root, 'ir-library');
    await mkdir(measurements, { recursive: true });
    await mkdir(library, { recursive: true });
    const sample = new Float32Array(4800);
    for (let frame = 0; frame < sample.length; frame++)
      sample[frame] = 0.3 * Math.sin((2 * Math.PI * 700 * frame) / 48000);
    await writeFile(join(root, 'sample.wav'), encodeWave([sample], 48000));
    await writeFile(
      join(root, 'main.sfz'),
      '<region> sample=sample.wav key=83 amp_veltrack=0 loop_mode=loop_continuous'
    );
    const sf = '0123456789abcdef01234567';
    await writeFile(
      join(root, 'sfz-references.json'),
      JSON.stringify([
        { id: sf, name: 'Synthetic tone', root, path: join(root, 'main.sfz') },
      ])
    );
    const impulse = new Float32Array(512);
    impulse[0] = 1;
    impulse[64] = 0.35;
    const original = encodeWave([impulse], 48000),
      sha256 = createHash('sha256').update(original).digest('hex'),
      ir = sha256.slice(0, 24);
    await writeFile(join(library, ir + '.wav'), original);
    await writeFile(
      join(library, 'index.json'),
      JSON.stringify({
        version: 1,
        entries: {
          [ir]: {
            irId: ir,
            composition: 'single',
            bytes: original.length,
            originals: [
              {
                role: 'single',
                fileName: 'impulse.wav',
                storageName: ir + '.wav',
                sha256,
                byteLength: original.length,
              },
            ],
          },
        },
      })
    );
    for (const [ear, id] of ['left-ear', 'right-ear'].entries()) {
      const records = ['left', 'right'].map((channel, pointId) => {
        const data = new Float32Array(1024),
          direct = ear === pointId,
          onsetIndex = direct ? 48 : 62;
        data[onsetIndex] = direct ? 1 : 0.25;
        data[onsetIndex + 9] = direct ? -0.07 : 0.025;
        return {
          measurementId: id,
          pointId,
          channel,
          sampleRate: 48000,
          onsetIndex,
          trimStartSamples: 0,
          outputTimeReference: 'audio-context',
          refScale: 1,
          data: Buffer.from(data.buffer).toString('base64'),
        };
      });
      await writeFile(
        join(measurements, id + '.json'),
        JSON.stringify({
          id,
          outputChannels: ['left', 'right'],
          points: [
            {
              pointId: 0,
              channels: records.map((record) => ({
                channel: record.channel,
                irId: record.pointId,
                ir: { stored: true },
              })),
            },
          ],
          channelResponses: records.map((record) => ({
            channel: record.channel,
            averageFrequencyResponse: [
              [20, 0],
              [20000, 0],
            ],
          })),
          impulseResponses: records,
        })
      );
    }
    const pipeline = [
      {
        name: 'SFZ Note Player',
        channel: '',
        parameters: { sf, mn: 83, mx: 83, th: 0.01, dm: 0, wm: 100 },
      },
      {
        name: 'IR Reverb',
        channel: '',
        parameters: { ir, dw: 0, de: false, pd: 0, cr: 'full', lt: '128' },
      },
      {
        name: 'Room EQ',
        channel: '',
        parameters: {
          ms: 'left-ear::ch=left',
          pm: 'full',
          tp: 8192,
          fl: 20,
          bs: [{ frequency: 700, gain: -3, q: 1, enabled: true }],
        },
      },
      {
        name: 'Crosstalk Cancellation',
        channel: '',
        parameters: {
          ll: 'left-ear::ch=left',
          lr: 'right-ear::ch=left',
          rl: 'left-ear::ch=right',
          rr: 'right-ear::ch=right',
          tp: 1024,
          st: 75,
        },
      },
      {
        name: '5Band FIR PEQ',
        channel: '',
        parameters: {
          pm: 'min',
          tp: 8192,
          f2: 700,
          g2: 3,
          q2: 1,
          t2: 'pk',
          e2: true,
          lt: '0',
        },
      },
      {
        name: 'Group Delay EQ',
        channel: '',
        parameters: { tp: 4096, d7: 2, lt: '0' },
      },
      {
        name: 'Group Delay PEQ',
        channel: '',
        parameters: {
          tp: 4096,
          t0: 'pk',
          f0: 700,
          d0: 2,
          q0: 1,
          e0: true,
          lt: '0',
        },
      },
      {
        name: 'Bass Management',
        channel: 'All',
        parameters: {
          ph: 'Linear',
          tp: '8192',
          su: 8,
          ro: [1, 1, 0, 3],
          rt: [8, 8],
          fc: [300, 300],
        },
      },
      {
        name: 'FIR Crossover',
        channel: 'All',
        parameters: { bc: 2, pm: 'min', tp: 8192, f1: 800, s1: -48, lt: '0' },
      },
    ];
    const preset = join(directory, 'mixed.effetune_preset');
    await writeFile(preset, JSON.stringify({ pipeline }));
    for (const section of [false, true]) {
      const off = pipeline.map((node) => ({
        ...node,
        enabled: section,
        parameters: {
          ...node.parameters,
          sf: '../missing',
          ir: '../missing',
          ms: '../missing',
        },
      }));
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: section
            ? [{ name: 'Section', enabled: false }, ...off]
            : off,
        })
      );
      const result = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '4', '257', '0'],
        {
          encoding: 'utf8',
          env: {
            ...process.env,
            PATH: '/nonexistent',
            XDG_CONFIG_HOME: join(directory, 'missing-root'),
            PIPETUNE_TEST_ASSET_MEMORY_BYTES: '1048576',
          },
        }
      );
      expect(result.status, result.stderr).toBe(0);
      const [active, latency, ...audio] = result.stdout
        .trim()
        .split(/\s+/)
        .map(Number);
      expect(active).toBe(0);
      expect(latency).toBe(0);
      expect(result.stderr).toContain('dependencies=0');
      expect(audio[0]).toBe(1);
      expect(audio.slice(1).every((value) => value === 0)).toBe(true);
    }
    await writeFile(preset, JSON.stringify({ pipeline }));
    if (exported) {
      for (const [index, node] of pipeline.entries())
        await writeFile(
          join(directory, 'asset-' + index + '.effetune_preset'),
          JSON.stringify({ pipeline: [node] })
        );
    }
    const run = (
      rate: number,
      width: number,
      backend: string,
      rebuild: boolean
    ) => {
      const result = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [
          preset,
          String(rate),
          String(width),
          '48013',
          '0',
          backend,
          '1000',
          ...(rebuild ? ['rebuild'] : []),
        ],
        {
          encoding: 'utf8',
          maxBuffer: 24 * 1024 * 1024,
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
      expect(active, result.stderr).toBe(9);
      expect(latency).toBeGreaterThan(8192);
      expect(audio).toHaveLength(width * 48013);
      expect(audio.every(Number.isFinite)).toBe(true);
      const energy = audio
        .slice(32000, 47000)
        .reduce((sum, value) => sum + value * value, 0);
      expect(energy).toBeGreaterThan(1);
      return { audio, latency, stderr: result.stderr };
    };
    const cold = run(48000, 4, 'scalar', false),
      warm = run(48000, 4, 'scalar', false);
    expect(warm.audio).toEqual(cold.audio);
    expect(warm.stderr).toContain('cacheHits=3');
    for (const rate of [48000, 96000])
      for (const width of [4, 16]) {
        const scalar = run(rate, width, 'scalar', true),
          simd = run(rate, width, 'simd', true);
        expect(simd.latency).toBe(scalar.latency);
        let error = 0;
        for (let at = 0; at < scalar.audio.length; at++)
          error = Math.max(error, Math.abs(scalar.audio[at] - simd.audio[at]));
        expect(error).toBeLessThan(0.0002);
      }
    // Every stage must affect the signal, beyond merely being counted as active.
    for (let omitted = 0; omitted < pipeline.length; omitted++) {
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: pipeline.filter((_, index) => index !== omitted),
        })
      );
      const result = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '4', '48013', '0', 'scalar', '1000'],
        {
          encoding: 'utf8',
          maxBuffer: 8 * 1024 * 1024,
          env: {
            ...process.env,
            PATH: '/nonexistent',
            XDG_CONFIG_HOME: directory,
            XDG_CACHE_HOME: join(directory, 'cache'),
          },
        }
      );
      expect(result.status, result.stderr).toBe(0);
      const [active, , ...audio] = result.stdout
        .trim()
        .split(/\s+/)
        .map(Number);
      expect(active, pipeline[omitted].name).toBe(8);
      let difference = 0;
      for (let at = 32000; at < 47000; at++)
        difference += (audio[at] - cold.audio[at]) ** 2;
      expect(difference, pipeline[omitted].name).toBeGreaterThan(0.0001);
    }
    await writeFile(preset, JSON.stringify({ pipeline }));
  } finally {
    if (!exported) await rm(directory, { recursive: true, force: true });
  }
}, 120000);
