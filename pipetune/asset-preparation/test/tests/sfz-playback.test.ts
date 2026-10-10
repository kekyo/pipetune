import { spawnSync } from 'node:child_process';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test('a decoded WAV in a native SFZ bank produces instrument audio from detected input notes', () => {
  const instrument = Float32Array.from(
    { length: 4800 },
    (_, index) => Math.sin((2 * Math.PI * 700 * index) / 48000) * 0.2
  );
  const original = encodeWave([instrument], 48000);
  const definition =
    '<region> sample=instrument.wav key=83 amp_veltrack=0 loop_mode=loop_continuous';
  const driver = process.env.PIPETUNE_SFZ_PLAYBACK_DRIVER!;
  expect(driver).toBeTruthy();
  const native = spawnSync(driver, [definition], {
    input: original,
    maxBuffer: 4 * 1024 * 1024,
    env: { ...process.env, PATH: '/nonexistent' },
  });
  expect(
    native.status,
    native.error?.message ?? native.stderr?.toString()
  ).toBe(0);
  const end = native.stdout.indexOf(10);
  const metadata = JSON.parse(native.stdout.subarray(0, end).toString());
  expect(metadata.latencyFrames).toBeGreaterThan(0);
  expect(metadata.assetActive).toBe(true);
  const bytes = native.stdout.subarray(end + 1);
  const output = Float32Array.from({ length: bytes.length / 4 }, (_, index) =>
    bytes.readFloatLE(index * 4)
  );
  const tail = output.subarray(16000, 24000);
  const amplitude = (frequency: number) => {
    let real = 0;
    let imaginary = 0;
    for (let index = 0; index < tail.length; ++index) {
      const phase = (2 * Math.PI * frequency * index) / 48000;
      real += tail[index] * Math.cos(phase);
      imaginary += tail[index] * Math.sin(phase);
    }
    return (Math.hypot(real, imaginary) * 2) / tail.length;
  };
  expect(output.every(Number.isFinite)).toBe(true);
  expect(amplitude(700)).toBeGreaterThan(0.05);
  expect(amplitude(1000)).toBeLessThan(amplitude(700) / 10);
});
