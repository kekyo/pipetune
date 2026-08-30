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
  !help.stdout.includes("--measure-blocks") ||
  !help.stdout.includes("1 through 16") ||
  !help.stdout.includes("every available backend variant")
) {
  fail("DSP backend benchmark help differs", help);
}

const sixteenChannels = spawnSync(
  benchmark,
  ["--channels", "16", "--help"],
  { encoding: "utf8" },
);
if (sixteenChannels.status !== 0) {
  fail("DSP backend benchmark must accept sixteen channels", sixteenChannels);
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
const reportedVariants = report?.variants;
const measuredVariants = result?.variants;
const variantNames = reportedVariants?.map((variant) => variant.name);
if (
  report.sampleRate !== 48000 ||
  report.channels !== 2 ||
  report.framesPerBlock !== 64 ||
  report.warmupBlocks !== 1 ||
  report.measureBlocks !== 4 ||
  report.results.length !== 1 ||
  result.preset !== preset ||
  result.activePluginCount < 1 ||
  !Array.isArray(reportedVariants) ||
  reportedVariants.length < 2 ||
  reportedVariants[0].name !== "scalar" ||
  new Set(variantNames).size !== variantNames.length ||
  reportedVariants.some(
    (variant) =>
      typeof variant.name !== "string" ||
      typeof variant.cpuRequirement !== "string",
  ) ||
  !Array.isArray(measuredVariants) ||
  measuredVariants.length !== reportedVariants.length ||
  measuredVariants.some(
    (variant, index) =>
      variant.name !== reportedVariants[index].name ||
      !(variant.nanosecondsPerFrame > 0) ||
      !(variant.speedupVsScalar > 0) ||
      !Number.isFinite(variant.checksum),
  ) ||
  measuredVariants[0].speedupVsScalar !== 1
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

const invalidChannels = spawnSync(
  benchmark,
  ["--channels", "17", preset],
  { encoding: "utf8" },
);
if (
  invalidChannels.status === 0 ||
  !invalidChannels.stderr.includes("between 1 and 16")
) {
  fail("DSP backend benchmark must reject seventeen channels", invalidChannels);
}
