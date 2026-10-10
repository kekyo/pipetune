import { spawnSync } from 'node:child_process';
import {
  mkdir,
  mkdtemp,
  readdir,
  readFile,
  rm,
  writeFile,
  rename,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, test } from 'vitest';

test('Room EQ cache follows sources, settings, rates and processing width without Node', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'pipetune-room-cache-'));
  try {
    const library = join(directory, 'effetune', 'measurement-backups');
    await mkdir(library, { recursive: true });
    const source = join(library, 'room.json'),
      preset = join(directory, 'room.effetune_preset');
    const data = new Float32Array(4096);
    data[32] = 1;
    data[400] = 0.55;
    const measurement = {
      id: 'room',
      points: [{ pointId: 0 }],
      averageFrequencyResponse: [
        [20, 0],
        [20000, 0],
      ],
      impulseResponses: [
        {
          measurementId: 'room',
          pointId: 0,
          onsetIndex: 32,
          sampleRate: 48000,
          refScale: 1,
          data: Buffer.from(data.buffer).toString('base64'),
        },
      ],
    };
    await writeFile(source, JSON.stringify(measurement));
    const base = { ms: 'room', pm: 'full', tp: 8192, fl: 20 };
    const run = async (
      parameters: object,
      rate: number,
      width: number,
      backend: string,
      cache: string
    ) => {
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            { name: 'Room EQ', enabled: true, channel: 'A', parameters },
          ],
        })
      );
      const result = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, String(rate), String(width), '8448', '0', backend],
        {
          encoding: 'utf8',
          maxBuffer: 8 * 1024 * 1024,
          env: {
            ...process.env,
            PATH: '/nonexistent',
            XDG_CONFIG_HOME: directory,
            XDG_CACHE_HOME: cache,
          },
        }
      );
      return {
        ...result,
        hit: Number(result.stderr.match(/cacheHits=(\d+)/)?.[1] ?? -1),
      };
    };
    const cache = join(directory, 'cache');
    const cold = await run(base, 48000, 2, 'scalar', cache);
    expect(cold.status, cold.stderr).toBe(0);
    expect(cold.hit).toBe(0);
    const warm = await run(base, 48000, 2, 'scalar', cache);
    expect(warm.status, warm.stderr).toBe(0);
    expect(warm.hit).toBe(1);
    expect(warm.stdout).toBe(cold.stdout);
    const simd = await run(base, 48000, 2, 'simd', cache);
    expect(simd.status, simd.stderr).toBe(0);
    expect(simd.hit).toBe(1);
    expect(
      (
        await run(
          { ...base, gn: -3, dl: 2, fd: 77, mn: 'renamed' },
          48000,
          2,
          'scalar',
          cache
        )
      ).hit
    ).toBe(1);
    for (const change of [
      { tp: 16384 },
      { pm: 'min' },
      { pm: 'lin' },
      { sm: 0.3 },
      { fl: 80 },
      { fh: 8000 },
      { mb: 0 },
      { cr: 0 },
      { dw: 20 },
      { pa: false, pl: 300 },
      { pr: 35 },
      { le: true },
      { rv: 30 },
      { rw: 50 },
      { rf: 500 },
      { rs: 0.4 },
      { pq: false, ps: 0.3 },
      { rp: 1 },
      { lt: '512' },
      { bs: [{ frequency: 800, gain: -3, q: 1, type: 'pk', enabled: true }] },
      { ms0: 'room' },
    ]) {
      const changed = await run(
        { ...base, ...change },
        48000,
        2,
        'scalar',
        cache
      );
      expect(changed.status, changed.stderr).toBe(0);
      expect(changed.hit, JSON.stringify(change)).toBe(0);
    }
    expect((await run(base, 96000, 2, 'scalar', cache)).hit).toBe(0);
    expect((await run(base, 48000, 4, 'scalar', cache)).hit).toBe(0);
    // Changing all original bytes is observed even if ID and metadata stay fixed.
    const modified = structuredClone(measurement);
    const changedPcm = Buffer.from(modified.impulseResponses[0].data, 'base64');
    changedPcm.writeFloatLE(-0.35, 400 * 4);
    modified.impulseResponses[0].data = changedPcm.toString('base64');
    await writeFile(source + '.new', JSON.stringify(modified));
    await rename(source + '.new', source);
    const changed = await run(base, 48000, 2, 'scalar', cache);
    expect(changed.status, changed.stderr).toBe(0);
    expect(changed.hit).toBe(0);
    expect(changed.stdout).not.toBe(cold.stdout);
    await rm(source);
    const missing = await run(base, 48000, 2, 'scalar', cache);
    expect(missing.status).toBe(1);
    await writeFile(source, JSON.stringify(measurement));
    expect((await run(base, 48000, 2, 'scalar', cache)).hit).toBe(1);
    const entries = join(cache, 'pipetune', 'assets-v1');
    for (const name of await readdir(entries)) {
      const path = join(entries, name),
        bytes = await readFile(path);
      bytes[bytes.length - 1] ^= 0x80;
      await writeFile(path, bytes);
    }
    const recovered = await run(base, 48000, 2, 'scalar', cache);
    expect(recovered.status, recovered.stderr).toBe(0);
    expect(recovered.hit).toBe(0);
    expect(recovered.stdout).toBe(cold.stdout);
    const blocked = join(directory, 'blocked');
    await writeFile(blocked, 'not a directory');
    const uncached = await run(base, 48000, 2, 'scalar', blocked);
    expect(uncached.status, uncached.stderr).toBe(0);
    expect(uncached.hit).toBe(0);
    expect(uncached.stderr).toContain('cache could not be saved');
    expect(uncached.stdout).toBe(cold.stdout);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}, 120000);
