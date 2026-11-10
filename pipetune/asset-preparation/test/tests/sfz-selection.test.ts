import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

interface SampleInfo {
  size: number;
  frames: number;
  channels: number;
}
interface Scenario {
  name: string;
  text: string;
  metadata: Record<string, SampleInfo>;
  budget: number;
  definitionBytes: number;
  fits: boolean;
}

const scenarios: Scenario[] = [
  {
    name: 'generated PCM and file PCM share the capacity budget',
    text: '<region> sample=*sine key=60\n<region> sample=a.wav key=60',
    metadata: {
      '*sine': { size: 0, frames: 2048, channels: 1 },
      'a.wav': { size: 19244, frames: 4800, channels: 1 },
    },
    budget: 8904,
    definitionBytes: 100,
    fits: false,
  },
  {
    name: 'generated and file layers are rejected together when oversized',
    text: '<region> sample=*sine key=60\n<region> sample=*noise key=61\n<region> sample=a.wav lokey=60 hikey=61',
    metadata: {
      '*sine': { size: 0, frames: 2048, channels: 1 },
      '*noise': { size: 0, frames: 8192, channels: 1 },
      'a.wav': { size: 80044, frames: 20000, channels: 1 },
    },
    budget: 43000,
    definitionBytes: 100,
    fits: false,
  },
  {
    name: 'all layers fit, sharing source PCM',
    text: '<group> sample=a.wav\n<region> key=60 lovel=1 hivel=63\n<region> key=60 lovel=64',
    metadata: { 'a.wav': { size: 100, frames: 100, channels: 1 } },
    budget: 1916,
    definitionBytes: 25,
    fits: true,
  },
  {
    name: 'equal velocity layers cannot substitute for the complete instrument',
    text: '<region> sample=z.wav lokey=60 hikey=63 lovel=32 hivel=96\n<region> sample=a.wav lokey=60 hikey=63 lovel=32 hivel=96',
    metadata: {
      'z.wav': { size: 100, frames: 100, channels: 1 },
      'a.wav': { size: 100, frames: 100, channels: 1 },
    },
    budget: 1124,
    definitionBytes: 25,
    fits: false,
  },
  {
    name: 'a smaller sample cannot substitute for another velocity layer',
    text: '<region> sample=a.wav lokey=60 hikey=63\n<region> sample=b.wav lokey=60 hikey=63',
    metadata: {
      'a.wav': { size: 100, frames: 200, channels: 1 },
      'b.wav': { size: 100, frames: 100, channels: 1 },
    },
    budget: 1124,
    definitionBytes: 25,
    fits: false,
  },
  {
    name: 'all velocity ranges must fit together',
    text: '<region> sample=large.wav key=60 lovel=64 hivel=64\n<region> sample=lower.wav key=60 lovel=1 hivel=63\n<region> sample=higher.wav key=60 lovel=65 hivel=127',
    metadata: {
      'large.wav': { size: 100, frames: 1000, channels: 1 },
      'lower.wav': { size: 100, frames: 100, channels: 1 },
      'higher.wav': { size: 100, frames: 100, channels: 1 },
    },
    budget: 1112,
    definitionBytes: 0,
    fits: false,
  },
  {
    name: 'raw source size is checked independently of decoded PCM',
    text: '<region> sample=a.wav key=60\n<region> sample=b.wav key=60 lovel=65',
    metadata: {
      'a.wav': { size: 1500, frames: 50, channels: 1 },
      'b.wav': { size: 1500, frames: 50, channels: 1 },
    },
    budget: 2000,
    definitionBytes: 500,
    fits: false,
  },
  {
    name: 'random and round robin layers retain their original ranges and groups',
    text: '<group> seq_length=2\n<region> sample=a.wav lokey=60 hikey=62 seq_position=1 lorand=0 hirand=0.5\n<region> sample=b.wav lokey=60 hikey=62 seq_position=2 lorand=0.5 hirand=1',
    metadata: {
      'a.wav': { size: 100, frames: 100, channels: 1 },
      'b.wav': { size: 100, frames: 100, channels: 1 },
    },
    budget: 4096,
    definitionBytes: 0,
    fits: true,
  },
  {
    name: 'disjoint key ranges retain all layers',
    text: '<region> sample=a.wav key=60\n<region> sample=a.wav key=62\n<region> sample=b.wav key=60\n<region> sample=b.wav key=62',
    metadata: {
      'a.wav': { size: 100, frames: 100, channels: 1 },
      'b.wav': { size: 100, frames: 100, channels: 1 },
    },
    budget: 4096,
    definitionBytes: 0,
    fits: true,
  },
  {
    name: 'unknown duration is accepted pending decoded PCM validation',
    text: '<region> sample=a.mp3 key=60',
    metadata: { 'a.mp3': { size: 100, frames: 0, channels: 0 } },
    budget: 1060,
    definitionBytes: 0,
    fits: true,
  },
  {
    name: 'encoded size is checked even when duration is unknown',
    text: '<region> sample=a.wav key=60\n<region> sample=b.mp3 key=61',
    metadata: {
      'a.wav': { size: 100, frames: 100, channels: 1 },
      'b.mp3': { size: 3000, frames: 0, channels: 0 },
    },
    budget: 2000,
    definitionBytes: 0,
    fits: false,
  },
  {
    name: 'one byte below the complete key index budget fails',
    text: '<region> sample=a.wav lokey=0 hikey=127',
    metadata: { 'a.wav': { size: 100, frames: 100, channels: 1 } },
    budget: 1967,
    definitionBytes: 0,
    fits: false,
  },
  {
    name: 'the complete key index fits at the exact boundary',
    text: '<region> sample=a.wav lokey=0 hikey=127',
    metadata: { 'a.wav': { size: 100, frames: 100, channels: 1 } },
    budget: 1968,
    definitionBytes: 0,
    fits: true,
  },
  {
    name: 'one byte below the shared sample bank budget fails',
    text: '<group> sample=a.wav\n<region> key=60 lovel=1 hivel=63\n<region> key=60 lovel=64',
    metadata: { 'a.wav': { size: 100, frames: 100, channels: 1 } },
    budget: 1915,
    definitionBytes: 25,
    fits: false,
  },
  {
    name: 'definition and original file sizes fit at the exact boundary',
    text: '<region> sample=a.wav key=60',
    metadata: { 'a.wav': { size: 1500, frames: 100, channels: 1 } },
    budget: 2000,
    definitionBytes: 500,
    fits: true,
  },
  {
    name: 'one byte below the original file budget fails',
    text: '<region> sample=a.wav key=60',
    metadata: { 'a.wav': { size: 1500, frames: 100, channels: 1 } },
    budget: 1999,
    definitionBytes: 500,
    fits: false,
  },
  {
    name: 'metadata for unused sources still counts toward the budget',
    text: '<region> sample=a.wav key=60',
    metadata: {
      'a.wav': { size: 100, frames: 100, channels: 1 },
      'b.wav': { size: 5000, frames: 100, channels: 1 },
    },
    budget: 2000,
    definitionBytes: 0,
    fits: false,
  },
  {
    name: 'missing metadata prevents range loss',
    text: '<region> sample=a.wav key=60',
    metadata: {},
    budget: 4096,
    definitionBytes: 0,
    fits: false,
  },
  {
    name: 'large synthetic metadata is checked without allocating PCM',
    text: '<region> sample=a.wav key=60',
    metadata: { 'a.wav': { size: 2 ** 40, frames: 2 ** 40, channels: 2 } },
    budget: 1024 ** 3,
    definitionBytes: 0,
    fits: false,
  },
  {
    name: 'decoded byte multiplication cannot overflow the budget check',
    text: '<region> sample=a.wav key=60',
    metadata: { 'a.wav': { size: 100, frames: 2 ** 62, channels: 2 } },
    budget: 1024 ** 3,
    definitionBytes: 0,
    fits: false,
  },
];

test.each(scenarios)(
  'complete SFZ capacity matches EffeTune 2.17: $name',
  async ({ text, metadata, budget, definitionBytes, fits }) => {
    const source = process.env.PIPETUNE_EFFETUNE_SOURCE!;
    const parser = await import(
      /* @vite-ignore */ pathToFileURL(source + '/js/sfz/parser.js').href
    );
    const service = await import(
      /* @vite-ignore */ pathToFileURL(source + '/js/sfz/service.js').href
    );
    const parsed = await parser.parseSfz({
      selectedPath: 'main.sfz',
      readText: async () => text,
      onDiagnostic: null,
    });
    let expected:
      { regions: Record<string, unknown>[]; reduced: boolean } | undefined;
    let failure: { code: string; message: string } | undefined;
    try {
      expected = service.selectSfzRegionsForBudget(
        parsed.regions,
        new Map(Object.entries(metadata)),
        budget,
        definitionBytes
      );
    } catch (error) {
      failure = error as { code: string; message: string };
    }
    expect(failure === undefined).toBe(fits);
    const chunks: Buffer[] = [];
    const word = (value: number) => {
      const bytes = Buffer.alloc(4);
      bytes.writeUInt32LE(value);
      chunks.push(bytes);
    };
    const wide = (value: number) => {
      word(value % 2 ** 32);
      word(Math.floor(value / 2 ** 32));
    };
    const string = (value: string) => {
      const bytes = Buffer.from(value);
      word(bytes.length);
      chunks.push(bytes);
    };
    string(text);
    word(Object.keys(metadata).length);
    for (const [path, info] of Object.entries(metadata)) {
      string(path);
      wide(info.size);
      wide(info.frames);
      word(info.channels);
    }
    const native = spawnSync(
      process.env.PIPETUNE_SFZ_DRIVER!,
      ['--select', String(budget), String(definitionBytes)],
      { input: Buffer.concat(chunks) }
    );
    if (failure) {
      expect(native.status).not.toBe(0);
      expect(native.stderr.toString()).toBe(
        failure.code + ': ' + failure.message + '\n'
      );
    } else {
      expect(native.status, native.stderr.toString()).toBe(0);
      const actual = JSON.parse(native.stdout.toString());
      expect(actual.reduced).toBe(false);
      expect(actual.parsed.regions).toHaveLength(expected!.regions.length);
      // Capacity validation must preserve every resident field in source order.
      // The upstream parser also supplies fields outside PipeTune's SFZ subset.
      for (const [index, region] of actual.parsed.regions.entries()) {
        expect(region).toEqual(
          Object.fromEntries(
            Object.entries(expected!.regions[index]).filter(([key]) =>
              Object.hasOwn(region, key)
            )
          )
        );
      }
    }
  }
);
