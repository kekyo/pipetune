import { mkdtempSync, rmSync } from "node:fs";
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

const [cmake, sourceDirectory, workspaceExecutable] = process.argv.slice(2);
if (!cmake || !sourceDirectory || !workspaceExecutable) {
  fail("component build test arguments are incomplete");
} else {
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
        `pipetune ${resolvedVersion.stdout.trim()}\n`
    ) {
      fail(
        "workspace PipeTune executable did not use the screw-up version",
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
          version.stdout !== `pipetune ${overriddenVersion}\n`
        ) {
          fail(
            "standalone PipeTune component did not use the overridden version",
            version,
          );
        }
      }
    }
  } finally {
    rmSync(buildDirectory, { recursive: true, force: true });
  }
}
