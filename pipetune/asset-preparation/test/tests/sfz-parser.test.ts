import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

const run = (
  selected: string,
  documents: Record<string, string>,
  paths: string[],
  maximumBytes: number
) => {
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
  word(Object.keys(documents).length);
  for (const [path, content] of Object.entries(documents)) {
    text(path);
    text(content);
  }
  word(paths.length);
  for (const path of paths) {
    text(path);
    word(48000);
    word(1);
    word(0);
  }
  return spawnSync(
    process.env.PIPETUNE_SFZ_DRIVER!,
    ['--parse', String(maximumBytes)],
    { input: Buffer.concat(chunks), maxBuffer: 8 * 1024 * 1024 }
  );
};

const scenarios: {
  name: string;
  selected: string;
  documents: Record<string, string>;
  paths: string[];
}[] = [
  {
    name: 'include, macro, inherited scopes and selected-root samples',
    selected: 'banks/main.sfz',
    documents: {
      'banks/main.sfz':
        '#define $ROOT ../samples/\n<control> default_path=$ROOT\n<global> volume=-6\n#include "parts/a.sfz"\n<region> sample=end.wav key=d4',
      'banks/parts/a.sfz':
        '<master> pan=25\n<group> ampeg_release=0.25\n<region> sample="piano one.wav" key=c4\n#include "b.sfz"',
      'banks/parts/b.sfz':
        '<group> volume=-3\n<region> sample=two.wav key=61 transpose=2',
    },
    paths: ['samples/piano one.wav', 'samples/two.wav', 'samples/end.wav'],
  },
  {
    name: 'late initial CC and key switch conditions',
    selected: 'main.sfz',
    documents: {
      'main.sfz':
        '<group> sample=a.wav\n<region> key=60 locc7=90 hicc7=110\n<region> key=61 locc1=5\n<region> key=62 sw_last=c2\n<region> key=63 sw_last=d2\n<control> set_cc1=6 sw_default=c2',
    },
    paths: ['a.wav'],
  },
  {
    name: 'unsupported, ignored, missing and invalid regions',
    selected: 'main.sfz',
    documents: {
      'main.sfz':
        '<region> sample=a.wav key=60 unknown_opcode=5\n<region> sample=a.wav trigger=release\n<region> sample=a.wav lobend=5\n<region> sample=a.wav sw_last=c2\n<region> sample=lost.wav\n<region> sample=a.wav tune=bad\n<region> sample=a.wav tune=bad\n<region> sample=a.wav ampeg_attack=-1',
    },
    paths: ['a.wav'],
  },
  {
    name: 'comments, quotes, repeated defines and case-sensitive macros',
    selected: 'main.sfz',
    documents: {
      'main.sfz':
        '// prefix\n#define $Path "a.wav"\n#define $other $Path\n/* comment <region> sample=wrong */ <region> sample=$other key=C#4\n#define $Path b.wav\n<region> sample=$Path key=db4\n<region> sample=$path key=62',
    },
    paths: ['a.wav', 'b.wav'],
  },
  {
    name: 'key aliases overwrite inherited values in assignment order',
    selected: 'main.sfz',
    documents: {
      'main.sfz':
        '<global> key=60 hikey=80\n<master> key=62\n<group> lokey=40\n<region> sample=a.wav key=65 hikey=70 key=67 lokey=66\n<region> sample=a.wav volume=bad volume=2',
    },
    paths: ['a.wav'],
  },
  {
    name: 'numeric formats, Unicode whitespace and loop modes',
    selected: 'main.sfz',
    documents: {
      'main.sfz':
        '\ufeff#define $gain 0xA\n<region> sample=a.wav key=60 volume=$gain tune=+1e2 loop_mode=one_shot offset=0b10 end=0o10 ampeg_attack=" 0.01 "\n<region> sample=a.wav key=61 pan=\u00a0-25\u00a0 loop_mode=loop_sustain loop_start=-1',
    },
    paths: ['a.wav'],
  },
  {
    name: 'duplicate missing paths and empty regions',
    selected: 'main.sfz',
    documents: {
      'main.sfz':
        '<region> sample=z.wav\n<region> sample=音.wav\n<region> sample=z.wav\n<region> key=1\n<region> sample=a.wav lovel=0',
    },
    paths: ['a.wav'],
  },
  {
    name: 'numeric rejection and subnormal rounding agree with Number',
    selected: 'main.sfz',
    documents: {
      'main.sfz':
        '<region> sample=a.wav volume=+-1\n<region> sample=a.wav volume=1e-999\n<region> sample=a.wav volume=-1e-999\n<region> sample=a.wav volume=2.4703282292062327e-324\n<region> sample=a.wav volume=1.5e-323\n<region> sample=a.wav key=bad\n<region> sample=a.wav end=2.5',
    },
    paths: ['a.wav'],
  },
  {
    name: 'invalid opcodes are diagnosed before combined range validation',
    selected: 'main.sfz',
    documents: {
      'main.sfz':
        '<region> sample=a.wav volume=999 tune=bad\n<region> sample=a.wav loop_mode=ONE_SHOT\n<region> sample=a.wav sw_default=c2 sw_lokey=0 sw_hikey=127 sw_label="all"\n<region> sample=a.wav locc7=2.5',
    },
    paths: ['a.wav'],
  },
];

test.each(scenarios)(
  'SFZ parser matches app: $name',
  async ({ selected, documents, paths }) => {
    const upstream = await import(
      /* @vite-ignore */ pathToFileURL(
        `${process.env.PIPETUNE_EFFETUNE_SOURCE}/js/sfz/parser.js`
      ).href
    );
    const reference = await upstream.parseSfz({
      selectedPath: selected,
      readText: async (path: string) =>
        (documents as Record<string, string>)[path],
      hasSample: (path: string) => paths.includes(path),
      onDiagnostic: null,
    });
    const native = run(selected, documents, paths, 256 * 1024 * 1024);
    expect(native.status, native.stderr.toString()).toBe(0);
    expect(JSON.parse(native.stdout.toString())).toEqual(reference);
  }
);

test.each(['cycle', 'depth', 'visits', 'expanded', 'missing', 'escape'])(
  'SFZ %s include failure preserves the upstream error category',
  async (scenario) => {
    const upstream = await import(
      /* @vite-ignore */ pathToFileURL(
        `${process.env.PIPETUNE_EFFETUNE_SOURCE}/js/sfz/parser.js`
      ).href
    );
    let documents: Record<string, string> = {
      'main.sfz': '#include "main.sfz"',
    };
    let maximumBytes = 256 * 1024 * 1024;
    if (scenario === 'depth') {
      documents = { 'main.sfz': '#include "0.sfz"' };
      for (let index = 0; index < 33; ++index)
        documents[`${index}.sfz`] = `#include "${index + 1}.sfz"`;
    }
    if (scenario === 'visits')
      documents = {
        'main.sfz': '#include "empty.sfz"\n'.repeat(10000),
        'empty.sfz': '',
      };
    if (scenario === 'expanded') {
      documents = {
        'main.sfz': '#define $x abcdefghijklmnopqrstuvwxyz\n$x $x $x $x $x',
      };
      maximumBytes = 100;
    }
    if (scenario === 'missing')
      documents = { 'main.sfz': '#include "absent.sfz"' };
    if (scenario === 'escape')
      documents = { 'main.sfz': '#include "../outside.sfz"' };
    let expected: unknown;
    try {
      await upstream.parseSfz({
        selectedPath: 'main.sfz',
        readText: async (path: string) => documents[path],
        maxBytes: maximumBytes,
        onDiagnostic: null,
      });
    } catch (error) {
      expected = error;
    }
    expect(expected).toBeTruthy();
    const native = run('main.sfz', documents, [], maximumBytes);
    expect(native.status).not.toBe(0);
    expect(native.stderr.toString()).toBe(
      `${(expected as { code: string }).code}: ${(expected as Error).message}\n`
    );
  }
);
