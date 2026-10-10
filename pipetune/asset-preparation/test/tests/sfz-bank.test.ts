import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

interface Sample {
  path: string;
  sampleRate: number;
  channels: Float32Array[];
}

const encode = (selected: string, definition: string, samples: Sample[]) => {
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
  text(selected);
  word(1);
  text(selected);
  text(definition);
  word(samples.length);
  for (const sample of samples) {
    text(sample.path);
    word(sample.sampleRate);
    word(sample.channels.length);
    word(sample.channels[0].length);
    for (const channel of sample.channels) {
      const bytes = Buffer.alloc(channel.length * 4);
      for (let frame = 0; frame < channel.length; ++frame)
        bytes.writeFloatLE(channel[frame], frame * 4);
      chunks.push(bytes);
    }
  }
  return Buffer.concat(chunks);
};

const run = (
  selected: string,
  definition: string,
  samples: Sample[],
  maximumBytes: number
) => {
  const driver = process.env.PIPETUNE_SFZ_DRIVER;
  expect(driver).toBeTruthy();
  return spawnSync(driver!, ['--prepare', String(maximumBytes)], {
    input: encode(selected, definition, samples),
    maxBuffer: 16 * 1024 * 1024,
  });
};

test.each([44100, 48000, 96000])(
  'SFZ mono/stereo bank at %i Hz matches unmodified app JS',
  async (sampleRate) => {
    const module = (name: string) =>
      pathToFileURL(`${process.env.PIPETUNE_EFFETUNE_SOURCE}/js/sfz/${name}.js`)
        .href;
    const parser = await import(/* @vite-ignore */ module('parser'));
    const asset = await import(/* @vite-ignore */ module('asset'));
    const selected = 'banks/instrument.sfz';
    const definition =
      '<region> sample="mono piano.wav" key=c4 amp_veltrack=0 loop_mode=loop_continuous loop_start=1 loop_end=31\n' +
      '<region> sample=stereo.wav lokey=61 hikey=80 pitch_keycenter=69 lovel=0 pan=30 volume=-3 offset=2 end=63 ampeg_release=0.125';
    const samples = [
      {
        path: 'banks/mono piano.wav',
        sampleRate,
        channels: [
          Float32Array.from(
            { length: 64 },
            (_, index) => Math.sin(index * 0.3) * 0.2
          ),
        ],
      },
      {
        path: 'banks/stereo.wav',
        sampleRate: 48000,
        channels: [
          Float32Array.from({ length: 96 }, (_, index) => index / 100),
          Float32Array.from({ length: 96 }, (_, index) => -index / 200),
        ],
      },
    ];
    const reference = await parser.parseSfz({
      selectedPath: selected,
      readText: async () => definition,
      hasSample: () => true,
      onDiagnostic: null,
    });
    const packed = asset.packSfzAsset(
      reference.regions,
      new Map(samples.map((sample) => [sample.path, sample])),
      { onDiagnostic: null }
    );
    const native = run(selected, definition, samples, 256 * 1024 * 1024);
    expect(native.status, native.stderr.toString()).toBe(0);
    const end = native.stdout.indexOf(10);
    const metadata = JSON.parse(native.stdout.subarray(0, end).toString());
    expect(metadata).toEqual({
      regions: reference.regions.length,
      footprintBytes: packed.footprintBytes,
      warmupFrames: packed.warmupSamples,
      samples: packed.samples,
    });
    expect(native.stdout.subarray(end + 1)).toEqual(
      Buffer.from(packed.payload)
    );
    expect(
      run(selected, definition, samples, packed.footprintBytes).status
    ).toBe(0);
    expect(
      run(selected, definition, samples, packed.footprintBytes - 1).status
    ).not.toBe(0);
  }
);

test('SFZ sample ordering uses UTF-16 and integer sample-index lanes', async () => {
  const parser = await import(
    /* @vite-ignore */ pathToFileURL(
      `${process.env.PIPETUNE_EFFETUNE_SOURCE}/js/sfz/parser.js`
    ).href
  );
  const asset = await import(
    /* @vite-ignore */ pathToFileURL(
      `${process.env.PIPETUNE_EFFETUNE_SOURCE}/js/sfz/asset.js`
    ).href
  );
  const names = ['\ue000.wav', '\u{10000}.wav', '音.wav'];
  const definition = names
    .map((name, index) => `<region> sample=${name} key=${60 + index}`)
    .join('\n');
  const samples = names.map((path, index) => ({
    path,
    sampleRate: 48000,
    channels: [new Float32Array([index + 1, 0, 0, 0])],
  }));
  const parsed = await parser.parseSfz({
    selectedPath: 'main.sfz',
    readText: async () => definition,
    hasSample: () => true,
    onDiagnostic: null,
  });
  const reference = asset.packSfzAsset(
    parsed.regions,
    new Map(samples.map((sample) => [sample.path, sample]))
  );
  const native = run('main.sfz', definition, samples, 256 * 1024 * 1024);
  expect(native.status, native.stderr.toString()).toBe(0);
  expect(native.stdout.subarray(native.stdout.indexOf(10) + 1)).toEqual(
    Buffer.from(reference.payload)
  );
});

test.each(['empty', 'channels', 'nonfinite', 'offset', 'loop', 'escape'])(
  'invalid SFZ %s input cannot produce a playable bank',
  (scenario) => {
    const sample = {
      path: 'sample.wav',
      sampleRate: 48000,
      channels: [new Float32Array([1, 0, 0, 0])],
    };
    let definition = '<region> sample=sample.wav key=60';
    if (scenario === 'empty') sample.channels[0] = new Float32Array();
    if (scenario === 'channels')
      sample.channels = [
        sample.channels[0],
        sample.channels[0],
        sample.channels[0],
      ];
    if (scenario === 'nonfinite') sample.channels[0][2] = NaN;
    if (scenario === 'offset') definition += ' offset=4';
    if (scenario === 'loop')
      definition += ' loop_mode=loop_continuous loop_end=4';
    if (scenario === 'escape') definition = '<region> sample=../sample.wav';
    const native = run('main.sfz', definition, [sample], 256 * 1024 * 1024);
    expect(native.status).not.toBe(0);
    expect(native.stdout.length).toBe(0);
  }
);

test('SFZ partial import preserves playable PCM and combines distinct region omissions', async () => {
  const source = process.env.PIPETUNE_EFFETUNE_SOURCE!;
  const parser = await import(
    /* @vite-ignore */ pathToFileURL(source + '/js/sfz/parser.js').href
  );
  const asset = await import(
    /* @vite-ignore */ pathToFileURL(source + '/js/sfz/asset.js').href
  );
  const service = await import(
    /* @vite-ignore */ pathToFileURL(source + '/js/sfz/service.js').href
  );
  const definition =
    '<group> sample=a.wav\n<region> tune=bad\n<region> offset=99\n' +
    '<region> loop_mode=loop_continuous loop_end=99\n<region> loop_mode=no_loop loop_start=-1\n' +
    '<region> loop_mode=one_shot loop_end=99\n<region> loop_mode=loop_sustain loop_start=2 loop_end=15\n' +
    '<region> sample=missing.wav\n<region> sample=missing.wav';
  const sample = {
    path: 'a.wav',
    sampleRate: 44100,
    channels: [Float32Array.from({ length: 16 }, (_, index) => index / 16)],
  };
  const parsed = await parser.parseSfz({
    selectedPath: 'main.sfz',
    readText: async () => definition,
    hasSample: (path: string) => path === 'a.wav',
    onDiagnostic: null,
  });
  const warnings: { code: string; count: number }[] = [];
  const diagnostics: { invalidRegions: string[] }[] = [];
  const reference = asset.packSfzAsset(
    parsed.regions,
    new Map([['a.wav', sample]]),
    {
      onDiagnostic: (value: { invalidRegions: string[] }) =>
        diagnostics.push(value),
      onWarning: (value: { code: string; count: number }) =>
        warnings.push(value),
    }
  );
  const recovered = warnings.map((warning) =>
    warning.code === 'invalid-regions'
      ? {
          ...warning,
          count:
            warning.count +
            parsed.warnings.find(
              (item: { code: string }) => item.code === warning.code
            ).count,
        }
      : warning
  );
  const native = spawnSync(
    process.env.PIPETUNE_SFZ_DRIVER!,
    ['--prepare-details', String(256 * 1024 * 1024)],
    { input: encode('main.sfz', definition, [sample]) }
  );
  expect(native.status, native.stderr.toString()).toBe(0);
  const end = native.stdout.indexOf(10);
  const metadata = JSON.parse(native.stdout.subarray(0, end).toString());
  expect(metadata).toEqual({
    regions: 3,
    footprintBytes: reference.footprintBytes,
    warmupFrames: reference.warmupSamples,
    samples: reference.samples,
    warnings: service.mergeSfzWarnings(parsed.warnings, recovered),
    invalidRegions: [
      ...parsed.diagnostics.invalidRegions,
      ...diagnostics.flatMap((value) => value.invalidRegions),
    ],
  });
  expect(native.stdout.subarray(end + 1)).toEqual(
    Buffer.from(reference.payload)
  );
});
