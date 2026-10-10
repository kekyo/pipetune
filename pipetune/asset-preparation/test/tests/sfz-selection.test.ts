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
}

const scenarios: Scenario[] = [
  {
    name: 'all layers fit, sharing source PCM',
    text: '<group> sample=a.wav\n<region> key=60 lovel=1 hivel=63\n<region> key=60 lovel=64',
    metadata: { 'a.wav': { size: 100, frames: 100, channels: 1 } },
    budget: 1236,
    definitionBytes: 25,
  },
  {
    name: 'velocity 64 and source ordering break full ties',
    text: '<region> sample=z.wav lokey=60 hikey=63 lovel=32 hivel=96\n<region> sample=a.wav lokey=60 hikey=63 lovel=32 hivel=96',
    metadata: {
      'z.wav': { size: 100, frames: 100, channels: 1 },
      'a.wav': { size: 100, frames: 100, channels: 1 },
    },
    budget: 1124,
    definitionBytes: 25,
  },
  {
    name: 'smaller decoded sample wins velocity tie',
    text: '<region> sample=a.wav lokey=60 hikey=63\n<region> sample=b.wav lokey=60 hikey=63',
    metadata: {
      'a.wav': { size: 100, frames: 200, channels: 1 },
      'b.wav': { size: 100, frames: 100, channels: 1 },
    },
    budget: 1124,
    definitionBytes: 25,
  },
  {
    name: 'velocity 63 is tried before 65',
    text: '<region> sample=large.wav key=60 lovel=64 hivel=64\n<region> sample=lower.wav key=60 lovel=1 hivel=63\n<region> sample=higher.wav key=60 lovel=65 hivel=127',
    metadata: {
      'large.wav': { size: 100, frames: 1000, channels: 1 },
      'lower.wav': { size: 100, frames: 100, channels: 1 },
      'higher.wav': { size: 100, frames: 100, channels: 1 },
    },
    budget: 1112,
    definitionBytes: 0,
  },
  {
    name: 'raw source size forces reduction independently of PCM',
    text: '<region> sample=a.wav key=60\n<region> sample=b.wav key=60 lovel=65',
    metadata: {
      'a.wav': { size: 1500, frames: 50, channels: 1 },
      'b.wav': { size: 1500, frames: 50, channels: 1 },
    },
    budget: 2000,
    definitionBytes: 500,
  },
  {
    name: 'random and round robin become representative while groups survive',
    text: '<group> seq_length=2\n<region> sample=a.wav lokey=60 hikey=62 seq_position=1 lorand=0 hirand=0.5\n<region> sample=b.wav lokey=60 hikey=62 seq_position=2 lorand=0.5 hirand=1',
    metadata: {
      'a.wav': { size: 100, frames: 100, channels: 1 },
      'b.wav': { size: 100, frames: 100, channels: 1 },
    },
    budget: 1120,
    definitionBytes: 0,
  },
  {
    name: 'nonadjacent keys cannot merge over a hole',
    text: '<region> sample=a.wav key=60\n<region> sample=a.wav key=62\n<region> sample=b.wav key=60\n<region> sample=b.wav key=62',
    metadata: {
      'a.wav': { size: 100, frames: 100, channels: 1 },
      'b.wav': { size: 100, frames: 100, channels: 1 },
    },
    budget: 1240,
    definitionBytes: 0,
  },
  {
    name: 'unknown duration is accepted when no reduction is required',
    text: '<region> sample=a.mp3 key=60',
    metadata: { 'a.mp3': { size: 100, frames: 0, channels: 0 } },
    budget: 1024,
    definitionBytes: 0,
  },
  {
    name: 'unknown duration cannot silently lose a key during reduction',
    text: '<region> sample=a.wav key=60\n<region> sample=b.mp3 key=61',
    metadata: {
      'a.wav': { size: 100, frames: 100, channels: 1 },
      'b.mp3': { size: 3000, frames: 0, channels: 0 },
    },
    budget: 2000,
    definitionBytes: 0,
  },
  {
    name: 'minimum complete range exceeds budget',
    text: '<region> sample=a.wav lokey=0 hikey=127',
    metadata: { 'a.wav': { size: 100, frames: 100, channels: 1 } },
    budget: 1619,
    definitionBytes: 0,
  },
  {
    name: 'missing metadata prevents range loss',
    text: '<region> sample=a.wav key=60',
    metadata: {},
    budget: 4096,
    definitionBytes: 0,
  },
  {
    name: 'large synthetic metadata is checked without allocating PCM',
    text: '<region> sample=a.wav key=60',
    metadata: { 'a.wav': { size: 2 ** 40, frames: 2 ** 40, channels: 2 } },
    budget: 1024 ** 3,
    definitionBytes: 0,
  },
];

test.each(scenarios)(
  'SFZ capacity selection matches app: $name',
  async ({ text, metadata, budget, definitionBytes }) => {
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
    let expected: unknown;
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
      expect({
        ...actual,
        regions: actual.parsed.regions,
        parsed: undefined,
      }).toEqual(expected);
    }
  }
);
