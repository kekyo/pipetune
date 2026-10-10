import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

test.each([
  'wave',
  'wave-junk',
  'wave-odd',
  'wave-missing-format',
  'wave-no-frames',
  'wave-scan-limit',
  'wave-count-limit',
  'aiff',
  'flac',
  'flac-zero-frames',
  'flac-invalid-length',
  'compressed',
  'truncated',
])('SFZ header estimates match app: %s', async (scenario) => {
  let bytes = encodeWave([new Float32Array(100), new Float32Array(100)], 44100);
  if (
    scenario === 'wave-junk' ||
    scenario === 'wave-odd' ||
    scenario === 'wave-scan-limit' ||
    scenario === 'wave-count-limit'
  ) {
    const size =
      scenario === 'wave-scan-limit'
        ? 1024 * 1024
        : scenario === 'wave-odd'
          ? 3
          : 32;
    const chunk = Buffer.alloc(8 + size + (size & 1));
    chunk.write('JUNK');
    chunk.writeUInt32LE(size, 4);
    bytes = Buffer.concat([
      bytes.subarray(0, 12),
      ...Array.from(
        { length: scenario === 'wave-count-limit' ? 128 : 1 },
        () => chunk
      ),
      bytes.subarray(12),
    ]);
    bytes.writeUInt32LE(bytes.length - 8, 4);
  }
  if (scenario === 'wave-missing-format') bytes.write('JUNK', 12);
  if (scenario === 'wave-no-frames') bytes.writeUInt32LE(0, 40);
  if (scenario === 'aiff') {
    bytes = Buffer.alloc(38);
    bytes.write('FORM');
    bytes.writeUInt32BE(30, 4);
    bytes.write('AIFFCOMM', 8);
    bytes.writeUInt32BE(18, 16);
    bytes.writeUInt16BE(2, 20);
    bytes.writeUInt32BE(1234, 22);
    bytes.writeUInt16BE(24, 26);
    bytes.writeUInt16BE(16383 + 15, 28);
    bytes.writeUInt32BE(0xac440000, 30);
  }
  if (scenario.startsWith('flac')) {
    bytes = Buffer.alloc(42);
    bytes.write('fLaC');
    bytes[4] = 0x80;
    bytes[7] = scenario === 'flac-invalid-length' ? 35 : 34;
    const packed =
      (96000n << 44n) |
      (1n << 41n) |
      (23n << 36n) |
      (scenario === 'flac-zero-frames' ? 0n : 0x123456789n);
    bytes.writeBigUInt64BE(packed, 18);
  }
  if (scenario === 'compressed')
    bytes = Buffer.from('OggS compressed audio is intentionally unknown');
  if (scenario === 'truncated') bytes = bytes.subarray(0, 27);
  const upstream = await import(
    /* @vite-ignore */ pathToFileURL(
      process.env.PIPETUNE_EFFETUNE_SOURCE + '/js/sfz/bank.js'
    ).href
  );
  const expected = await upstream.readSfzAudioHeader(
    async (position: number, length: number) =>
      bytes.subarray(position, position + length),
    bytes.length
  );
  const actual = spawnSync(
    process.env.PIPETUNE_SFZ_DRIVER!,
    ['--audio-header'],
    { input: bytes }
  );
  expect(actual.status, actual.stderr.toString()).toBe(0);
  expect(JSON.parse(actual.stdout.toString())).toEqual(expected);
});
