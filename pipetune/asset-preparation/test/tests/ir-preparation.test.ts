import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

test.each(['impulse', 'silence'])(
  'default %s preparation matches the unmodified application JS payload',
  async (mode) => {
    const url = pathToFileURL(
      `${process.env.PIPETUNE_EFFETUNE_SOURCE}/js/ir-library/ir-preparation.js`
    ).href;
    const upstream = await import(/* @vite-ignore */ url);
    const impulse = new Float32Array(512);
    if (mode === 'impulse') {
      impulse[0] = 1;
      impulse[64] = 0.5;
    }
    const reference = upstream.prepareIr({
      channels: [impulse],
      sampleRate: 48000,
    });
    const driver = process.env.PIPETUNE_IR_DRIVER;
    expect(driver).toBeTruthy();
    const native = spawnSync(driver!, ['--payload', mode]);
    expect(native.status, native.stderr.toString()).toBe(0);
    expect(native.stdout).toEqual(Buffer.from(reference.payload));
  }
);
