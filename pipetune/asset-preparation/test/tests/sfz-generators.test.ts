import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

test.each([
  '*sine',
  '*saw',
  '*square',
  '*triangle',
  '*tri',
  '*noise',
  '*silence',
])(
  'SFZ %s generates the upstream sample rate and deterministic PCM',
  async (sample) => {
    const upstream = await import(
      /* @vite-ignore */ pathToFileURL(
        `${process.env.PIPETUNE_EFFETUNE_SOURCE}/js/sfz/generators.js`
      ).href
    );
    const expected = upstream.createSfzGeneratorSample(sample);
    const run = () =>
      spawnSync(process.env.PIPETUNE_SFZ_DRIVER!, ['--generator', sample], {
        maxBuffer: 1024 * 1024,
      });
    const native = run();
    expect(native.status, native.stderr.toString()).toBe(0);
    const end = native.stdout.indexOf(10);
    expect(end).toBeGreaterThan(0);
    expect(JSON.parse(native.stdout.subarray(0, end).toString())).toEqual({
      sampleRate: expected.sampleRate,
      channels: 1,
      frames: expected.channels[0].length,
    });
    const bytes = native.stdout.subarray(end + 1);
    expect(bytes.length).toBe(expected.channels[0].byteLength);
    for (let frame = 0; frame < expected.channels[0].length; ++frame) {
      expect(
        Math.abs(bytes.readFloatLE(frame * 4) - expected.channels[0][frame])
      ).toBeLessThanOrEqual(1e-7);
    }
    const repeated = run();
    expect(repeated.status, repeated.stderr.toString()).toBe(0);
    expect(repeated.stdout.equals(native.stdout)).toBe(true);
  }
);
