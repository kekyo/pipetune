import { spawnSync } from 'node:child_process';
import { expect, test } from 'vitest';

test('coprime measurement rates keep resampling work memory bounded', () => {
  const chunks: Buffer[] = [];
  const u32 = (value: number) => {
    const bytes = Buffer.alloc(4);
    bytes.writeUInt32LE(value);
    chunks.push(bytes);
  };
  const f64 = (value: number) => {
    const bytes = Buffer.alloc(8);
    bytes.writeDoubleLE(value);
    chunks.push(bytes);
  };
  u32(48000);
  u32(8192);
  u32(0);
  for (const value of [0.17, 20, 16000, 6, 1]) f64(value);
  u32(0);
  u32(1);
  u32(1);
  u32(0);
  u32(1);
  u32(0);
  u32(767999);
  u32(10000);
  f64(1);
  const data = new Float32Array(600000);
  data[10000] = 1;
  u32(data.length);
  chunks.push(Buffer.from(data.buffer));
  const result = spawnSync(
    '/bin/bash',
    [
      '-c',
      'ulimit -v 196608 && exec "$@"',
      'room-memory',
      process.env.PIPETUNE_ROOM_DRIVER!,
      '--design-ir',
    ],
    { input: Buffer.concat(chunks), maxBuffer: 4 * 1024 * 1024 }
  );
  expect(result.status, result.stderr?.toString()).toBe(0);
  const newline = result.stdout.indexOf(10),
    pcm = result.stdout.subarray(newline + 1);
  expect(pcm.length).toBe(8192 * 4);
  let energy = 0;
  for (let index = 0; index < 8192; index++) {
    const sample = pcm.readFloatLE(index * 4);
    expect(Number.isFinite(sample)).toBe(true);
    energy += sample * sample;
  }
  expect(energy).toBeGreaterThan(0.1);
}, 120000);
