import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

const cases = [
  { name: 'negative cut offset', dc: true, co: -0.25 },
  { name: 'positive cut offset and long decay', dc: true, co: 2, dt: 400 },
  {
    name: 'true stereo with short decay',
    channels: 4,
    cm: 'true',
    dt: 10,
    tr: 37,
  },
  {
    name: 'mono projection after multichannel analysis',
    channels: 4,
    cm: 'mono',
    dt: 65,
  },
  {
    name: 'independent projection',
    channels: 4,
    cm: 'indep',
    dc: true,
    co: 1,
    tr: 20,
  },
  {
    name: 'diagonal matrix and leading silence',
    channels: 3,
    width: 6,
    cm: 'multi',
    leading: 73,
  },
  { name: 'last sample after direct cut', frames: 32, dc: true, co: 50 },
  {
    name: 'zero-latency configuration forces full rate',
    head: 0,
    cr: 'quarter',
  },
  { name: 'automatic half rate', rate: 96000, cr: 'auto' },
  { name: 'quarter rate', rate: 192000, cr: 'quarter' },
  { name: 'commit capacity fade', channels: 16, width: 16, frames: 110000 },
].map((entry) => ({
  rate: 48000,
  channels: 1,
  width: 2,
  frames: 4096,
  cm: 'auto',
  head: 128,
  cr: 'full',
  dc: false,
  co: 0,
  dt: 100,
  tr: 100,
  leading: 0,
  ...entry,
}));

test.each(cases)(
  '$name matches application coefficients, analysis, and allocation bounds',
  async (scenario) => {
    const root = process.env.PIPETUNE_EFFETUNE_SOURCE;
    const preparationUrl = pathToFileURL(
      root + '/js/ir-library/ir-preparation.js'
    ).href;
    const contractUrl = pathToFileURL(
      root + '/js/ir-library/ir-plugin-contract.js'
    ).href;
    const preparation = await import(/* @vite-ignore */ preparationUrl);
    const contract = await import(/* @vite-ignore */ contractUrl);
    const channels = Array.from({ length: scenario.channels }, (_, channel) => {
      const pcm = new Float32Array(scenario.frames);
      for (let frame = scenario.leading; frame < pcm.length; frame++)
        pcm[frame] =
          Math.exp(-(frame - scenario.leading) / (scenario.frames * 0.1)) *
          Math.sin((frame - scenario.leading + 1) * (0.03 + channel * 0.11)) *
          (channel + 1);
      return pcm;
    });
    const config = contract.resolveIrProcessingConfig({
      sampleRate: scenario.rate,
      channelCount: channels.length,
      engineChannels: scenario.width,
      selectedChannels: scenario.width,
      channelMode: scenario.cm,
      latency: String(scenario.head),
      convolutionRate: scenario.cr,
    });
    expect(config.valid).toBe(true);
    const host = preparation.prepareIr({
      channels,
      sampleRate: config.sampleRate,
      options: {
        topology: config.topology,
        paths: config.paths,
        directCut: scenario.dc,
        cutOffsetMs: scenario.co,
        decayPercent: scenario.dt,
        trimPercent: scenario.tr,
        analysisPoints: 1600,
      },
    });
    const maximum = contract.maximumIrFramesForKernel({
      ...config,
      sourceFrames: host.frames,
    });
    const reference = preparation.emitPreparedIr({
      ...host,
      options: {
        topology: config.topology,
        paths: config.paths,
        assetChannels: config.assetChannels,
        maxFrames: maximum,
        analysisPoints: 1600,
      },
    });
    const input = Buffer.concat(channels.map((pcm) => Buffer.from(pcm.buffer)));
    const native = spawnSync(
      process.env.PIPETUNE_IR_DRIVER!,
      [
        '--prepare',
        String(scenario.rate),
        String(scenario.channels),
        String(scenario.frames),
        String(scenario.width),
        scenario.cm,
        String(scenario.head),
        scenario.cr,
        scenario.dc ? '1' : '0',
        String(scenario.co),
        String(scenario.dt),
        String(scenario.tr),
      ],
      { input, maxBuffer: 80 * 1024 * 1024 }
    );
    expect(native.status, native.stderr.toString()).toBe(0);
    const line = native.stdout.indexOf(10);
    expect(line).toBeGreaterThan(0);
    const metadata = JSON.parse(native.stdout.subarray(0, line).toString());
    const payload = native.stdout.subarray(line + 1);
    const expected = Buffer.from(reference.payload);
    expect(payload.length).toBe(expected.length);
    const headerSize = 32 + config.paths.length * 12;
    expect(payload.subarray(0, headerSize)).toEqual(
      expected.subarray(0, headerSize)
    );
    let error = 0;
    for (let offset = headerSize; offset < expected.length; offset += 4)
      error = Math.max(
        error,
        Math.abs(payload.readFloatLE(offset) - expected.readFloatLE(offset))
      );
    expect(error).toBeLessThan(2e-7);
    for (const key of [
      'onsetFrame',
      'leadingSilenceFrames',
      'sourceStartFrame',
    ])
      expect(metadata[key]).toBe(reference.analysis[key]);
    expect(metadata.footprintBytes).toBe(
      contract.estimateIrKernelCommitFootprint({
        ...config,
        frames: reference.frames,
        assetChannels: reference.asset.channels,
      })
    );
    expect(metadata.capacityLimited).toBe(maximum < host.frames);
    for (const [key, expectedAnalysis] of [
      ['analysis', reference.analysis],
      ['original', reference.analysis.original],
    ] as const) {
      const actual = metadata[key];
      for (const field of ['rt60Seconds', 'peakDb', 'l1GainUpperBound']) {
        if (expectedAnalysis[field] === null) expect(actual[field]).toBeNull();
        else
          expect(
            Math.abs(actual[field] - expectedAnalysis[field])
          ).toBeLessThan(1e-7);
      }
      for (const field of ['sampleFrames', 'envelope', 'edcDb']) {
        expect(actual[field]).toHaveLength(expectedAnalysis[field].length);
        let maximumError = 0;
        for (let index = 0; index < actual[field].length; index++)
          maximumError = Math.max(
            maximumError,
            Math.abs(actual[field][index] - expectedAnalysis[field][index])
          );
        expect(maximumError, field).toBeLessThan(0.00002);
      }
    }
    for (const [key, referenceKey] of [
      ['initialGains', 'initialNormalizationGains'],
      ['finalGains', 'finalNormalizationGains'],
    ]) {
      expect(metadata[key]).toHaveLength(
        reference.analysis[referenceKey].length
      );
      for (let index = 0; index < metadata[key].length; index++)
        expect(
          Math.abs(
            metadata[key][index] - reference.analysis[referenceKey][index]
          )
        ).toBeLessThan(2e-7);
    }
  }
);
