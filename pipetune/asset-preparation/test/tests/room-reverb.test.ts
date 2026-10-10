import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

const cases = [
  { name: 'default extended observation' },
  { name: 'three-point reverb consensus', points: 3 },
  { name: 'reverb without direct phase', phaseCorrectionAmount: 0 },
  {
    name: 'partial independent reverb',
    reverbAmount: 0.35,
    phaseCorrectionAmount: 0.6,
  },
  { name: 'overlapping low-frequency extension', extension: true },
  { name: 'empty reverb band', reverbMaxFrequency: 20 },
  { name: 'short measurement window budget', frames: 256 },
  {
    name: 'reverb window equal to direct window',
    directWindowMs: 50,
    reverbWindowMs: 20,
  },
  {
    name: 'narrow short reverb band',
    reverbWindowMs: 20,
    reverbMaxFrequency: 200,
  },
  { name: 'observation limited by measurement length', reverbWindowMs: 1000 },
  { name: 'long reflection with short FIR', allpassDelayMs: 90, frames: 48000 },
  {
    name: 'realizable delayed all-pass passes energy guard',
    pureAllpass: true,
    allpassGain: 0.5,
    allpassDelayMs: 20,
    frames: 131072,
    sampleRate: 96000,
    phaseCorrectionAmount: 0,
    correctionAmount: 0,
    reverbWindowMs: 1000,
    reverbMaxFrequency: 20000,
    reverbSmoothing: 0.02,
  },
  {
    name: 'unrealizable advance is disabled by energy guard',
    pureAllpass: true,
    allpassGain: 0.8,
    allpassDelayMs: 80,
    frames: 131072,
    sampleRate: 96000,
    phaseCorrectionAmount: 0,
    correctionAmount: 0,
    reverbWindowMs: 1000,
    reverbMaxFrequency: 20000,
    reverbSmoothing: 0.02,
  },
  {
    name: '96 kHz multiple points at FFT boundary',
    sampleRate: 96000,
    frames: 65536,
    points: 3,
  },
  {
    name: '96 kHz longer FIR with extension',
    sampleRate: 96000,
    frames: 65536,
    taps: 32768,
    extension: true,
  },
].map((entry) => ({
  sampleRate: 48000,
  frames: 24000,
  taps: 8192,
  points: 1,
  phaseCorrectionAmount: 1,
  reverbAmount: 1,
  reverbWindowMs: 300,
  reverbMaxFrequency: 250,
  directWindowMs: 6,
  allpassDelayMs: 8,
  extension: false,
  pureAllpass: false,
  allpassGain: 0.6,
  correctionAmount: 1,
  reverbSmoothing: 0.05,
  ...entry,
}));

// Synthetic direct response, reflections, room modes and delayed all-pass tail.
// Based on the MIT-licensed EffeTune v2.13.0 room-eq-design.test.mjs fixture;
// copyright (c) 2025-2026 Yoshiyuki Kobayashi, see ../../room-eq/LICENSE.effetune.
test.each(cases)(
  '$name matches the application reverb FIR and guards',
  async (scenario) => {
    const upstream = await import(
      /* @vite-ignore */ pathToFileURL(
        process.env.PIPETUNE_EFFETUNE_SOURCE + '/js/room-eq/design-core.js'
      ).href
    );
    const impulses = Array.from({ length: scenario.points }, (_, pointId) => {
      const onsetIndex = scenario.pureAllpass ? 512 : 128,
        dry = new Float64Array(scenario.frames);
      const pole = Math.exp((-2 * Math.PI * 7000) / scenario.sampleRate);
      for (const [delay, amplitude] of [
        [0, 1],
        [3, 0.32],
        [11.6, -0.24],
        [27.9, 0.18],
        [45.3, -0.12],
        [97, 0.07],
      ]) {
        const start =
          onsetIndex +
          Math.round(
            ((delay + (delay ? pointId * 0.21 : 0)) * scenario.sampleRate) /
              1000
          );
        for (let term = 0; term < 64 && start + term < dry.length; term++)
          dry[start + term] +=
            amplitude * (1 - pointId * 0.025) * (1 - pole) * pole ** term;
      }
      for (const [frequency, t60, amplitude] of [
        [45, 0.45, 0.04],
        [72, 0.55, 0.03],
        [110, 0.35, 0.035],
      ])
        for (let index = onsetIndex; index < dry.length; index++) {
          const time = (index - onsetIndex) / scenario.sampleRate;
          dry[index] +=
            amplitude *
            Math.exp((-Math.log(1000) / t60) * time) *
            Math.sin(2 * Math.PI * frequency * (1 + pointId * 0.01) * time);
        }
      if (scenario.pureAllpass) {
        dry.fill(0);
        dry[onsetIndex] = 1;
      }
      const data = new Float32Array(dry.length),
        delay = Math.round(
          (scenario.allpassDelayMs * scenario.sampleRate) / 1000
        ),
        gain = scenario.allpassGain;
      for (let index = 0; index < data.length; index++) {
        let value = -gain * dry[index],
          coefficient = 1 - gain * gain;
        for (
          let term = 1;
          term <= (scenario.pureAllpass ? Math.ceil(dry.length / delay) : 41);
          term++
        ) {
          const source = index - term * delay;
          if (source < 0) break;
          value += coefficient * dry[source];
          coefficient *= gain;
        }
        data[index] = value;
      }
      return {
        data,
        sampleRate: scenario.sampleRate,
        onsetIndex,
        refScale: 1,
        pointId,
      };
    });
    const config = {
      ...scenario,
      phase: 'full',
      lowFrequencyPhaseExtension: scenario.extension,
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
    u32(scenario.sampleRate);
    u32(scenario.taps);
    u32(2);
    for (const value of [
      0.17,
      20,
      16000,
      6,
      scenario.correctionAmount,
      scenario.directWindowMs,
      -1,
      scenario.phaseCorrectionAmount,
      scenario.reverbAmount,
      scenario.reverbWindowMs,
      scenario.reverbMaxFrequency,
      scenario.reverbSmoothing,
      -1,
    ])
      f64(value);
    u32(Number(scenario.extension));
    u32(0);
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
    if (scenario.pureAllpass) {
      const diagnostic = metadata.diagnostics.reverbCorrection[0];
      expect(diagnostic.state).toBe(
        scenario.allpassDelayMs === 20 ? 'applied' : 'disabled'
      );
      expect(diagnostic.scale).toBe(scenario.allpassDelayMs === 20 ? 1 : 0);
      if (scenario.allpassDelayMs === 80) {
        expect(diagnostic.reason).toBe('firEnergy');
        const baseline = upstream.designRoomEq({
          config: { ...config, reverbAmount: 0 },
          sources: [{ measurement, impulses }],
        });
        expect(reference.channels[0]).toEqual(baseline.channels[0]);
      }
    }
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
    let error = 0;
    for (let index = 0; index < scenario.taps; index++)
      error = Math.max(
        error,
        Math.abs(pcm.readFloatLE(index * 4) - reference.channels[0][index])
      );
    expect(error).toBeLessThan(2e-6);
  },
  60000
);
