import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

const scenarios = [
  { name: 'single measured reflection', phase: 'min' },
  { name: 'linear measured reflection', phase: 'lin' },
  { name: 'multiple points use power averaging', phase: 'min', points: 3 },
  {
    name: 'reference scale changes the point power',
    phase: 'lin',
    points: 2,
    scaled: true,
  },
  { name: '44.1 kHz source at 48 kHz', phase: 'min', sourceRate: 44100 },
  { name: '48 kHz source at 44.1 kHz', phase: 'lin', sampleRate: 44100 },
  {
    name: '48 kHz source at 96 kHz',
    phase: 'min',
    sampleRate: 96000,
    points: 2,
  },
  {
    name: '96 kHz source at 44.1 kHz',
    phase: 'lin',
    sourceRate: 96000,
    sampleRate: 44100,
  },
].map((entry) => ({
  points: 1,
  scaled: false,
  sampleRate: 48000,
  sourceRate: 48000,
  ...entry,
}));

test.each(scenarios)(
  '$name matches application magnitude design from the original impulse data',
  async (scenario) => {
    const upstream = await import(
      /* @vite-ignore */ pathToFileURL(
        process.env.PIPETUNE_EFFETUNE_SOURCE + '/js/room-eq/design-core.js'
      ).href
    );
    const impulses = Array.from({ length: scenario.points }, (_, point) => {
      const data = new Float32Array(4099 + point * 31);
      const onsetIndex = 73 + point * 13;
      data[onsetIndex] = 1 + point;
      data[onsetIndex + 57 + point * 3] = 0.55 - point * 0.1;
      data[onsetIndex + 211] = -0.25;
      for (let index = 1; index < 500; index++)
        data[onsetIndex + index] +=
          0.02 *
          Math.sin(index * (0.19 + point * 0.03)) *
          Math.exp(-index / 130);
      return {
        data,
        onsetIndex,
        sampleRate: scenario.sourceRate,
        refScale: scenario.scaled ? point + 0.4 : 1,
        pointId: point * 3 + 2,
      };
    });
    // Deliberately different legacy FR: a complete IR set must replace it.
    const measurement = {
      id: scenario.name,
      averageFrequencyResponse: [
        [20, 0],
        [1000, 24],
        [20000, -12],
      ],
    };
    const reference = upstream.designRoomEq({
      config: {
        sampleRate: scenario.sampleRate,
        taps: 8192,
        phase: scenario.phase,
        smoothing: 0.17,
      },
      sources: [{ measurement, impulses }],
    });
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
    u32(scenario.sampleRate);
    u32(8192);
    u32(scenario.phase === 'min' ? 0 : 1);
    for (const number of [0.17, 20, 16000, 6, 1]) f64(number);
    u32(0);
    u32(1);
    u32(1);
    u32(measurement.averageFrequencyResponse.length);
    for (const pair of measurement.averageFrequencyResponse) {
      f64(pair[0]);
      f64(pair[1]);
    }
    u32(impulses.length);
    for (const impulse of impulses) {
      u32(impulse.pointId);
      u32(impulse.sampleRate);
      u32(impulse.onsetIndex);
      f64(impulse.refScale);
      u32(impulse.data.length);
      chunks.push(Buffer.from(impulse.data.buffer));
    }
    const result = spawnSync(
      process.env.PIPETUNE_ROOM_DRIVER!,
      ['--design-ir'],
      { input: Buffer.concat(chunks), maxBuffer: 4 * 1024 * 1024 }
    );
    expect(result.status, result.stderr?.toString()).toBe(0);
    const newline = result.stdout.indexOf(10);
    const metadata = JSON.parse(result.stdout.subarray(0, newline).toString());
    expect(metadata.supportsFullPhase).toBe(true);
    expect(metadata.qualityWarnings).toEqual(reference.qualityWarnings);
    expect(metadata.filterDelaySamples).toBe(
      reference.latencyInfo.filterDelaySamples
    );
    const pcm = result.stdout.subarray(newline + 1);
    expect(pcm.length).toBe(8192 * 4);
    let error = 0;
    for (let index = 0; index < 8192; index++)
      error = Math.max(
        error,
        Math.abs(pcm.readFloatLE(index * 4) - reference.channels[0][index])
      );
    expect(error).toBeLessThan(2e-7);
  }
);
