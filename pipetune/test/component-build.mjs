import {
  existsSync,
  mkdtempSync,
  readFileSync,
  rmSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { spawnSync } from "node:child_process";

const fail = (message, command) => {
  process.stderr.write(`${message}\n`);
  if (command?.stdout) {
    process.stderr.write(command.stdout);
  }
  if (command?.stderr) {
    process.stderr.write(command.stderr);
  }
  process.exitCode = 1;
};

const run = (command, commandArguments, options) =>
  spawnSync(command, commandArguments, {
    encoding: "utf8",
    ...options,
  });

const overriddenVersion = "9.8.7-test";

const [cmake, sourceDirectory, workspaceExecutable, backendArtifactTest] =
  process.argv.slice(2);
if (
  !cmake ||
  !sourceDirectory ||
  !workspaceExecutable ||
  !backendArtifactTest
) {
  fail("component build test arguments are incomplete");
} else {
  const effetunePackage = JSON.parse(
    readFileSync(
      join(dirname(sourceDirectory), "deps", "effetune", "package.json"),
      "utf8",
    ),
  );
  if (typeof effetunePackage.version !== "string") {
    fail("EffeTune package version is unavailable");
  }
  const effetuneVersion = effetunePackage.version;
  const resolvedVersion = run(
    "npx",
    ["screw-up", "format", "-e", "{version}", "-f"],
    { cwd: dirname(sourceDirectory) },
  );
  if (resolvedVersion.status !== 0) {
    fail("screw-up version resolution failed", resolvedVersion);
  } else {
    const workspaceVersion = run(workspaceExecutable, ["--version"], {});
    if (
      workspaceVersion.status !== 0 ||
      workspaceVersion.stdout !==
        `PipeTune ${resolvedVersion.stdout.trim()}, EffeTune DSP ${effetuneVersion}\n`
    ) {
      fail(
        "workspace PipeTune executable did not report the build versions",
        workspaceVersion,
      );
    }
  }

  const buildDirectory = mkdtempSync(
    join(tmpdir(), "pipetune-component-build-"),
  );
  try {
    const configured = run(
      cmake,
      [
        "-S",
        sourceDirectory,
        "-B",
        buildDirectory,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_TESTING=OFF",
        `-DPIPETUNE_BUILD_VERSION=${overriddenVersion}`,
      ],
      {},
    );
    if (configured.status !== 0) {
      fail("standalone PipeTune component configuration failed", configured);
    } else {
      const built = run(
        cmake,
        [
          "--build",
          buildDirectory,
          "--target",
          "pipetune",
          "--parallel",
          "2",
        ],
        {},
      );
      if (built.status !== 0) {
        fail("standalone PipeTune component build failed", built);
      } else {
        const version = run(
          join(buildDirectory, "pipetune"),
          ["--version"],
          {},
        );
        if (
          version.status !== 0 ||
          version.stdout !==
            `PipeTune ${overriddenVersion}, EffeTune DSP ${effetuneVersion}\n`
        ) {
          fail(
            "standalone PipeTune component did not report the build versions",
            version,
          );
        }

        const benchmarkBuilt = run(
          cmake,
          [
            "--build",
            buildDirectory,
            "--target",
            "pipetune-dsp-benchmark",
            "--parallel",
            "2",
          ],
          {},
        );
        if (benchmarkBuilt.status !== 0) {
          fail(
            "standalone DSP backend benchmark build failed",
            benchmarkBuilt,
          );
        } else {
          const backendNames = [
            "libeffetune-dsp-scalar.so",
            "libeffetune-dsp-simd.so",
            "libeffetune-dsp-simd-x86-64-v3.so",
            "libeffetune-dsp-simd-x86-64-v4.so",
            "libeffetune-dsp-simd-arm64-sve.so",
          ];
          const backendPaths = backendNames
            .map((name) => join(buildDirectory, name))
            .filter((path) => existsSync(path));
          const goldenRoot = join(
            dirname(sourceDirectory),
            "deps",
            "effetune",
            "dsp",
            "plugins",
          );
          const goldenPaths = [
            join(
              goldenRoot,
              "dynamics",
              "auto_leveler",
              "golden",
              "case-008.f32",
            ),
            join(
              goldenRoot,
              "lofi",
              "bluetooth_sbc_simulator",
              "golden",
              "case-001.f32",
            ),
            join(
              goldenRoot,
              "lofi",
              "cassette_artifacts",
              "golden",
              "case-001.f32",
            ),
            join(
              goldenRoot,
              "lofi",
              "tape_artifacts",
              "golden",
              "case-001.f32",
            ),
            join(
              goldenRoot,
              "lofi",
              "vinyl_artifacts",
              "golden",
              "case-003.f32",
            ),
          ];
          const artifactCheck = run(
            backendArtifactTest,
            [...goldenPaths, ...backendPaths],
            {},
          );
          if (artifactCheck.status !== 0) {
            fail(
              "standalone Release DSP backends failed artifact validation",
              artifactCheck,
            );
          }

          const benchmark = run(
            join(buildDirectory, "pipetune-dsp-benchmark"),
            [
              "--json",
              "--warmup-blocks",
              "1",
              "--measure-blocks",
              "2",
              join(
                dirname(sourceDirectory),
                "deps",
                "effetune",
                "presets",
                "processor",
                "bbe.effetune_preset",
              ),
            ],
            {},
          );
          if (benchmark.status !== 0) {
            fail(
              "standalone DSP backend benchmark was not runnable",
              benchmark,
            );
          }
        }
      }
    }
  } finally {
    rmSync(buildDirectory, { recursive: true, force: true });
  }
}
