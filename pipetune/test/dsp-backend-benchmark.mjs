import { spawnSync } from "node:child_process";

const [benchmark, preset] = process.argv.slice(2);
if (!benchmark || !preset) {
  process.stderr.write("benchmark test arguments are incomplete\n");
  process.exit(1);
}

const fail = (message, result) => {
  process.stderr.write(`${message}\n`);
  if (result?.stdout) {
    process.stderr.write(result.stdout);
  }
  if (result?.stderr) {
    process.stderr.write(result.stderr);
  }
  process.exit(1);
};

const help = spawnSync(benchmark, ["--help"], { encoding: "utf8" });
if (
  help.status !== 0 ||
  !help.stdout.includes("pipetune-dsp-benchmark") ||
  !help.stdout.includes("--measure-blocks")
) {
  fail("DSP backend benchmark help differs", help);
}

const measured = spawnSync(
  benchmark,
  [
    "--json",
    "--warmup-blocks",
    "1",
    "--measure-blocks",
    "4",
    "--frames",
    "64",
    preset,
  ],
  { encoding: "utf8" },
);
if (measured.status !== 0) {
  fail("DSP backend benchmark could not process a preset", measured);
}

let report;
try {
  report = JSON.parse(measured.stdout);
} catch (error) {
  fail(`DSP backend benchmark JSON is invalid: ${error.message}`, measured);
}
const result = report?.results?.[0];
if (
  report.sampleRate !== 48000 ||
  report.channels !== 2 ||
  report.framesPerBlock !== 64 ||
  report.warmupBlocks !== 1 ||
  report.measureBlocks !== 4 ||
  report.results.length !== 1 ||
  result.preset !== preset ||
  result.activePluginCount < 1 ||
  !(result.scalarNanosecondsPerFrame > 0) ||
  !(result.simdNanosecondsPerFrame > 0) ||
  !(result.speedup > 0)
) {
  fail("DSP backend benchmark result differs", measured);
}

const invalid = spawnSync(
  benchmark,
  ["--frames", "16", preset],
  { encoding: "utf8" },
);
if (
  invalid.status === 0 ||
  !invalid.stderr.includes("at least 32")
) {
  fail("DSP backend benchmark must reject invalid frame counts", invalid);
}
