import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  mkdir,
  mkdtemp,
  rename,
  rm,
  symlink,
  truncate,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test.each([
  'missing original',
  'wrong hash',
  'changed content',
  'path traversal',
  'symbolic link',
  'oversized original',
  'oversized index',
  'invalid ID',
  'true stereo width',
  'independent width',
  'quarter rate',
  'latency',
  'channel mode',
  'unavailable pair',
  'unavailable single',
])('rejects %s as a preset error', async (scenario) => {
  const directory = await mkdtemp(join(tmpdir(), 'pipetune-ir-invalid-'));
  try {
    const library = join(directory, 'effetune', 'ir-library');
    await mkdir(library, { recursive: true });
    const pcm = new Float32Array(512);
    pcm[0] = 1;
    const bytes = encodeWave([pcm], 48000);
    const sha256 = createHash('sha256').update(bytes).digest('hex');
    const id = sha256.slice(0, 24);
    const original = {
      role: 'single',
      fileName: 'input.wav',
      storageName: id + '.wav',
      sha256,
      byteLength: bytes.length,
    };
    const path = join(library, original.storageName);
    await writeFile(path, bytes);
    if (scenario === 'missing original') await rm(path);
    if (scenario === 'wrong hash') original.sha256 = '0'.repeat(64);
    if (scenario === 'changed content') {
      bytes[44] = 1;
      await writeFile(path, bytes);
    }
    if (scenario === 'path traversal') original.storageName = '../outside.wav';
    if (scenario === 'symbolic link') {
      const outside = join(directory, 'outside.wav');
      await rename(path, outside);
      await symlink(outside, path);
    }
    if (scenario === 'oversized original')
      await truncate(path, 64 * 1024 * 1024 + 1);
    const indexPath = join(library, 'index.json');
    await writeFile(
      indexPath,
      JSON.stringify({
        version: 1,
        entries: {
          [id]: {
            irId: id,
            composition: 'single',
            originals: [original],
            bytes: bytes.length,
            channels: 1,
            frames: 512,
            sampleRate: 48000,
            topology: 'mono',
            pathSummary: [],
            importedAt: '2026-10-10T00:00:00.000Z',
            analysis: {
              storageName: id + '.analysis',
              onsetFrame: 0,
              rt60: null,
              peakDb: 0,
            },
          },
        },
      })
    );
    if (scenario === 'oversized index')
      await truncate(indexPath, 32 * 1024 * 1024 + 1);
    const parameters = {
      ir: scenario === 'invalid ID' ? '../outside' : id,
      cm:
        scenario === 'true stereo width'
          ? 'true'
          : scenario === 'independent width'
            ? 'indep'
            : scenario === 'channel mode'
              ? 'unknown'
              : 'auto',
      cr: scenario === 'quarter rate' ? 'quarter' : 'full',
      lt: scenario === 'latency' ? '33' : '128',
      dw: 0,
      de: false,
    };
    const preset = join(directory, 'invalid.effetune_preset');
    await writeFile(
      preset,
      JSON.stringify({
        pipeline: [
          {
            name: 'IR Reverb',
            parameters,
            channel:
              scenario === 'unavailable pair'
                ? '34'
                : scenario === 'unavailable single'
                  ? '16'
                  : 'A',
          },
        ],
      })
    );
    const result = spawnSync(
      process.env.PIPETUNE_ASSET_DRIVER!,
      [preset, '48000', '2', '1024', '0'],
      {
        encoding: 'utf8',
        env: {
          ...process.env,
          XDG_CONFIG_HOME: directory,
          XDG_CACHE_HOME: join(directory, 'cache'),
        },
      }
    );
    expect(result.status, result.stderr).toBe(1);
    expect(result.stdout).toBe('');
    expect(result.stderr).toMatch(/IR|asset/);
    expect(result.stderr).not.toContain('requires external asset loading');
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
