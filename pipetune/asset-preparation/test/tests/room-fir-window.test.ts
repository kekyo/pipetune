import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

test('long low-frequency group delay reports the residual beyond the available FIR window', async () => {
  const sampleRate = 96000,
    taps = 8192,
    radius = 0.9996;
  const data = new Float32Array(262144);
  const cosine = Math.cos((2 * Math.PI * 20) / sampleRate);
  const states = Array.from({ length: 12 }, () => ({
    x1: 0,
    x2: 0,
    y1: 0,
    y2: 0,
  }));
  // Cascaded second-order all-pass, from EffeTune's MIT-licensed
  // room-eq-group-delay-analysis.test.mjs (2025-2026 Yoshiyuki Kobayashi).
  for (let index = 0; index < data.length; index++) {
    let value = index === 0 ? 1 : 0;
    for (const state of states) {
      const output =
        radius * radius * value -
        2 * radius * cosine * state.x1 +
        state.x2 +
        2 * radius * cosine * state.y1 -
        radius * radius * state.y2;
      state.x2 = state.x1;
      state.x1 = value;
      state.y2 = state.y1;
      state.y1 = output;
      value = output;
    }
    data[index] = value;
  }
  const upstream = await import(
    /* @vite-ignore */ pathToFileURL(
      process.env.PIPETUNE_EFFETUNE_SOURCE + '/js/room-eq/design-core.js'
    ).href
  );
  const reference = upstream.designRoomEq({
    config: {
      sampleRate,
      taps,
      phase: 'full',
      smoothing: 0.3,
      phaseSmoothing: 0.3,
      directWindowMs: 6,
      lowFrequency: 20,
      lowFrequencyPhaseExtension: true,
    },
    sources: [
      {
        measurement: { id: 'fir-window' },
        impulses: [{ data, sampleRate, onsetIndex: 0, pointId: 0 }],
      },
    ],
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
  u32(sampleRate);
  u32(taps);
  u32(2);
  for (const value of [0.3, 20, 16000, 6, 1, 6, -1, 1, 0, 300, 250, 0.05, 0.3])
    f64(value);
  u32(1);
  u32(0);
  u32(0);
  u32(0);
  u32(1);
  u32(1);
  u32(0);
  u32(1);
  u32(0);
  u32(sampleRate);
  u32(0);
  f64(1);
  u32(data.length);
  chunks.push(Buffer.from(data.buffer));
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
  const diagnostic = metadata.diagnostics.phaseCorrection[0];
  expect(diagnostic.state).toBe('reduced');
  expect(diagnostic.reason).toBe('firWindow');
  expect(diagnostic.scale).toBeCloseTo(
    reference.diagnostics.phaseCorrection[0].scale,
    6
  );
  expect(diagnostic.residualMaximumMs).toBeGreaterThan(300);
  expect(diagnostic.residualMaximumMs).toBeCloseTo(
    reference.diagnostics.phaseCorrection[0].residualMaximumMs,
    2
  );
  expect(metadata.diagnostics.lowFrequencyPhaseExtension).toMatchObject(
    reference.diagnostics.lowFrequencyPhaseExtension
  );
  let error = 0;
  const pcm = native.stdout.subarray(newline + 1);
  for (let index = 0; index < taps; index++)
    error = Math.max(
      error,
      Math.abs(pcm.readFloatLE(index * 4) - reference.channels[0][index])
    );
  expect(error).toBeLessThan(2e-6);
}, 120000);
