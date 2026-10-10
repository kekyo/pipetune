import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

const cases = [
  { name: 'low-frequency all-pass', stages: [1] },
  { name: 'multi-stage common delay', stages: [8] },
  { name: 'point consensus with different phase', stages: [2, 5, 8] },
  {
    name: 'specific point in low-frequency consensus',
    stages: [2, 5, 8],
    referencePoint: 2,
  },
  { name: 'short observation degrades safely', stages: [1], frames: 512 },
  { name: 'long measured tail beyond half FIR', stages: [6], frames: 24000 },
  {
    name: 'phase amount disables extension',
    stages: [1],
    phaseCorrectionAmount: 0,
  },
  {
    name: 'partial low-frequency correction',
    stages: [3],
    phaseCorrectionAmount: 0.35,
  },
  { name: 'no extension band', stages: [1], lowFrequency: 500 },
  {
    name: 'wide extension band',
    stages: [3],
    lowFrequency: 20,
    phaseLowFrequency: 4000,
  },
  {
    name: 'FIR edge energy guard',
    stages: [20],
    coefficient: -0.995,
    frames: 24000,
  },
  { name: 'arrival outside direct window at 10 ms', stages: [0], delay: 480 },
  { name: 'arrival outside direct window at 20 ms', stages: [0], delay: 960 },
  {
    name: '10 ms-class low-frequency group delay',
    stages: [5],
    frames: 24000,
    taps: 32768,
    maximumResidualMs: 1,
  },
  {
    name: '20 ms-class low-frequency group delay',
    stages: [10],
    frames: 24000,
    taps: 32768,
    maximumResidualMs: 2,
  },
].map((entry) => ({
  frames: 12000,
  coefficient: -0.98,
  delay: 0,
  referencePoint: 0,
  taps: 8192,
  maximumResidualMs: 0,
  lowFrequency: 50,
  phaseLowFrequency: 500,
  phaseCorrectionAmount: 1,
  ...entry,
}));

test.each(cases)(
  '$name matches low-frequency FIR and reduction diagnostics',
  async (scenario) => {
    const url = (path: string) =>
      pathToFileURL(process.env.PIPETUNE_EFFETUNE_SOURCE + path).href;
    const upstream = await import(
      /* @vite-ignore */ url('/js/room-eq/design-core.js')
    );
    const impulses = scenario.stages.map((stages, pointId) => {
      let data = new Float32Array(scenario.frames);
      const onsetIndex = 128;
      data[onsetIndex + scenario.delay] = 1;
      for (let stage = 0; stage < stages; stage++) {
        const output = new Float32Array(data.length);
        let previousInput = 0,
          previousOutput = 0;
        for (let index = onsetIndex; index < data.length; index++) {
          output[index] =
            scenario.coefficient * data[index] +
            previousInput -
            scenario.coefficient * previousOutput;
          previousInput = data[index];
          previousOutput = output[index];
        }
        data = output;
      }
      return { data, sampleRate: 48000, onsetIndex, refScale: 1, pointId };
    });
    const config = {
      sampleRate: 48000,
      taps: scenario.taps,
      phase: 'full',
      smoothing: 0.05,
      lowFrequency: scenario.lowFrequency,
      highFrequency: 16000,
      directWindowMs: 6,
      phaseLowFrequency: scenario.phaseLowFrequency,
      correctionAmount: 0,
      phaseCorrectionAmount: scenario.phaseCorrectionAmount,
      referencePoint: scenario.referencePoint,
      lowFrequencyPhaseExtension: true,
    };
    const measurement = {
      id: scenario.name,
      averageFrequencyResponse: [
        [20, 0],
        [20000, 0],
      ],
    };
    const reference = upstream.designRoomEq({
      config,
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
    u32(48000);
    u32(scenario.taps);
    u32(2);
    for (const value of [
      0.05,
      scenario.lowFrequency,
      16000,
      6,
      0,
      6,
      scenario.phaseLowFrequency,
      scenario.phaseCorrectionAmount,
      0,
      300,
      250,
      0.05,
      -1,
    ])
      f64(value);
    u32(1);
    u32(scenario.referencePoint);
    u32(0);
    u32(0);
    u32(1);
    u32(1);
    u32(2);
    for (const pair of measurement.averageFrequencyResponse) {
      f64(pair[0]);
      f64(pair[1]);
    }
    u32(impulses.length);
    for (const impulse of impulses) {
      u32(impulse.pointId);
      u32(impulse.sampleRate);
      u32(impulse.onsetIndex);
      f64(1);
      u32(impulse.data.length);
      chunks.push(Buffer.from(impulse.data.buffer));
    }
    const native = spawnSync(
      process.env.PIPETUNE_ROOM_DRIVER!,
      ['--design-phase'],
      {
        input: Buffer.concat(chunks),
        maxBuffer: 4 * 1024 * 1024,
      }
    );
    expect(native.status, native.stderr?.toString()).toBe(0);
    const newline = native.stdout.indexOf(10);
    const metadata = JSON.parse(native.stdout.subarray(0, newline).toString());
    expect(metadata.qualityWarnings).toEqual(reference.qualityWarnings);
    for (const key of [
      'phaseCorrection',
      'lowFrequencyPhaseExtension',
      'reverbCorrection',
    ]) {
      expect(metadata.diagnostics[key]).toHaveLength(1);
      for (const [field, expected] of Object.entries(
        reference.diagnostics[key][0]
      )) {
        if (typeof expected === 'number')
          expect(metadata.diagnostics[key][0][field]).toBeCloseTo(expected, 5);
        else expect(metadata.diagnostics[key][0][field]).toBe(expected);
      }
    }
    const pcm = native.stdout.subarray(newline + 1);
    const taps = Float32Array.from({ length: scenario.taps }, (_, index) =>
      pcm.readFloatLE(index * 4)
    );
    let error = 0;
    for (let index = 0; index < taps.length; index++)
      error = Math.max(
        error,
        Math.abs(taps[index] - reference.channels[0][index])
      );
    expect(error).toBeLessThan(2e-6);
    if (scenario.maximumResidualMs > 0) {
      const gd = await import(
        /* @vite-ignore */ url('/js/room-eq/group-delay-analysis.js')
      );
      const frequencies = Array.from(
        { length: 100 },
        (_, index) => 70 * (350 / 70) ** (index / 99)
      );
      const source = gd.analyzeRoomEqGroupDelay(
        impulses[0].data,
        128,
        48000,
        frequencies
      );
      const filter = gd.analyzeRoomEqGroupDelay(
        taps,
        scenario.taps / 2,
        48000,
        frequencies
      );
      const before = gd.smoothRoomEqGroupDelay(source, frequencies, 0.05);
      const after = gd.smoothRoomEqGroupDelay(
        gd.combineRoomEqGroupDelay(source, filter),
        frequencies,
        0.05
      );
      const rms = Math.sqrt(
        after.excess.reduce(
          (sum: number, value: number) => sum + value * value,
          0
        ) / frequencies.length
      );
      const beforeRms = Math.sqrt(
        before.excess.reduce(
          (sum: number, value: number) => sum + value * value,
          0
        ) / frequencies.length
      );
      expect(rms).toBeLessThan(scenario.maximumResidualMs);
      expect(rms).toBeLessThan(beforeRms * 0.25);
    }
  },
  60000
);
