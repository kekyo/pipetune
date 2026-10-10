import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
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

test('registered IRs reuse validated preparation and recover from cache failure without external commands', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'pipetune-ir-cache-'));
  try {
    const data = join(directory, 'effetune');
    const library = join(data, 'ir-library');
    const cache = join(directory, 'cache', 'pipetune');
    await mkdir(library, { recursive: true });
    const impulse = new Float32Array(512);
    impulse[0] = 1;
    impulse[64] = 0.5;
    const original = encodeWave([impulse], 48000);
    const sha256 = createHash('sha256').update(original).digest('hex');
    const ir = sha256.slice(0, 24);
    const source = join(library, ir + '.wav');
    await writeFile(source, original);
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
    const parameters = { ir, cm: 'mono', lt: '128', cr: 'full' };
    const run = (
      settings: Record<string, unknown>,
      cacheDirectory: string,
      rate: number,
      width: number
    ) => {
      const result = spawnSync(
        process.env.PIPETUNE_IR_CACHE_DRIVER!,
        [data, cacheDirectory, String(rate), String(width)],
        {
          input: JSON.stringify(settings),
          env: { ...process.env, PATH: '/nonexistent' },
          maxBuffer: 4 * 1024 * 1024,
        }
      );
      const header = result.stdout.indexOf(10);
      const [hit, head, frames] = result.stdout
        .subarray(0, header)
        .toString()
        .split(' ')
        .map(Number);
      return {
        status: result.status,
        error: result.stderr.toString(),
        hit,
        head,
        frames,
        payload: result.stdout.subarray(header + 1),
      };
    };
    const cold = run(parameters, cache, 48000, 2);
    expect(cold.status, cold.error).toBe(0);
    expect(cold.hit).toBe(0);
    expect(cold.payload.readFloatLE(32)).toBeCloseTo(1 / Math.sqrt(1.25), 6);
    const warm = run(parameters, cache, 48000, 2);
    expect(warm.status, warm.error).toBe(0);
    expect(warm.hit).toBe(1);
    expect(warm.payload).toEqual(cold.payload);
    expect(
      run(
        { ...parameters, de: false, dw: -6, dl: -12, pd: 20 },
        cache,
        48000,
        2
      ).hit
    ).toBe(1);
    const cachePath = join(
      cache,
      'assets-v1',
      (await readdir(join(cache, 'assets-v1')))[0]
    );
    const damaged = await readFile(cachePath);
    damaged[damaged.length - 1] ^= 0x80;
    await writeFile(cachePath, damaged);
    const recovered = run(parameters, cache, 48000, 2);
    expect(recovered.status, recovered.error).toBe(0);
    expect(recovered.hit).toBe(0);
    expect(recovered.payload).toEqual(cold.payload);
    expect(run(parameters, cache, 48000, 2).hit).toBe(1);
    const shorter = run({ ...parameters, tr: 25 }, cache, 48000, 2);
    expect(shorter.status, shorter.error).toBe(0);
    expect(shorter.hit).toBe(0);
    expect(shorter.frames).toBe(128);
    expect(shorter.payload).not.toEqual(cold.payload);
    expect(run({ ...parameters, lt: '512' }, cache, 48000, 2).head).toBe(512);
    const highRate = run(parameters, cache, 96000, 2);
    expect(highRate.status, highRate.error).toBe(0);
    expect(highRate.hit).toBe(0);
    expect(highRate.payload.readUInt32LE(12)).toBe(96000);
    expect(run(parameters, cache, 48000, 1).hit).toBe(0);
    await rm(source);
    expect(run(parameters, cache, 48000, 2).status).toBe(1);
    const changed = Buffer.from(original);
    changed[changed.length - 1] ^= 1;
    await writeFile(source, changed);
    expect(run(parameters, cache, 48000, 2).status).toBe(1);
    await writeFile(source, original);
    expect(run(parameters, cache, 48000, 2).hit).toBe(1);
    const unavailable = join(directory, 'blocked');
    await writeFile(unavailable, 'file');
    const uncached = run(parameters, unavailable, 48000, 2);
    expect(uncached.status, uncached.error).toBe(0);
    expect(uncached.hit).toBe(0);
    expect(uncached.payload).toEqual(cold.payload);
    expect(uncached.error).toContain('cache');

    const preset = join(directory, 'cached.effetune_preset');
    await writeFile(
      preset,
      JSON.stringify({
        pipeline: [
          {
            name: 'IR Reverb',
            parameters: { ...parameters, de: false, dw: 0 },
          },
        ],
      })
    );
    for (const cached of [true, false]) {
      const processed = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '2', '512', '0'],
        {
          encoding: 'utf8',
          env: {
            ...process.env,
            XDG_CONFIG_HOME: directory,
            XDG_CACHE_HOME: cached ? join(directory, 'cache') : unavailable,
            PATH: '/nonexistent',
          },
        }
      );
      expect(processed.status, processed.stderr).toBe(0);
      expect(processed.stderr).toContain(
        cached ? 'cacheHits=1' : 'cacheHits=0'
      );
      if (!cached)
        expect(processed.stderr).toContain('cache could not be saved');
      const [active, latency, ...audio] = processed.stdout
        .trim()
        .split(/\s+/)
        .map(Number);
      expect(active).toBe(1);
      expect(latency).toBe(128);
      expect(audio[128]).toBeCloseTo(1 / Math.sqrt(1.25), 6);
      expect(audio[192]).toBeCloseTo(0.5 / Math.sqrt(1.25), 6);
    }
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
