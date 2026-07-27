import { mkdtempSync, rmSync } from "node:fs";
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

const run = (command, commandArguments) =>
  spawnSync(command, commandArguments, { encoding: "utf8" });

const [cmake, sourceDirectory] = process.argv.slice(2);
if (!cmake || !sourceDirectory) {
  fail("component build test arguments are incomplete");
} else {
  const buildDirectory = mkdtempSync(
    join(tmpdir(), "pipetune-component-build-"),
  );
  try {
    const configured = run(cmake, [
      "-S",
      sourceDirectory,
      "-B",
      buildDirectory,
      "-DCMAKE_BUILD_TYPE=Release",
      "-DBUILD_TESTING=OFF",
    ]);
    if (configured.status !== 0) {
      fail("standalone PipeTune component configuration failed", configured);
    } else {
      const built = run(cmake, [
        "--build",
        buildDirectory,
        "--target",
        "pipetune",
        "--parallel",
        "2",
      ]);
      if (built.status !== 0) {
        fail("standalone PipeTune component build failed", built);
      } else {
        const version = run(join(buildDirectory, "pipetune"), ["--version"]);
        if (
          version.status !== 0 ||
          !version.stdout.startsWith("pipetune ")
        ) {
          fail("standalone PipeTune component is not runnable", version);
        }
      }
    }
  } finally {
    rmSync(buildDirectory, { recursive: true, force: true });
  }
}
