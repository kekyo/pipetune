import { spawnSync } from 'node:child_process';
import { expect, test } from 'vitest';

test('an oversized SFZ is rejected instead of reducing its layers', () => {
  const chunks: Buffer[] = [];
  const word = (value: number) => {
    const bytes = Buffer.alloc(4);
    bytes.writeUInt32LE(value);
    chunks.push(bytes);
  };
  const text = (value: string) => {
    const bytes = Buffer.from(value);
    word(bytes.length);
    chunks.push(bytes);
  };
  text('<region> sample=a.wav key=60\n<region> sample=b.wav key=60');
  word(2);
  for (const path of ['a.wav', 'b.wav']) {
    text(path);
    word(100);
    word(0);
    word(100);
    word(0);
    word(1);
  }
  const native = spawnSync(
    process.env.PIPETUNE_SFZ_DRIVER!,
    ['--select', '1500', '0'],
    { input: Buffer.concat(chunks) }
  );
  expect(native.status).not.toBe(0);
  expect(native.stderr.toString()).toBe(
    'too-large: The complete SFZ instrument exceeds the memory limit.\n'
  );
});
