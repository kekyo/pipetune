import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

const cases = [
  { name: 'minimum peak and dip', phase: 'min' },
  { name: 'linear peak and dip', phase: 'lin' },
  {
    name: 'unsupported tap count uses application default',
    phase: 'lin',
    taps: 2048,
  },
  {
    name: 'wide smoothing and no boost',
    phase: 'min',
    smoothing: 1,
    maxBoostDb: 0,
  },
  {
    name: 'narrow smoothing and maximum boost',
    phase: 'lin',
    smoothing: 0.02,
    maxBoostDb: 18,
  },
  {
    name: 'partial correction and shelves',
    phase: 'min',
    correctionAmount: 0.35,
    bands: true,
    sampleRate: 44100,
    taps: 16384,
  },
  {
    name: 'EQ without automatic correction',
    phase: 'lin',
    correctionAmount: 0,
    bands: true,
    taps: 32768,
  },
  {
    name: 'restricted correction band',
    phase: 'min',
    lowFrequency: 160,
    highFrequency: 4500,
    sampleRate: 96000,
    taps: 65536,
  },
  {
    name: 'largest filter at high rate',
    phase: 'lin',
    sampleRate: 192000,
    taps: 131072,
  },
  { name: 'unassigned channels retain delay', phase: 'lin', unassigned: true },
].map((entry) => ({
  sampleRate: 48000,
  taps: 8192,
  smoothing: 0.17,
  lowFrequency: 20,
  highFrequency: 16000,
  maxBoostDb: 6,
  correctionAmount: 1,
  bands: false,
  unassigned: false,
  ...entry,
}));

test.each(cases)(
  '$name matches the unmodified application FIR and quality diagnostics',
  async (scenario) => {
    const upstream = await import(
      /* @vite-ignore */ pathToFileURL(
        process.env.PIPETUNE_EFFETUNE_SOURCE + '/js/room-eq/design-core.js'
      ).href
    );
    const eqBands = scenario.bands
      ? [
          { enabled: true, type: 'pk', frequency: 1300, gain: -3.5, q: 1.7 },
          { enabled: true, type: 'ls', frequency: 170, gain: 2.4, q: 0.7 },
          { enabled: true, type: 'hs', frequency: 8000, gain: -1.2, q: 0.9 },
          { enabled: false, type: 'pk', frequency: 500, gain: 18, q: 5 },
        ]
      : [];
    const response = Array.from({ length: 90 }, (_, index) => {
      const frequency = 20 * 1000 ** (index / 89);
      return [
        frequency,
        4 +
          8 * Math.exp(-(Math.log2(frequency / 700) ** 2) / 0.8) -
          12 * Math.exp(-(Math.log2(frequency / 3100) ** 2) / 0.3),
      ];
    });
    const sources = scenario.unassigned
      ? [null, null]
      : [
          {
            measurement: {
              id: scenario.name,
              averageFrequencyResponse: response,
            },
          },
          {
            measurement: {
              id: scenario.name + '-reversed',
              averageFrequencyResponse: [...response].reverse(),
            },
          },
        ];
    const reference = upstream.designRoomEq({
      config: { ...scenario, eqBands },
      sources,
    });
    const chunks: Buffer[] = [];
    const u32 = (value: number) => {
      const data = Buffer.alloc(4);
      data.writeUInt32LE(value);
      chunks.push(data);
    };
    const f64 = (value: number) => {
      const data = Buffer.alloc(8);
      data.writeDoubleLE(value);
      chunks.push(data);
    };
    u32(scenario.sampleRate);
    u32(scenario.taps);
    u32(scenario.phase === 'min' ? 0 : 1);
    for (const value of [
      scenario.smoothing,
      scenario.lowFrequency,
      scenario.highFrequency,
      scenario.maxBoostDb,
      scenario.correctionAmount,
    ])
      f64(value);
    u32(eqBands.length);
    for (const band of eqBands) {
      u32(['pk', 'ls', 'hs'].indexOf(band.type));
      u32(Number(band.enabled));
      f64(band.frequency);
      f64(band.gain);
      f64(band.q);
    }
    u32(sources.length);
    for (const source of sources) {
      u32(Number(source !== null));
      const points = source?.measurement.averageFrequencyResponse ?? [];
      u32(points.length);
      for (const point of points) {
        f64(point[0]);
        f64(point[1]);
      }
    }
    const native = spawnSync(process.env.PIPETUNE_ROOM_DRIVER!, ['--design'], {
      input: Buffer.concat(chunks),
      maxBuffer: 16 * 1024 * 1024,
    });
    expect(
      native.status,
      native.error?.message ?? native.stderr?.toString()
    ).toBe(0);
    const newline = native.stdout.indexOf(10);
    expect(newline).toBeGreaterThan(0);
    const metadata = JSON.parse(native.stdout.subarray(0, newline).toString());
    expect(metadata.filterDelaySamples).toBe(
      reference.latencyInfo.filterDelaySamples
    );
    expect(metadata.qualityWarnings).toEqual(reference.qualityWarnings);
    expect(metadata.supportsFullPhase).toBe(reference.supportsFullPhase);
    expect(metadata.diagnostics).toMatchObject(reference.diagnostics);
    const pcm = native.stdout.subarray(newline + 1);
    expect(pcm.length).toBe(sources.length * reference.config.taps * 4);
    let error = 0;
    for (let channel = 0; channel < sources.length; channel++)
      for (let frame = 0; frame < reference.config.taps; frame++)
        error = Math.max(
          error,
          Math.abs(
            pcm.readFloatLE((channel * reference.config.taps + frame) * 4) -
              reference.channels[channel][frame]
          )
        );
    expect(error).toBeLessThan(2e-7);
  }
);
