import { constants, accessSync, mkdtempSync, rmSync } from "node:fs";
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
      try {
        accessSync(executable, constants.X_OK);
        accessSync(service, constants.R_OK);
        accessSync(environmentExample, constants.R_OK);
      } catch (error) {
        fail(`installed PipeTune layout is incomplete: ${error.message}`);
      }

      if (process.exitCode !== 1) {
        const version = spawnSync(executable, ["--version"], {
          encoding: "utf8",
        });
        if (version.status !== 0 || !version.stdout.startsWith("pipetune ")) {
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
