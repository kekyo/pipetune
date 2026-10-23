import { spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test.each(['*sine', '*noise', '*silence'])(
  'registered SFZ %s prepares generated and mixed banks without external commands',
  async (sample) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-sfz-generated-'));
    try {
      const root = join(directory, 'instrument');
      const data = join(directory, 'effetune');
      const cache = join(directory, 'cache');
      await mkdir(join(root, 'banks'), { recursive: true });
      await mkdir(data);
      const path = join(root, 'banks', 'main.sfz');
      const id = '0123456789abcdef01234567';
      await writeFile(
        join(data, 'sfz-references.json'),
        JSON.stringify([{ id, name: 'Generated sample', root, path }])
      );
      await writeFile(
        path,
        `<control> default_path=../absent/\n<region> sample=${sample} key=83 amp_veltrack=0`
      );
      const preset = join(directory, 'generated.effetune_preset');
      await writeFile(
        preset,
        JSON.stringify({
          pipeline: [
            {
              name: 'SFZ Note Player',
              parameters: { sf: id, dm: 0, wm: 100, th: 0.01, mn: 83, mx: 83 },
            },
          ],
        })
      );
      const run = (maximumBytes: number) => {
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
              XDG_CACHE_HOME: cache,
              PIPETUNE_TEST_SFZ_BYTES: String(maximumBytes),
            },
          }
        );
        const [active, latency, ...audio] = native.stdout
          .trim()
          .split(/\s+/)
          .map(Number);
        return { ...native, active, latency, audio };
      };
      const cold = run(256 * 1024 * 1024);
      expect(cold.status, cold.stderr).toBe(0);
      expect(cold.active).toBe(1);
      expect(cold.latency).toBeGreaterThan(0);
      expect(cold.audio.length).toBe(48026);
      expect(cold.audio.every(Number.isFinite)).toBe(true);
      expect(cold.stderr).toContain('cacheHits=0');
      const tail = cold.audio.slice(16000, 24000);
      if (sample === '*silence')
        expect(tail.every((value) => value === 0)).toBe(true);
      else
        expect(
          Math.sqrt(
            tail.reduce((sum, value) => sum + value * value, 0) / tail.length
          )
        ).toBeGreaterThan(0.05);
      const warm = run(256 * 1024 * 1024);
      expect(warm.status, warm.stderr).toBe(0);
      expect(warm.stderr).toContain('cacheHits=1');
      expect(warm.audio).toEqual(cold.audio);
      const footprint = sample === '*noise' ? 33480 : 8904;
      const bounded = run(footprint);
      expect(bounded.status, bounded.stderr).toBe(0);
      expect(bounded.audio).toEqual(cold.audio);
      expect(run(footprint - 1).status).not.toBe(0);

      await writeFile(
        join(root, 'instrument.wav'),
        encodeWave(
          [
            Float32Array.from(
              { length: 20000 },
              (_, frame) => 0.2 * Math.sin((2 * Math.PI * 700 * frame) / 48000)
            ),
          ],
          48000
        )
      );
      await writeFile(
        path,
        `<region> sample=${sample} key=83 amp_veltrack=0\n` +
          '<region> sample=../instrument.wav key=83 amp_veltrack=0 loop_mode=loop_continuous'
      );
      const mixed = run(256 * 1024 * 1024);
      expect(mixed.status, mixed.stderr).toBe(0);
      const difference = mixed.audio
        .slice(16000, 24000)
        .map((value, index) => value - cold.audio[index + 16000]);
      const differenceRms = Math.sqrt(
        difference.reduce((sum, value) => sum + value * value, 0) /
          difference.length
      );
      expect(differenceRms).toBeGreaterThan(0.05);
      const reduced = run(footprint);
      expect(reduced.status, reduced.stderr).toBe(0);
      expect(reduced.stderr).toContain(
        'SFZ reduced-bank: 1; layers were reduced'
      );
      expect(reduced.audio).toEqual(cold.audio);

      await writeFile(
        path,
        `<region> sample=${sample} key=83 amp_veltrack=0 ` +
          'ampeg_attack=-1 ampeg_hold=NaN ampeg_decay=Infinity ampeg_release=-1'
      );
      const adjusted = run(256 * 1024 * 1024);
      expect(adjusted.status, adjusted.stderr).toBe(0);
      expect(adjusted.stderr).toContain(
        'SFZ envelope-times-adjusted: 4; envelope times were adjusted to supported values'
      );
      expect(adjusted.stderr).not.toContain('invalid-regions');
      const adjustedWarm = run(256 * 1024 * 1024);
      expect(adjustedWarm.status, adjustedWarm.stderr).toBe(0);
      expect(adjustedWarm.stderr).toContain('cacheHits=1');
      expect(adjustedWarm.stderr).toContain(
        'SFZ envelope-times-adjusted: 4; envelope times were adjusted to supported values'
      );
      expect(adjustedWarm.audio).toEqual(adjusted.audio);
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
