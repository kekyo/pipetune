import {
  constants,
  accessSync,
  mkdtempSync,
  readFileSync,
  rmSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
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

const [
  cmake,
  buildDirectory,
  systemdAnalyze,
  installPrefix,
  binaryDirectory,
  systemdUserUnitDirectory,
  libraryDirectory,
  documentationDirectory,
  testService,
] = process.argv.slice(2);
if (
  !cmake ||
  !buildDirectory ||
  !systemdAnalyze ||
  !installPrefix ||
  !binaryDirectory ||
  !systemdUserUnitDirectory ||
  !libraryDirectory ||
  !documentationDirectory ||
  !testService
) {
  fail("install layout test arguments are incomplete");
} else {
  const stagingDirectory = mkdtempSync(join(tmpdir(), "pipetune-install-test-"));
  const installPath = (directory, fileName) => {
    const destination = directory.startsWith("/")
      ? directory
      : join(installPrefix, directory);
    return join(stagingDirectory, destination.replace(/^\/+/, ""), fileName);
  };
  try {
    const installed = spawnSync(cmake, ["--install", buildDirectory], {
      encoding: "utf8",
      env: { ...process.env, DESTDIR: stagingDirectory },
    });
    if (installed.status !== 0) {
      fail("staged PipeTune installation failed", installed);
    } else {
      const executable = installPath(binaryDirectory, "pipetune");
      const service = installPath(
        systemdUserUnitDirectory,
        "pipetune.service",
      );
      const environmentExample = installPath(
        documentationDirectory,
        "environment.example",
      );
      const scalarDspBackend = installPath(
        join(libraryDirectory, "pipetune"),
        "libeffetune-dsp-scalar.so",
      );
      const simdDspBackend = installPath(
        join(libraryDirectory, "pipetune"),
        "libeffetune-dsp-simd.so",
      );
      const architectureDspBackends = new Map([
        [
          "x64",
          [
            "libeffetune-dsp-simd-x86-64-v3.so",
            "libeffetune-dsp-simd-x86-64-v4.so",
          ],
        ],
        ["ia32", ["libeffetune-dsp-simd-x86-64-v3.so"]],
        ["arm64", ["libeffetune-dsp-simd-arm64-sve.so"]],
        ["arm", []],
        ["riscv64", []],
      ]).get(process.arch);
      if (architectureDspBackends === undefined) {
        fail(`unsupported install-test architecture: ${process.arch}`);
      }
      const dspBackendDocumentation = installPath(
        documentationDirectory,
        "dsp-backends.md",
      );
      try {
        accessSync(executable, constants.X_OK);
        accessSync(service, constants.R_OK);
        accessSync(environmentExample, constants.R_OK);
        accessSync(scalarDspBackend, constants.R_OK);
        accessSync(simdDspBackend, constants.R_OK);
        for (const backend of architectureDspBackends ?? []) {
          accessSync(
            installPath(join(libraryDirectory, "pipetune"), backend),
            constants.R_OK,
          );
        }
        accessSync(dspBackendDocumentation, constants.R_OK);
      } catch (error) {
        fail(`installed PipeTune layout is incomplete: ${error.message}`);
      }

      if (process.exitCode !== 1) {
        const serviceLines = readFileSync(service, "utf8")
          .split(/\r?\n/u);
        const environmentFiles = serviceLines.filter((line) =>
          line.startsWith("EnvironmentFile="),
        );
        const execStart = serviceLines.find((line) =>
          line.startsWith("ExecStart="),
        );
        const restart = serviceLines.find((line) =>
          line.startsWith("Restart="),
        );
        const startLimitInterval = serviceLines.find((line) =>
          line.startsWith("StartLimitIntervalSec="),
        );
        const startLimitBurst = serviceLines.find((line) =>
          line.startsWith("StartLimitBurst="),
        );
        if (environmentFiles.length !== 0) {
          fail(
            "PipeTune user service must let the daemon parse its configuration",
          );
        }
        if (
          !execStart?.endsWith(
            " daemon --config %E/pipetune/environment",
          )
        ) {
          fail("PipeTune user service does not launch the daemon subcommand");
        }
        if (
          restart !== "Restart=on-failure" ||
          startLimitInterval !== "StartLimitIntervalSec=30s" ||
          startLimitBurst !== "StartLimitBurst=3"
        ) {
          fail(
            "PipeTune user service must bound repeated daemon failures",
          );
        }

        const version = spawnSync(executable, ["--version"], {
          encoding: "utf8",
        });
        if (version.status !== 0 || !version.stdout.startsWith("PipeTune ")) {
          fail("installed PipeTune executable is not runnable", version);
        }

        const verified = spawnSync(
          systemdAnalyze,
          [
            "--user",
            "--man=no",
            "--generators=no",
            "verify",
            testService,
          ],
          { encoding: "utf8" },
        );
        if (verified.status !== 0) {
          fail("installed PipeTune user service is invalid", verified);
        }
      }
    }
  } finally {
    rmSync(stagingDirectory, { recursive: true, force: true });
  }
}
