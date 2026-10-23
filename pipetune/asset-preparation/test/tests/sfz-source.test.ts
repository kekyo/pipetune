import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { expect, test } from 'vitest';

const upstream = (name: string) =>
  pathToFileURL(`${process.env.PIPETUNE_EFFETUNE_SOURCE}/js/sfz/${name}.js`)
    .href;

const frame = (value: string | Buffer) => {
  const bytes = Buffer.isBuffer(value) ? value : Buffer.from(value);
  const size = Buffer.alloc(4);
  size.writeUInt32LE(bytes.length);
  return [size, bytes];
};

const word = (value: number) => {
  const bytes = Buffer.alloc(4);
  bytes.writeUInt32LE(value);
  return bytes;
};

const runBank = (
  selected: string,
  banks: string[],
  documents: Record<string, string>,
  locale: string | undefined
) => {
  const input = Buffer.concat([
    ...frame(selected),
    word(banks.length),
    ...banks.flatMap(frame),
    word(Object.keys(documents).length),
    ...Object.entries(documents).flatMap(([path, contents]) => [
      ...frame(path),
      ...frame(contents),
    ]),
  ]);
  return spawnSync(process.env.PIPETUNE_SFZ_DRIVER!, ['--source-bank'], {
    input,
    encoding: 'utf8',
    maxBuffer: 8 * 1024 * 1024,
    env: {
      ...process.env,
      ...(locale === undefined ? {} : { LC_ALL: locale }),
    },
  });
};

const referenceBank = (
  selected: string,
  banks: string[],
  documents: Record<string, string>,
  locale: string
) => {
  const script = `import { readFileSync } from 'node:fs';
    import { readSfzBankDefines } from ${JSON.stringify(upstream('source'))};
    const { selected, banks, documents } = JSON.parse(readFileSync(0, 'utf8'));
    const dependencies = [];
    const result = await readSfzBankDefines(selected, banks, async path => {
      dependencies.push(path); return documents[path] ?? null;
    });
    console.log(JSON.stringify({ defines: Object.fromEntries(result.defines),
      bankPath: result.bankPath ?? null, dependencies }));`;
  const native = spawnSync(
    process.execPath,
    ['--input-type=module', '-e', script],
    {
      input: JSON.stringify({ selected, banks, documents }),
      encoding: 'utf8',
      env: { ...process.env, LC_ALL: locale },
    }
  );
  expect(native.status, native.stderr).toBe(0);
  return JSON.parse(native.stdout);
};

const runParse = (
  selected: string,
  definitions: Record<string, string>,
  documents: Record<string, string>
) => {
  const input = Buffer.concat([
    ...frame(selected),
    word(Object.keys(definitions).length),
    ...Object.entries(definitions).flatMap(([key, value]) => [
      ...frame(key),
      ...frame(value),
    ]),
    word(Object.keys(documents).length),
    ...Object.entries(documents).flatMap(([path, contents]) => [
      ...frame(path),
      ...frame(contents),
    ]),
  ]);
  return spawnSync(process.env.PIPETUNE_SFZ_DRIVER!, ['--source-parse'], {
    input,
    encoding: 'utf8',
    maxBuffer: 8 * 1024 * 1024,
  });
};

test('SFZ source decoder matches strict UTF-8, BOM and Windows-1252 for all bytes', async () => {
  const source = await import(/* @vite-ignore */ upstream('source'));
  const inputs = [
    Buffer.alloc(0),
    Buffer.from('plain ASCII'),
    Buffer.from([0xef, 0xbb, 0xbf, 0x61]),
    Buffer.from('日本語'),
    Buffer.from([0xef, 0xbb, 0xbf, 0xff]),
    Buffer.from([0xc0, 0xaf]),
    Buffer.from([0xed, 0xa0, 0x80]),
    Buffer.from([0xf4, 0x90, 0x80, 0x80]),
    Buffer.from([0xe2, 0x82]),
    Buffer.from(Array.from({ length: 256 }, (_, byte) => byte)),
  ];
  for (const bytes of inputs) {
    const actual = spawnSync(
      process.env.PIPETUNE_SFZ_DRIVER!,
      ['--source-decode'],
      {
        input: bytes,
        maxBuffer: 1024 * 1024,
      }
    );
    expect(actual.status, actual.stderr.toString()).toBe(0);
    expect(actual.stdout.toString('utf8')).toBe(source.decodeSfzText(bytes));
  }
});

test('SFZ bank candidates follow depth, locale order, ARIA registration and fallback', async () => {
  const selected = 'Banks/Grand/Main.sfz';
  const banks = [
    'other/skip.bank.xml',
    'Banks/root.bank.xml',
    'Banks/Grand/z.bank.xml',
    'Banks/Grand/ä.bank.xml',
    'Banks/Grand/a.bank.xml',
  ];
  const documents = Object.fromEntries(
    banks.map((path) => [
      path,
      '<AriaBank><Define name="$sample_dir" value="$Banks/ignored" />',
    ])
  );
  documents['Banks/Grand/a.bank.xml'] =
    '<AriaBank><AriaElement path="Main.sfz" />' +
    '<Define name="$sample_dir" value="$Banks/ignored" />' +
    '<Define name="$tone" value="first" /><Define name="$tone" value="last &amp; best" />';
  documents['Banks/Grand/ä.bank.xml'] =
    '<AriaBank><AriaElement path="OTHER.SFZ" />';
  for (const locale of ['C', 'C.UTF-8', 'en_US.UTF-8', 'ja_JP.UTF-8']) {
    const expected = referenceBank(selected, banks, documents, locale);
    const native = runBank(selected, banks, documents, locale);
    expect(native.status, native.stderr).toBe(0);
    expect(JSON.parse(native.stdout)).toEqual(expected);
  }
});

test('SFZ bank consults all candidates in locale order when none register the instrument', () => {
  const selected = 'Banks/Grand/Main.sfz';
  const banks = [
    'Banks/root.bank.xml',
    'Banks/Grand/z.bank.xml',
    'Banks/Grand/ä.bank.xml',
    'Banks/Grand/a.bank.xml',
    'Banks/Grand/_a.bank.xml',
    'Banks/Grand/-a.bank.xml',
    'Banks/Grand/e\u0301.bank.xml',
    'Banks/Grand/é.bank.xml',
  ];
  const documents = Object.fromEntries(
    banks.map((path) => [path, '<AriaBank><AriaElement path="Other.sfz" />'])
  );
  for (const locale of ['C', 'C.UTF-8', 'en_US.UTF-8', 'ja_JP.UTF-8']) {
    const expected = referenceBank(selected, banks, documents, locale);
    const native = runBank(selected, banks, documents, locale);
    expect(native.status, native.stderr).toBe(0);
    expect(JSON.parse(native.stdout)).toEqual(expected);
  }
});

test('SFZ bank reads sole same-directory candidate and decodes XML entities', async () => {
  const source = await import(/* @vite-ignore */ upstream('source'));
  const banks = ['Banks/program.bank.xml'];
  const documents = {
    'Banks/program.bank.xml':
      '<AriaBank><Define name="$quoted" ' +
      'value="&amp;&quot;&apos;&lt;&gt;" /><Define name="$sample_dir" value="$Banks/Samples" />' +
      '<Define name="$invalid-key" value="ignored" />',
  };
  const expected = await source.readSfzBankDefines(
    'Banks/Inst/Piano.sfz',
    banks,
    async (path: string) => documents[path as keyof typeof documents] ?? null
  );
  const native = runBank('Banks/Inst/Piano.sfz', banks, documents, undefined);
  expect(native.status, native.stderr).toBe(0);
  expect(JSON.parse(native.stdout)).toEqual({
    defines: Object.fromEntries(expected.defines),
    bankPath: expected.bankPath,
    dependencies: banks,
  });
});

test.each(['\u00a0', '\ufeff', '\u2028', '\u2029', '\u3000'])(
  'SFZ bank attributes accept JavaScript whitespace %j around equals',
  async (whitespace) => {
    const source = await import(/* @vite-ignore */ upstream('source'));
    const selected = 'Banks/Main.sfz';
    const banks = ['Banks/main.bank.xml'];
    const text =
      '<AriaBank><Define name' +
      whitespace +
      '=' +
      whitespace +
      '"$voice"' +
      ' value' +
      whitespace +
      '=' +
      whitespace +
      '"warm' +
      whitespace +
      'tone" />';
    const documents = { [banks[0]]: text };
    const expected = await source.readSfzBankDefines(
      selected,
      banks,
      async (path: string) => documents[path] ?? null
    );
    const native = runBank(selected, banks, documents, undefined);
    expect(native.status, native.stderr).toBe(0);
    expect(JSON.parse(native.stdout)).toEqual({
      defines: Object.fromEntries(expected.defines),
      bankPath: expected.bankPath,
      dependencies: banks,
    });
  }
);

test('SFZ bank accepts a 100 KiB quoted define without regex recursion failure', async () => {
  const source = await import(/* @vite-ignore */ upstream('source'));
  const selected = 'Banks/Main.sfz';
  const banks = ['Banks/main.bank.xml'];
  const documents = {
    [banks[0]]:
      '<AriaBank><Define name="$large" value="' +
      'a'.repeat(100 * 1024) +
      '" />',
  };
  const expected = await source.readSfzBankDefines(
    selected,
    banks,
    async (path: string) => documents[path] ?? null
  );
  const native = runBank(selected, banks, documents, undefined);
  expect(native.status, `${native.signal}: ${native.stderr}`).toBe(0);
  expect(JSON.parse(native.stdout)).toEqual({
    defines: Object.fromEntries(expected.defines),
    bankPath: expected.bankPath,
    dependencies: banks,
  });
});

test.each([
  '<DefineX name="$skip" value="no" /><Define name="$keep" value="yes" />',
  '<Define data-name="$first" name="$last" value="found" />',
  '<Define NAME="$first" name="$last" VALUE="old" value="new" />',
  "<Define name='$single' value='a &amp; b' />",
  '<Define name="$x" value="one>two" /><Define name="$y" value="three" />',
  '<Define name="$x" value="unfinished <Define name="$y" value="nested" />',
  '<Define name="$x" value="case &AMP; stays" />',
  '<Define name="$x" value="first" /><Define name="$x" value="second" />',
])(
  'SFZ bank attribute scan matches the upstream expression: %s',
  async (fragment) => {
    const source = await import(/* @vite-ignore */ upstream('source'));
    const selected = 'Banks/Main.sfz';
    const banks = ['Banks/main.bank.xml'];
    const documents = { [banks[0]]: '<AriaBank>' + fragment };
    const expected = await source.readSfzBankDefines(
      selected,
      banks,
      async (path: string) => documents[path] ?? null
    );
    const native = runBank(selected, banks, documents, undefined);
    expect(native.status, native.stderr).toBe(0);
    expect(JSON.parse(native.stdout)).toEqual({
      defines: Object.fromEntries(expected.defines),
      bankPath: expected.bankPath,
      dependencies: banks,
    });
  }
);

test('SFZ bank requires each unread candidate and records skipped definitions', () => {
  const selected = 'Banks/Inst/Main.sfz';
  const banks = ['Banks/Inst/a.bank.xml', 'Banks/Inst/b.bank.xml'];
  const unresolved = runBank(selected, banks, {}, undefined);
  expect(unresolved.status).toBe(1);
  expect(unresolved.stderr).toContain('required:Banks/Inst/a.bank.xml');
  const native = runBank(
    selected,
    banks,
    {
      'Banks/Inst/a.bank.xml': '',
      'Banks/Inst/b.bank.xml': '<AriaBank><AriaElement path="Main.sfz" />',
    },
    undefined
  );
  expect(native.status, native.stderr).toBe(0);
  expect(JSON.parse(native.stdout)).toEqual({
    defines: {},
    bankPath: 'Banks/Inst/b.bank.xml',
    dependencies: banks,
  });
});

test('SFZ bank uses Unicode lowercase for ARIA paths and tolerates malformed unrelated XML', async () => {
  const source = await import(/* @vite-ignore */ upstream('source'));
  const selected = 'Banks/Ärzte.sfz';
  const banks = ['Banks/a.bank.xml', 'Banks/b.bank.xml'];
  const documents = {
    'Banks/a.bank.xml': '<AriaBank><AriaElement path="Other.sfz"',
    'Banks/b.bank.xml':
      '<AriaBank><ARIAELEMENT PATH="ärzte.SFZ" />' +
      '<DEFINE NAME="$voice" VALUE="shared &lt;music&gt;" />',
  };
  const expected = await source.readSfzBankDefines(
    selected,
    banks,
    async (path: string) => documents[path as keyof typeof documents] ?? null
  );
  const native = runBank(selected, banks, documents, undefined);
  expect(native.status, native.stderr).toBe(0);
  expect(JSON.parse(native.stdout)).toEqual({
    defines: Object.fromEntries(expected.defines),
    bankPath: expected.bankPath,
    dependencies: banks,
  });
});

test.each([
  ['Banks/ΟΣ', 'ος'],
  ['Banks/\u{10400}.sfz', '\u{10428}.SFZ'],
])(
  'SFZ ARIA path comparison uses context-sensitive Unicode lowercase: %s',
  async (selected, ariaPath) => {
    const source = await import(/* @vite-ignore */ upstream('source'));
    const banks = ['Banks/a.bank.xml', 'Banks/b.bank.xml'];
    const documents = {
      [banks[0]]: '<AriaBank><AriaElement path="Other.sfz" />',
      [banks[1]]: `<AriaBank><AriaElement path="${ariaPath}" /><Define name="$selected" value="yes" />`,
    };
    const expected = await source.readSfzBankDefines(
      selected,
      banks,
      async (path: string) => documents[path] ?? null
    );
    const native = runBank(selected, banks, documents, undefined);
    expect(native.status, native.stderr).toBe(0);
    expect(JSON.parse(native.stdout)).toEqual({
      defines: Object.fromEntries(expected.defines),
      bankPath: expected.bankPath,
      dependencies: banks,
    });
  }
);

test('SFZ bank rejects an ARIA path escaping the selected folder', async () => {
  const source = await import(/* @vite-ignore */ upstream('source'));
  const selected = 'Banks/Inst/Main.sfz';
  const banks = ['Banks/Inst/main.bank.xml'];
  const documents = {
    'Banks/Inst/main.bank.xml':
      '<AriaBank><AriaElement path="../../../outside.sfz" />',
  };
  await expect(
    source.readSfzBankDefines(
      selected,
      banks,
      async (path: string) => documents[path as keyof typeof documents] ?? null
    )
  ).rejects.toMatchObject({ code: 'prepare' });
  const native = runBank(selected, banks, documents, undefined);
  expect(native.status).toBe(1);
  expect(native.stderr).toContain('prepare:');
});

test('SFZ parser applies initial bank defines and local overrides like the app', async () => {
  const parser = await import(/* @vite-ignore */ upstream('parser'));
  const selected = 'Banks/Main.sfz';
  const documents = {
    [selected]:
      '<region> sample=$sample_dir/piano.wav key=60\n' +
      '#define $sample_dir local\n<region> sample=$sample_dir/piano.wav key=61',
  };
  const defines = { $sample_dir: 'global' };
  const expected = await parser.parseSfz({
    selectedPath: selected,
    readText: async (path: string) =>
      documents[path as keyof typeof documents] ?? null,
    defines: new Map(Object.entries(defines)),
    hasSample: null,
    onDiagnostic: null,
  });
  const native = runParse(selected, defines, documents);
  expect(native.status, native.stderr).toBe(0);
  expect(
    JSON.parse(native.stdout).regions.map(
      (region: { sample: string }) => region.sample
    )
  ).toEqual(
    expected.regions.map((region: { sample: string }) => region.sample)
  );
});
