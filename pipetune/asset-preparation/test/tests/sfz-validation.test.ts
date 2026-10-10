import { spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test('SFZ registration, confinement and partial banks retain explicit diagnostics', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'pipetune-sfz-validation-'));
  try {
    const root = join(directory, 'instrument');
    const data = join(directory, 'effetune');
    const cache = join(directory, 'cache');
    await mkdir(root);
    await mkdir(data);
    const id = '0123456789abcdef01234567';
    const path = join(root, 'main.sfz');
    const sample = join(root, 'a.wav');
    const registry = join(data, 'sfz-references.json');
    const entry = { id, name: 'Native instrument', root, path };
    const preset = join(directory, 'test.effetune_preset');
    const pcm = Float32Array.from(
      { length: 128 },
      (_, frame) => Math.sin(frame) * 0.2
    );
    const wave = encodeWave([pcm], 48000);
    const definition = '<region> sample=a.wav key=60 amp_veltrack=0';
    await writeFile(sample, wave);
    await writeFile(path, definition);
    await writeFile(
      preset,
      JSON.stringify({
        pipeline: [{ name: 'SFZ Note Player', parameters: { sf: id } }],
      })
    );
    const run = () =>
      spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '2', '256', '0'],
        {
          encoding: 'utf8',
          env: {
            ...process.env,
            PATH: '/nonexistent',
            XDG_CONFIG_HOME: directory,
            XDG_CACHE_HOME: cache,
          },
        }
      );
    for (const value of [
      '{',
      '{}',
      JSON.stringify(Array(10001).fill({})),
      ' '.repeat(1024 * 1024 + 1),
      JSON.stringify([{ ...entry, root: 'relative' }]),
      JSON.stringify([{ ...entry, path: join(directory, 'outside.sfz') }]),
      JSON.stringify([{ ...entry, path: join(root, 'bank.sfzbank') }]),
      JSON.stringify([{ ...entry, name: 10 }]),
      JSON.stringify([{ ...entry, id: id.toUpperCase() }]),
      JSON.stringify([{ ...entry, root: root + '\0' }]),
    ]) {
      await writeFile(registry, value);
      const rejected = run();
      expect(rejected.status, rejected.stderr).not.toBe(0);
      expect(rejected.stderr).toContain('SFZ');
    }
    await writeFile(registry, JSON.stringify([entry]));
    const valid = run();
    expect(valid.status, valid.stderr).toBe(0);
    expect(valid.stdout.startsWith('1 ')).toBe(true);
    for (const text of [
      '#include "absent.sfz"',
      '#include "main.sfz"',
      '<region> sample=../outside.wav key=60',
      '<region> sample=/outside.wav key=60',
      '<region> sample=absent.wav key=60',
      '<region> sample=a.wav key=60 trigger=release',
      '<region> sample=a.wav key=200',
      '<region> sample=a.wav offset=999999 key=60',
    ]) {
      await writeFile(path, text);
      const rejected = run();
      expect(rejected.status, text + ': ' + rejected.stderr).not.toBe(0);
    }
    await writeFile(path, Buffer.from([0xc3, 0x28]));
    expect(run().status).not.toBe(0);
    const partial =
      definition +
      '\n<region> sample=absent.wav key=61\n<region> sample=a.wav key=62 trigger=release\n<region> sample=a.wav key=200';
    await writeFile(path, partial);
    const usable = run();
    expect(usable.status, usable.stderr).toBe(0);
    expect(usable.stdout.startsWith('1 ')).toBe(true);
    for (const code of [
      'missing-samples: 1',
      'unsupported-regions: 1',
      'invalid-regions: 1',
    ]) {
      expect(usable.stderr).toContain(code);
      expect(run().stderr).toContain(code);
    }
    await writeFile(path, definition);
    await writeFile(sample, encodeWave([pcm, pcm, pcm], 48000));
    expect(run().status).not.toBe(0);
    await writeFile(
      sample,
      encodeWave([Float32Array.from([0, NaN, Infinity])], 48000)
    );
    expect(run().status).not.toBe(0);
    await writeFile(sample, 'undecodable source');
    expect(run().status).not.toBe(0);
    await writeFile(sample, wave);
    const link = join(root, 'link.wav');
    await symlink('a.wav', link);
    await writeFile(path, '<region> sample=link.wav key=60');
    expect(run().status).toBe(0);
    await rm(link);
    await writeFile(join(directory, 'outside.wav'), wave);
    await symlink('../outside.wav', link);
    expect(run().status).not.toBe(0);
    await rm(link);
    await symlink('link.wav', link);
    expect(run().status).not.toBe(0);
    await rm(link);
    await writeFile(path, definition);
    await mkdir(cache, { recursive: true });
    await rm(join(cache, 'pipetune'), { recursive: true, force: true });
    await writeFile(join(cache, 'pipetune'), 'blocks cache directory creation');
    const noCache = run();
    expect(noCache.status, noCache.stderr).toBe(0);
    expect(noCache.stderr).toContain('cache could not be saved');
    // No external references are resolved for an unloaded or disabled node.
    await rm(registry);
    await writeFile(
      preset,
      JSON.stringify({
        pipeline: [{ name: 'SFZ Note Player', parameters: {} }],
      })
    );
    const unassigned = run();
    expect(unassigned.status, unassigned.stderr).toBe(0);
    expect(unassigned.stdout.startsWith('1 ')).toBe(true);
    expect(unassigned.stderr).toContain('No SFZ instrument is assigned');
    await writeFile(
      preset,
      JSON.stringify({
        pipeline: [
          { name: 'SFZ Note Player', enabled: false, parameters: { sf: id } },
        ],
      })
    );
    const disabled = run();
    expect(disabled.status, disabled.stderr).toBe(0);
    expect(disabled.stdout.startsWith('0 ')).toBe(true);
    expect(disabled.stderr).not.toContain('SFZ');
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
