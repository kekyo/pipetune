import { spawnSync } from 'node:child_process';
import {
  mkdir,
  mkdtemp,
  readdir,
  readFile,
  rm,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test('registered SFZ instruments prepare, reuse and recover without external commands', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'pipetune-registered-sfz-'));
  try {
    const root = join(directory, 'instrument');
    const data = join(directory, 'effetune');
    const cache = join(directory, 'cache');
    await mkdir(join(root, 'banks'), { recursive: true });
    await mkdir(join(root, 'samples'));
    await mkdir(data);
    const id = '0123456789abcdef01234567';
    const path = join(root, 'banks', 'main.sfz');
    const included = join(root, 'banks', 'part.sfz');
    const sample = join(root, 'samples', 'instrument.wav');
    const registry = join(data, 'sfz-references.json');
    const entry = { id, name: 'Registered native instrument', root, path };
    await writeFile(registry, JSON.stringify([entry]));
    await writeFile(
      path,
      '<control> default_path=../samples/\n#include "part.sfz"'
    );
    const definition =
      '<region> sample=instrument.wav key=83 amp_veltrack=0 loop_mode=loop_continuous';
    await writeFile(included, definition);
    const wave = (frequency: number) =>
      encodeWave(
        [
          Float32Array.from(
            { length: 4800 },
            (_, frame) =>
              0.2 * Math.sin((2 * Math.PI * frequency * frame) / 48000)
          ),
        ],
        48000
      );
    await writeFile(sample, wave(700));
    const preset = join(directory, 'sfz.effetune_preset');
    const parameters = { sf: id, dm: 0, wm: 100, th: 0.01, mn: 83, mx: 83 };
    await writeFile(
      preset,
      JSON.stringify({ pipeline: [{ name: 'SFZ Note Player', parameters }] })
    );
    let limits: NodeJS.ProcessEnv = {};
    const run = (rate: number, backend: string) => {
      const native = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, String(rate), '2', String(rate / 2), '0', backend, '1000'],
        {
          encoding: 'utf8',
          maxBuffer: 8 * 1024 * 1024,
          env: {
            ...process.env,
            PATH: '/nonexistent',
            XDG_CONFIG_HOME: directory,
            XDG_CACHE_HOME: cache,
            ...limits,
          },
        }
      );
      const [active, latency, ...audio] = native.stdout
        .trim()
        .split(/\s+/)
        .map(Number);
      return { ...native, active, latency, audio };
    };
    const amplitude = (audio: number[], rate: number, frequency: number) => {
      let sine = 0;
      let cosine = 0;
      const first = Math.floor(rate / 3),
        end = Math.floor(rate / 2);
      for (let frame = first; frame < end; ++frame) {
        sine +=
          audio[frame] * Math.sin((2 * Math.PI * frequency * frame) / rate);
        cosine +=
          audio[frame] * Math.cos((2 * Math.PI * frequency * frame) / rate);
      }
      return (2 * Math.hypot(sine, cosine)) / (end - first);
    };
    const cold = run(48000, 'scalar');
    expect(cold.status, cold.stderr).toBe(0);
    expect(cold.active, cold.stderr).toBe(1);
    expect(cold.latency).toBeGreaterThan(0);
    expect(cold.stderr).toContain('cacheHits=0');
    expect(amplitude(cold.audio, 48000, 700)).toBeGreaterThan(0.05);
    expect(amplitude(cold.audio, 48000, 1000)).toBeLessThan(0.01);
    const warm = run(48000, 'scalar');
    expect(warm.status, warm.stderr).toBe(0);
    expect(warm.stderr).toContain('cacheHits=1');
    expect(warm.audio).toEqual(cold.audio);
    const cacheDirectory = join(cache, 'pipetune', 'assets-v1');
    const cachePath = join(cacheDirectory, (await readdir(cacheDirectory))[0]);
    limits = {
      PIPETUNE_TEST_SFZ_BYTES: String(1024 ** 3),
      PIPETUNE_TEST_ASSET_MEMORY_BYTES: String(256 * 1024 * 1024),
    };
    const bounded = run(48000, 'scalar');
    expect(bounded.status, bounded.stderr).toBe(0);
    expect(bounded.audio).toEqual(cold.audio);
    expect(run(48000, 'scalar').stderr).toContain('cacheHits=1');
    limits = { PIPETUNE_TEST_SFZ_BYTES: '19912' };
    const exact = run(48000, 'scalar');
    expect(exact.status, exact.stderr).toBe(0);
    expect(exact.audio).toEqual(cold.audio);
    limits = { PIPETUNE_TEST_SFZ_BYTES: '19911' };
    expect(run(48000, 'scalar').status).not.toBe(0);
    limits = { PIPETUNE_TEST_ASSET_MEMORY_BYTES: '1' };
    expect(run(48000, 'scalar').stderr).toContain(
      'combined asset memory budget'
    );
    limits = {};
    const switched = run(96000, 'simd');
    expect(switched.status, switched.stderr).toBe(0);
    expect(switched.stderr).toContain('cacheHits=1');
    expect(amplitude(switched.audio, 96000, 700)).toBeGreaterThan(0.05);
    const damaged = await readFile(cachePath);
    damaged[damaged.length - 1] ^= 1;
    await writeFile(cachePath, damaged);
    const recovered = run(48000, 'scalar');
    expect(recovered.status, recovered.stderr).toBe(0);
    expect(recovered.stderr).toContain('cacheHits=0');
    expect(recovered.audio).toEqual(cold.audio);
    await writeFile(included, definition + ' volume=-6');
    const quieter = run(48000, 'scalar');
    expect(quieter.status, quieter.stderr).toBe(0);
    expect(quieter.stderr).toContain('cacheHits=0');
    expect(
      amplitude(quieter.audio, 48000, 700) / amplitude(cold.audio, 48000, 700)
    ).toBeCloseTo(10 ** (-6 / 20), 3);
    await writeFile(sample, wave(500));
    const changed = run(48000, 'scalar');
    expect(changed.status, changed.stderr).toBe(0);
    expect(changed.stderr).toContain('cacheHits=0');
    expect(amplitude(changed.audio, 48000, 500)).toBeGreaterThan(0.025);
    await rm(sample);
    expect(run(48000, 'scalar').status).not.toBe(0);
    await writeFile(sample, wave(500));
    expect(run(48000, 'scalar').stderr).toContain('cacheHits=1');
    await writeFile(
      registry,
      JSON.stringify([{ ...entry, id: '111111111111111111111111' }])
    );
    expect(run(48000, 'scalar').status).not.toBe(0);
    await writeFile(registry, JSON.stringify([entry]));
    expect(run(48000, 'scalar').status).toBe(0);
    await rm(included);
    expect(run(48000, 'scalar').status).not.toBe(0);
    await writeFile(included, definition);
    expect(run(48000, 'scalar').status).toBe(0);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
