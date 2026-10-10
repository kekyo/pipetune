import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

const cases = [
  { name: 'single-point direct all-pass' },
  { name: 'multipoint consensus', points: 3 },
  { name: 'specific reference point', points: 3, referencePoint: 6 },
  {
    name: 'Minimum reference window diagnostic',
    points: 3,
    referencePoint: 6,
    phase: 'min',
    variedLength: true,
  },
  {
    name: 'Linear reference window diagnostic',
    points: 3,
    referencePoint: 6,
    phase: 'lin',
    variedLength: true,
  },
  {
    name: 'missing reference falls back to consensus',
    points: 3,
    referencePoint: 99,
  },
  { name: 'phase amount zero', phaseCorrectionAmount: 0 },
  {
    name: 'independent partial magnitude and phase',
    correctionAmount: 0.6,
    phaseCorrectionAmount: 0.4,
  },
  {
    name: 'manual phase-low boundary',
    phaseLowFrequency: 150,
    directWindowMs: 20,
  },
  {
    name: 'phase smoothing independent from magnitude',
    correctionAmount: 0,
    phaseSmoothing: 0.8,
  },
  { name: 'direct phase after resampling', sourceRate: 44100 },
  { name: 'larger multipoint FIR', points: 2, taps: 16384 },
  { name: 'shortest direct window', directWindowMs: 1 },
  {
    name: 'long direct window including reflections',
    points: 2,
    directWindowMs: 50,
  },
].map((entry) => ({
  sampleRate: 48000,
  sourceRate: 48000,
  taps: 8192,
  points: 1,
  correctionAmount: 1,
  phaseCorrectionAmount: 1,
  directWindowMs: 6,
  phaseLowFrequency: -1,
  phaseSmoothing: -1,
  referencePoint: 0,
  phase: 'full',
  variedLength: false,
  ...entry,
}));

test.each(cases)(
  '$name matches the application FIR and diagnostics',
  async (scenario) => {
    const upstream = await import(
      /* @vite-ignore */ pathToFileURL(
        process.env.PIPETUNE_EFFETUNE_SOURCE + '/js/room-eq/design-core.js'
      ).href
    );
    const impulses = Array.from({ length: scenario.points }, (_, point) => {
      const data = new Float32Array(
        4096 + (scenario.variedLength ? point * 512 : 0)
      );
      const onsetIndex = 128 + point * 17;
      const coefficient = 0.72 - point * 0.07;
      data[onsetIndex] = coefficient;
      for (let index = 1; index < 300; index++)
        data[onsetIndex + index] =
          (1 - coefficient * coefficient) * (-coefficient) ** (index - 1);
      if (point) data[onsetIndex + 1200] = 0.12 * point;
      return {
        data,
        onsetIndex,
        sampleRate: scenario.sourceRate,
        refScale: 1,
        pointId: 2 + point * 3,
      };
    });
    const measurement = {
      id: scenario.name,
      averageFrequencyResponse: [
        [20, 0],
        [20000, 0],
      ],
    };
    const config = {
      ...scenario,
      smoothing: 0.17,
      phaseLowFrequency:
        scenario.phaseLowFrequency < 0 ? null : scenario.phaseLowFrequency,
      phaseSmoothing:
        scenario.phaseSmoothing < 0 ? null : scenario.phaseSmoothing,
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
    u32(scenario.sampleRate);
    u32(scenario.taps);
    u32(['min', 'lin', 'full'].indexOf(scenario.phase));
    for (const value of [
      0.17,
      20,
      16000,
      6,
      scenario.correctionAmount,
      scenario.directWindowMs,
      scenario.phaseLowFrequency,
      scenario.phaseCorrectionAmount,
      0,
      300,
      250,
      0.05,
      scenario.phaseSmoothing,
    ])
      f64(value);
    u32(0);
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
      f64(impulse.refScale);
      u32(impulse.data.length);
      chunks.push(Buffer.from(impulse.data.buffer));
    }
    const result = spawnSync(
      process.env.PIPETUNE_ROOM_DRIVER!,
      ['--design-phase'],
      {
        input: Buffer.concat(chunks),
        maxBuffer: 4 * 1024 * 1024,
      }
    );
    expect(result.status, result.stderr?.toString()).toBe(0);
    const newline = result.stdout.indexOf(10);
    const metadata = JSON.parse(result.stdout.subarray(0, newline).toString());
    expect(metadata.qualityWarnings).toEqual(reference.qualityWarnings);
    expect(metadata.supportsFullPhase).toBe(true);
    expect(metadata.filterDelaySamples).toBe(
      reference.latencyInfo.filterDelaySamples
    );
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
          expect(metadata.diagnostics[key][0][field]).toBeCloseTo(expected, 6);
        else expect(metadata.diagnostics[key][0][field]).toBe(expected);
      }
    }
    const pcm = result.stdout.subarray(newline + 1);
    expect(pcm.length).toBe(scenario.taps * 4);
    let error = 0;
    for (let index = 0; index < scenario.taps; index++)
      error = Math.max(
        error,
        Math.abs(pcm.readFloatLE(index * 4) - reference.channels[0][index])
      );
    expect(error).toBeLessThan(2e-6);
  }
);
