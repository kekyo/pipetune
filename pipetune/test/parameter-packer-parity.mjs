import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';

const [runner, effetuneRoot] = process.argv.slice(2);
if (!runner || !effetuneRoot) {
  throw new Error('usage: parameter-packer-parity.mjs RUNNER EFFETUNE_ROOT');
}

const { loadParamSpecs } = await import(
  pathToFileURL(path.join(effetuneRoot, 'scripts/gen-dsp-params.mjs'))
);
const { DSP_PARAM_PACKERS } = await import(
  pathToFileURL(path.join(effetuneRoot, 'js/audio/dsp-params.generated.js'))
);

const makeDirectParameters = (spec, mode) => {
  const parameters = {};
  for (const field of spec.fields) {
    field.keys.forEach((key, index) => {
      if (mode === 'valid') {
        if (field.kind === 'bool') parameters[key] = index % 2 === 0;
        else if (field.kind === 'enum') parameters[key] = field.values.at(-1);
        else parameters[key] = index % 2 === 0 ? field.min : field.max;
      } else {
        parameters[key] = field.kind === 'bool' ? 2 : { invalid: true };
      }
    });
  }
  if (spec.structured) {
    parameters[spec.structured.key] = mode === 'valid' ? 'p00112x3456p78' : 12;
  }
  return parameters;
};

const makeContainerParameters = spec => {
  const parameters = {};
  for (const field of spec.fields) {
    if (field.arrayKey) {
      parameters[field.arrayKey] = field.defaults.map((value, index) =>
        field.kind === 'bool' ? index % 2 : field.kind === 'enum' ? field.values.at(-1) : field.max
      );
    }
    if (field.objectArrayKey) {
      parameters[field.objectArrayKey] ??= Array.from({ length: field.count }, () => ({}));
      parameters[field.objectArrayKey].forEach((entry, index) => {
        entry[field.memberKey] =
          field.kind === 'bool' ? index % 2 : field.kind === 'enum' ? field.values.at(-1) : field.min;
      });
    }
  }
  return parameters;
};

const cases = [];
for (const spec of loadParamSpecs(path.join(effetuneRoot, 'dsp/plugins'))) {
  cases.push({ type: spec.type, parameters: {} });
  cases.push({ type: spec.type, parameters: makeDirectParameters(spec, 'valid') });
  cases.push({ type: spec.type, parameters: makeDirectParameters(spec, 'invalid') });
  for (const field of spec.fields.filter(field => field.rejectInvalid)) {
    for (const key of field.keys) {
      cases.push({
        type: spec.type,
        parameters: { [key]: { invalid: true } },
        expectedRejectedKey: key
      });
    }
  }
  if (spec.fields.some(field => field.arrayKey || field.objectArrayKey)) {
    cases.push({ type: spec.type, parameters: makeContainerParameters(spec) });
  }
}
const integerSpec = loadParamSpecs(path.join(effetuneRoot, 'dsp/plugins')).find(spec =>
  spec.fields.some(field =>
    field.kind === 'int' && field.min <= 1 && field.max >= 1 && !field.defaults.includes(1)
  )
);
if (!integerSpec) throw new Error('EffeTune has no suitable integer field for real-number parity');
const integerField = integerSpec.fields.find(field =>
  field.kind === 'int' && field.min <= 1 && field.max >= 1 && !field.defaults.includes(1)
);
cases.push({
  type: integerSpec.type,
  parameters: { [integerField.keys[0]]: '__PIPETUNE_REAL_ONE__' },
  expectedParameters: { [integerField.keys[0]]: 1 }
});
cases.push({
  type: 'MatrixPlugin',
  parameters: { mx: '00'.repeat(1025) },
  expectedError: true
});

const temporaryDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'pipetune-packer-'));
const requestPath = path.join(temporaryDirectory, 'request.json');
try {
  const encodedRequest = JSON.stringify({ cases })
    .replaceAll('"__PIPETUNE_REAL_ONE__"', '1.0');
  fs.writeFileSync(requestPath, encodedRequest);
  const execution = spawnSync(runner, [requestPath], {
    encoding: 'utf8',
    maxBuffer: 16 * 1024 * 1024
  });
  if (execution.status !== 0) {
    throw new Error(`native parameter packer failed:\n${execution.stderr}`);
  }
  const actual = JSON.parse(execution.stdout);
  if (!Array.isArray(actual.cases) || actual.cases.length !== cases.length) {
    throw new Error('native parameter packer returned an invalid case count');
  }

  cases.forEach((testCase, index) => {
    const packer = DSP_PARAM_PACKERS.get(testCase.type);
    if (!packer) throw new Error(`missing upstream packer for ${testCase.type}`);
    const expectedParameters = testCase.expectedParameters ?? testCase.parameters;
    let expectedFloats = [];
    let expectedBytes = [];
    let expectedError = false;
    try {
      expectedFloats = [...packer.pack(expectedParameters)];
      expectedBytes = packer.packBytes ? [...packer.packBytes(expectedParameters)] : [];
    } catch {
      expectedError = true;
    }
    const native = actual.cases[index];
    if (testCase.expectedRejectedKey) {
      if (!expectedError || native.error !== `invalid enum parameter ${testCase.expectedRejectedKey}`) {
        throw new Error(
          `parameter packing must reject invalid enum ${testCase.expectedRejectedKey} for ${testCase.type}`
        );
      }
      return;
    }
    if (testCase.expectedError) {
      if (native.error !== 'structured parameter capacity exceeded') {
        throw new Error(`parameter packing must reject excess structured data for ${testCase.type}`);
      }
      return;
    }
    if (expectedError) {
      if (native.error === '') {
        throw new Error(`parameter packing must reject invalid parameters for ${testCase.type}`);
      }
      return;
    }
    if (native.type !== testCase.type ||
        native.error !== '' ||
        native.hash !== packer.hash ||
        native.floatCount !== packer.floatCount ||
        JSON.stringify(native.floats) !== JSON.stringify(expectedFloats) ||
        JSON.stringify(native.bytes) !== JSON.stringify(expectedBytes)) {
      throw new Error(
        `parameter packing differs for ${testCase.type}, case ${index}\n` +
        `parameters: ${JSON.stringify(testCase.parameters)}\n` +
        `expected: ${JSON.stringify({ floats: expectedFloats, bytes: expectedBytes })}\n` +
        `actual: ${JSON.stringify({ floats: native.floats, bytes: native.bytes })}`
      );
    }
  });
  console.log(`Matched ${cases.length} native parameter-packing cases across ${DSP_PARAM_PACKERS.size} DSPs.`);
} finally {
  fs.rmSync(temporaryDirectory, { recursive: true, force: true });
}
