import childProcess from 'node:child_process';
import process from 'node:process';
import { createInterface } from 'node:readline';

const fail = message => {
  console.error(message);
  process.exitCode = 1;
};

const contains = async (executable, arguments_, pattern) => {
  const child = childProcess.spawn(executable, arguments_, {
    stdio: ['ignore', 'pipe', 'pipe']
  });
  let diagnostic = '';
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', text => { diagnostic += text; });
  child.on('error', error => { diagnostic = error.message; });
  const completed = new Promise(resolve => { child.on('close', resolve); });
  let matched = false;
  // Debug symbol tables can exceed spawnSync's output limit. Drain every
  // line without retaining the table or excluding local DSP ABI symbols.
  for await (const line of createInterface({ input: child.stdout })) {
    if (pattern.test(line)) matched = true;
  }
  if (await completed !== 0) fail(`${executable} failed: ${diagnostic}`);
  return matched;
};

const [binaryPath, nmPath, readelfPath, ...extra] = process.argv.slice(2);
if (!binaryPath || !nmPath || !readelfPath || extra.length !== 0) {
  fail('usage: dsp-linkage.mjs BINARY NM READELF');
} else {
  if (await contains(nmPath, ['--defined-only', binaryPath],
      /\bet_(?:abi_version|engine_create|pipeline_process)\b/u)) {
    fail('PipeTune must not contain statically linked EffeTune DSP ABI symbols');
  }

  if (await contains(readelfPath, ['-d', binaryPath],
      /Shared library: \[libeffetune-dsp-/u)) {
    fail('PipeTune must load DSP backends explicitly instead of using DT_NEEDED');
  }
}
