import { spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test('registered SFZ rejects invalid bank path lists before preparing audio', async () => {
  const directory = await mkdtemp(
    join(tmpdir(), 'pipetune-sfz-bank-registry-')
  );
  try {
    const root = join(directory, 'instrument');
    const data = join(directory, 'effetune');
    await mkdir(root);
    await mkdir(data);
    const id = '0123456789abcdef01234567';
    const path = join(root, 'main.sfz');
    const entry = { id, name: 'Bank registry validation', root, path };
    const registry = join(data, 'sfz-references.json');
    await writeFile(path, '<region> sample=*silence key=60');
    const preset = join(directory, 'test.effetune_preset');
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
            XDG_CACHE_HOME: join(directory, 'cache'),
          },
        }
      );
    for (const bankPaths of [
      null,
      {},
      'valid.bank.xml',
      [null],
      ['wrong.xml'],
      ['../outside.bank.xml'],
      ['..\\outside.bank.xml'],
      [join(root, 'absolute.bank.xml')],
      ['bad\0.bank.xml'],
      Array(10001).fill('valid.bank.xml'),
    ]) {
      await writeFile(registry, JSON.stringify([{ ...entry, bankPaths }]));
      const rejected = run();
      expect(
        rejected.status,
        JSON.stringify(bankPaths) + ': ' + rejected.stderr
      ).not.toBe(0);
      expect(rejected.stderr).toContain('SFZ registry');
    }
    for (const bankPaths of [undefined, [], ['missing.BANK.XML']]) {
      await writeFile(
        registry,
        JSON.stringify([
          { ...entry, bankPaths, importedAt: 'ignored metadata' },
        ])
      );
      const accepted = run();
      expect(accepted.status, accepted.stderr).toBe(0);
    }
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

test('Windows-1252 SFZ and include files resolve their decoded sample names and rebuild', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'pipetune-sfz-legacy-text-'));
  try {
    const root = join(directory, 'instrument');
    const data = join(directory, 'effetune');
    await mkdir(root);
    await mkdir(data);
    const id = '0123456789abcdef01234567';
    const path = join(root, 'main.sfz');
    await writeFile(
      join(data, 'sfz-references.json'),
      JSON.stringify([{ id, name: 'Legacy text', root, path }])
    );
    await writeFile(path, Buffer.from('#include "pièce.sfz"', 'latin1'));
    const definition =
      '<region> sample=café.wav key=83 amp_veltrack=0 loop_mode=loop_continuous';
    const included = join(root, 'pièce.sfz');
    await writeFile(included, Buffer.from(definition, 'latin1'));
    await writeFile(
      join(root, 'café.wav'),
      encodeWave(
        [
          Float32Array.from(
            { length: 4800 },
            (_, frame) => 0.2 * Math.sin((2 * Math.PI * 700 * frame) / 48000)
          ),
        ],
        48000
      )
    );
    const preset = join(directory, 'test.effetune_preset');
    await writeFile(
      preset,
      JSON.stringify({
        pipeline: [
          {
            name: 'SFZ Note Player',
            parameters: { sf: id, dm: 0, wm: 100, mn: 83, mx: 83, th: 0.01 },
          },
        ],
      })
    );
    const run = () => {
      const native = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '2', '24013', '0', 'scalar', '1000'],
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
      return {
        ...native,
        audio: native.stdout.trim().split(/\s+/).map(Number).slice(2),
      };
    };
    const cold = run();
    expect(cold.status, cold.stderr).toBe(0);
    expect(cold.stderr).toContain('cacheHits=0');
    expect(cold.audio.every(Number.isFinite)).toBe(true);
    const tail = cold.audio.slice(16000, 24000);
    expect(
      Math.sqrt(
        tail.reduce((sum, value) => sum + value * value, 0) / tail.length
      )
    ).toBeGreaterThan(0.05);
    const warm = run();
    expect(warm.status, warm.stderr).toBe(0);
    expect(warm.stderr).toContain('cacheHits=1');
    expect(warm.audio).toEqual(cold.audio);
    await writeFile(included, Buffer.from(definition + ' volume=-6', 'latin1'));
    const changed = run();
    expect(changed.status, changed.stderr).toBe(0);
    expect(changed.stderr).toContain('cacheHits=0');
    for (let frame = 16000; frame < 24000; ++frame)
      expect(
        Math.abs(changed.audio[frame] - cold.audio[frame] * 10 ** (-6 / 20))
      ).toBeLessThan(1e-6);
    await writeFile(path, '\ufeff#include "pièce.sfz"');
    const utf8 = run();
    expect(utf8.status, utf8.stderr).toBe(0);
    expect(utf8.audio).toEqual(changed.audio);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

test('SFZ original-byte limit includes the selected bank and excludes rejected candidates', async () => {
  const directory = await mkdtemp(
    join(tmpdir(), 'pipetune-sfz-bank-capacity-')
  );
  try {
    const root = join(directory, 'instrument');
    const data = join(directory, 'effetune');
    await mkdir(root);
    await mkdir(data);
    const id = '0123456789abcdef01234567';
    const path = join(root, 'main.sfz');
    await writeFile(
      join(data, 'sfz-references.json'),
      JSON.stringify([
        {
          id,
          name: 'Bank capacity',
          root,
          path,
          bankPaths: ['a.bank.xml', 'z.bank.xml'],
        },
      ])
    );
    const definition = '<region> sample=$voice key=60';
    await writeFile(path, definition);
    await writeFile(
      join(root, 'a.bank.xml'),
      '<Unrelated/>' + ' '.repeat(6000)
    );
    const selected =
      '<AriaBank><AriaElement path="main.sfz"/>' +
      '<Define name="$voice" value="*silence"/>';
    await writeFile(join(root, 'z.bank.xml'), selected + ' '.repeat(6000));
    const preset = join(directory, 'test.effetune_preset');
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
            XDG_CACHE_HOME: join(directory, 'cache'),
            PIPETUNE_TEST_SFZ_BYTES: '8904',
          },
        }
      );
    const accepted = run();
    expect(accepted.status, accepted.stderr).toBe(0);
    await writeFile(
      join(root, 'z.bank.xml'),
      selected.padEnd(8905 - definition.length, ' ')
    );
    const rejected = run();
    expect(rejected.status).not.toBe(0);
    expect(rejected.stderr).toContain('byte budget');
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

test('ARIA bank candidates, definitions and registry changes invalidate prepared audio', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'pipetune-sfz-aria-bank-'));
  try {
    const root = join(directory, 'instrument');
    const data = join(directory, 'effetune');
    await mkdir(join(root, 'Banks', 'Programs'), { recursive: true });
    await mkdir(join(root, 'Samples'));
    await mkdir(data);
    const id = '0123456789abcdef01234567';
    const path = join(root, 'Banks', 'Programs', 'main.sfz');
    const registry = join(data, 'sfz-references.json');
    const entry = { id, name: 'ARIA instrument', root, path };
    await writeFile(
      registry,
      JSON.stringify([{ ...entry, bankPaths: ['z.bank.xml', 'a.bank.xml'] }])
    );
    await writeFile(path, Buffer.from('// pièce\n#include "$part"', 'latin1'));
    await writeFile(
      join(root, 'Banks', 'Programs', 'réglages.sfz'),
      Buffer.from(
        '<region> sample=$sample_dir/café.wav key=83 amp_veltrack=0 loop_mode=loop_continuous volume=$gain',
        'latin1'
      )
    );
    const sample = join(root, 'Samples', 'café.wav');
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
    const bank = (gain: number) =>
      Buffer.from(
        '<AriaBank>' +
          '<AriaElement path="banks/programs/MAIN.SFZ"/>' +
          '<Define name="$part" value="réglages.sfz"/>' +
          '<Define name="$sample_dir" value="$Banks/Installed"/>' +
          `<Define name="$gain" value="${gain}"/></AriaBank>`,
        'latin1'
      );
    const rejectedBank = join(root, 'a.bank.xml');
    const chosenBank = join(root, 'z.bank.xml');
    await writeFile(rejectedBank, '<Unrelated/>');
    await writeFile(chosenBank, bank(-6));
    const preset = join(directory, 'test.effetune_preset');
    await writeFile(
      preset,
      JSON.stringify({
        pipeline: [
          {
            name: 'SFZ Note Player',
            parameters: { sf: id, dm: 0, wm: 100, mn: 83, mx: 83, th: 0.01 },
          },
        ],
      })
    );
    const run = () => {
      const native = spawnSync(
        process.env.PIPETUNE_ASSET_DRIVER!,
        [preset, '48000', '2', '24013', '0', 'scalar', '1000'],
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
      return {
        ...native,
        audio: native.stdout.trim().split(/\s+/).map(Number).slice(2),
      };
    };
    const amplitude = (audio: number[], frequency: number) => {
      let sine = 0,
        cosine = 0;
      for (let frame = 16000; frame < 24000; ++frame) {
        sine +=
          audio[frame] * Math.sin((2 * Math.PI * frequency * frame) / 48000);
        cosine +=
          audio[frame] * Math.cos((2 * Math.PI * frequency * frame) / 48000);
      }
      return (2 * Math.hypot(sine, cosine)) / 8000;
    };
    const cold = run();
    expect(cold.status, cold.stderr).toBe(0);
    expect(cold.stderr).toContain('cacheHits=0');
    expect(amplitude(cold.audio, 700)).toBeGreaterThan(0.05);
    const warm = run();
    expect(warm.status, warm.stderr).toBe(0);
    expect(warm.stderr).toContain('cacheHits=1');
    expect(warm.audio).toEqual(cold.audio);

    await writeFile(rejectedBank, bank(-12));
    const reselection = run();
    expect(reselection.status, reselection.stderr).toBe(0);
    expect(reselection.stderr).toContain('cacheHits=0');
    expect(
      amplitude(reselection.audio, 700) / amplitude(cold.audio, 700)
    ).toBeCloseTo(10 ** (-6 / 20), 3);
    await rm(rejectedBank);
    const missingCandidate = run();
    expect(missingCandidate.status, missingCandidate.stderr).toBe(0);
    expect(missingCandidate.audio).toEqual(cold.audio);
    await writeFile(rejectedBank, '<Unrelated/>');
    expect(run().stderr).toContain('cacheHits=1');

    await writeFile(
      registry,
      JSON.stringify([{ ...entry, bankPaths: ['z.bank.xml'] }])
    );
    const changedList = run();
    expect(changedList.status, changedList.stderr).toBe(0);
    expect(changedList.stderr).toContain('cacheHits=0');
    expect(changedList.audio).toEqual(cold.audio);
    await writeFile(chosenBank, bank(0));
    const louder = run();
    expect(louder.status, louder.stderr).toBe(0);
    expect(louder.stderr).toContain('cacheHits=0');
    expect(
      amplitude(louder.audio, 700) / amplitude(cold.audio, 700)
    ).toBeCloseTo(10 ** (6 / 20), 3);
    await writeFile(sample, wave(500));
    const replacedSample = run();
    expect(replacedSample.status, replacedSample.stderr).toBe(0);
    expect(replacedSample.stderr).toContain('cacheHits=0');
    expect(amplitude(replacedSample.audio, 500)).toBeGreaterThan(0.05);

    await rm(chosenBank);
    expect(run().status).not.toBe(0);
    await writeFile(chosenBank, bank(0));
    expect(run().status).toBe(0);
    const outside = join(directory, 'outside.bank.xml');
    await writeFile(outside, bank(0));
    await rm(chosenBank);
    await symlink(outside, chosenBank);
    expect(run().status).not.toBe(0);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
