import childProcess from 'node:child_process';
import process from 'node:process';

const fail = message => {
  console.error(message);
  process.exitCode = 1;
};

const run = (executable, arguments_) => {
  const result = childProcess.spawnSync(executable, arguments_, {
    encoding: 'utf8'
  });
  if (result.status !== 0) {
    fail(`${executable} failed: ${result.stderr || result.stdout}`);
    return null;
  }
  return result.stdout;
};

const [binaryPath, nmPath, readelfPath, ...extra] = process.argv.slice(2);
if (!binaryPath || !nmPath || !readelfPath || extra.length !== 0) {
  fail('usage: dsp-linkage.mjs BINARY NM READELF');
} else {
  const symbols = run(nmPath, ['--defined-only', binaryPath]);
  if (symbols !== null && /\bet_(?:abi_version|engine_create|pipeline_process)\b/u.test(symbols)) {
    fail('PipeTune must not contain statically linked EffeTune DSP ABI symbols');
  }

  const dynamicSection = run(readelfPath, ['-d', binaryPath]);
  if (dynamicSection !== null && /Shared library: \[libeffetune-dsp-/u.test(dynamicSection)) {
    fail('PipeTune must load DSP backends explicitly instead of using DT_NEEDED');
  }
}
