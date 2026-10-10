import { spawnSync } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, test } from 'vitest';
import { encodeWave } from '../support/wave';

const formats = [
  {
    name: 'float WAV and IRS',
    codec: 'pcm_f32le',
    container: 'wav',
    lossless: true,
    options: [],
  },
  {
    name: '24-bit WAV',
    codec: 'pcm_s24le',
    container: 'wav',
    lossless: true,
    options: [],
  },
  {
    name: 'AIFF',
    codec: 'pcm_s24be',
    container: 'aiff',
    lossless: true,
    options: [],
  },
  {
    name: 'FLAC',
    codec: 'flac',
    container: 'flac',
    lossless: true,
    options: [],
  },
  {
    name: 'MP3',
    codec: 'libmp3lame',
    container: 'mp3',
    lossless: false,
    options: ['-b:a', '320k'],
  },
  {
    name: 'Ogg Vorbis',
    codec: 'libvorbis',
    container: 'ogg',
    lossless: false,
    options: ['-q:a', '8'],
  },
  {
    name: 'M4A',
    codec: 'aac',
    container: 'mp4',
    lossless: false,
    options: ['-b:a', '256k', '-movflags', 'frag_keyframe+empty_moov'],
  },
];

test.each(formats)(
  '$name decodes at the original rate with independent channel content',
  async (format) => {
    const directory = await mkdtemp(join(tmpdir(), 'pipetune-audio-'));
    try {
      const rate = 48000;
      const channels = [440, 1700].map((frequency, channel) =>
        Float32Array.from(
          { length: 8192 },
          (_, frame) =>
            Math.sin((2 * Math.PI * frequency * frame) / rate) *
            (channel === 0 ? 0.5 : 0.25)
        )
      );
      // The executable creates test fixtures only; the product driver links native libraries.
      const fixture = join(directory, 'source.' + format.container);
      const encoded = spawnSync(
        process.env.PIPETUNE_FFMPEG_EXECUTABLE!,
        [
          '-hide_banner',
          '-loglevel',
          'error',
          '-f',
          'wav',
          '-i',
          'pipe:0',
          '-c:a',
          format.codec,
          ...format.options,
          '-f',
          format.container,
          fixture,
        ],
        { input: encodeWave(channels, rate), maxBuffer: 8 * 1024 * 1024 }
      );
      expect(encoded.status, encoded.stderr.toString()).toBe(0);
      const original = await readFile(fixture);
      const decoded = spawnSync(
        process.env.PIPETUNE_AUDIO_DRIVER!,
        [String(64 * 1024 * 1024), '0'],
        { input: original, maxBuffer: 8 * 1024 * 1024 }
      );
      expect(decoded.status, decoded.stderr.toString()).toBe(0);
      const line = decoded.stdout.indexOf(10);
      const [actualRate, width, frames] = decoded.stdout
        .subarray(0, line)
        .toString()
        .split(' ')
        .map(Number);
      const pcm = decoded.stdout.subarray(line + 1);
      expect(actualRate).toBe(rate);
      expect(width).toBe(2);
      expect(pcm.length).toBe(frames * width * 4);
      if (format.lossless) {
        expect(frames).toBe(8192);
        let error = 0;
        for (let channel = 0; channel < width; channel++)
          for (let frame = 0; frame < frames; frame++)
            error = Math.max(
              error,
              Math.abs(
                pcm.readFloatLE((channel * frames + frame) * 4) -
                  channels[channel][frame]
              )
            );
        expect(error).toBeLessThan(2e-7);
      } else {
        expect(frames).toBeGreaterThan(8000);
        expect(frames).toBeLessThan(11000);
        for (let channel = 0; channel < width; channel++) {
          const frequency = channel === 0 ? 440 : 1700;
          let real = 0;
          let imaginary = 0;
          const count = 4096;
          for (let frame = 0; frame < count; frame++) {
            const value = pcm.readFloatLE(
              (channel * frames + 2048 + frame) * 4
            );
            real += value * Math.cos((2 * Math.PI * frequency * frame) / rate);
            imaginary +=
              value * Math.sin((2 * Math.PI * frequency * frame) / rate);
          }
          expect(
            Math.abs(
              (Math.hypot(real, imaginary) * 2) / count -
                (channel === 0 ? 0.5 : 0.25)
            )
          ).toBeLessThan(0.03);
        }
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);

test.each([
  {
    name: 'non-finite PCM',
    channels: [Float32Array.of(1, NaN, 0)],
    budget: 1024,
  },
  {
    name: 'too many channels',
    channels: Array.from({ length: 17 }, () => Float32Array.of(1)),
    budget: 1024,
  },
  {
    name: 'decoded storage overflow',
    channels: [new Float32Array(4096)],
    budget: 64,
  },
])('rejects $name without publishing PCM', (scenario) => {
  const result = spawnSync(
    process.env.PIPETUNE_AUDIO_DRIVER!,
    [String(scenario.budget), '0'],
    { input: encodeWave(scenario.channels, 48000) }
  );
  expect(result.status).not.toBe(0);
  expect(result.stdout.length).toBe(0);
});

test.each([
  [44100, 48000],
  [48000, 44100],
  [192000, 48000],
  [1000, 384000],
])(
  'resampling from %i to %i preserves duration, channel order, and passband gain',
  (sourceRate, targetRate) => {
    const sourceFrames = sourceRate;
    const frequency = Math.min(1000, sourceRate * 0.1);
    const channels = [0.5, 0.25].map((gain) =>
      Float32Array.from(
        { length: sourceFrames },
        (_, frame) =>
          gain * Math.sin((2 * Math.PI * frequency * frame) / sourceRate)
      )
    );
    const result = spawnSync(
      process.env.PIPETUNE_AUDIO_DRIVER!,
      [String(64 * 1024 * 1024), String(targetRate)],
      { input: encodeWave(channels, sourceRate), maxBuffer: 8 * 1024 * 1024 }
    );
    expect(result.status, result.stderr.toString()).toBe(0);
    const line = result.stdout.indexOf(10);
    const [rate, width, frames] = result.stdout
      .subarray(0, line)
      .toString()
      .split(' ')
      .map(Number);
    const pcm = result.stdout.subarray(line + 1);
    expect(rate).toBe(targetRate);
    expect(width).toBe(2);
    expect(frames).toBe(Math.round((sourceFrames * targetRate) / sourceRate));
    // Measure steady-state passband gain away from the finite signal's sinc transients.
    const skip = Math.round(targetRate * 0.2);
    let error = 0;
    for (let channel = 0; channel < 2; channel++)
      for (let frame = skip; frame < frames - skip; frame++) {
        const expected =
          (channel === 0 ? 0.5 : 0.25) *
          Math.sin((2 * Math.PI * frequency * frame) / targetRate);
        error = Math.max(
          error,
          Math.abs(pcm.readFloatLE((channel * frames + frame) * 4) - expected)
        );
      }
    expect(error).toBeLessThan(0.001);
  }
);
