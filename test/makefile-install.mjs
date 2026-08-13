import {
  chmodSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const [projectRoot, makeExecutable] = process.argv.slice(2);

const fail = (message, result = undefined) => {
  console.error(message);
  if (result?.error) {
    console.error(result.error);
  }
  if (result?.stdout) {
    console.error(result.stdout);
  }
  if (result?.stderr) {
    console.error(result.stderr);
  }
  process.exit(1);
};

const assertEqual = (expected, actual, message) => {
  if (actual !== expected) {
    fail(
      `${message}\nexpected:\n${JSON.stringify(expected)}\nactual:\n${JSON.stringify(actual)}`,
    );
  }
};

const assertSuccess = (result, message) => {
  if (result.status !== 0) {
    fail(message, result);
  }
};

if (!projectRoot || !makeExecutable) {
  fail("Makefile install test arguments are incomplete");
}

const temporaryRoot = mkdtempSync(join(tmpdir(), "pipetune-makefile-test-"));

try {
  const binDirectory = join(temporaryRoot, "bin");
  const invocationRecord = join(temporaryRoot, "cmake-invocations.txt");
  const buildDirectory = join(temporaryRoot, "release");
  const prefix = "/manual-prefix";
  mkdirSync(binDirectory);
  const fakeCmake = join(binDirectory, "cmake");
  writeFileSync(
    fakeCmake,
    `#!/bin/sh
set -eu
printf '<call>\\n' >>"$PIPETUNE_TEST_CMAKE_INVOCATIONS"
printf '%s\\n' "$@" >>"$PIPETUNE_TEST_CMAKE_INVOCATIONS"
if [ -n "$PIPETUNE_TEST_RECONSTRUCT_MANIFEST" ] && [ -n "\${DESTDIR:-}" ]; then
  executable="$DESTDIR$PIPETUNE_TEST_RECONSTRUCT_PREFIX/bin/pipetune"
  icon="$DESTDIR$PIPETUNE_TEST_RECONSTRUCT_PREFIX/share/PipeTune icons/pipetune.svg"
  mkdir -p "$(dirname "$executable")" "$(dirname "$icon")"
  printf 'staged pipetune\\n' >"$executable"
  printf 'staged icon\\n' >"$icon"
  printf '%s\\n%s\\n' \
    "$PIPETUNE_TEST_RECONSTRUCT_PREFIX/bin/pipetune" \
    "$PIPETUNE_TEST_RECONSTRUCT_PREFIX/share/PipeTune icons/pipetune.svg" \
    >"$PIPETUNE_TEST_RECONSTRUCT_MANIFEST"
fi
`,
  );
  chmodSync(fakeCmake, 0o755);

  const runMake = (target, makeVariables, environment) => {
    writeFileSync(invocationRecord, "");
    const result = spawnSync(
      makeExecutable,
      [
        target,
        `BUILD_DIR=${buildDirectory}`,
        `PREFIX=${prefix}`,
        "BUILD_TYPE=Release",
        ...makeVariables,
      ],
      {
        cwd: projectRoot,
        encoding: "utf8",
        env: {
          ...process.env,
          PATH: `${binDirectory}:${process.env.PATH}`,
          PIPETUNE_TEST_CMAKE_INVOCATIONS: invocationRecord,
          PIPETUNE_TEST_RECONSTRUCT_MANIFEST: "",
          PIPETUNE_TEST_RECONSTRUCT_PREFIX: "",
          ...environment,
        },
      },
    );
    assertSuccess(result, `make ${target} failed`);
    return readFileSync(invocationRecord, "utf8")
      .split("<call>\n")
      .filter((record) => record.length > 0)
      .map((record) => record.trimEnd().split("\n"));
  };

  const installInvocation = [
    "--install",
    buildDirectory,
    "--prefix",
    prefix,
    "--strip",
  ];
  assertEqual(
    JSON.stringify([installInvocation]),
    JSON.stringify(runMake("install", [], {})),
    "make install must only install an existing build",
  );
  assertEqual(
    JSON.stringify([
      [
        "-S",
        ".",
        "-B",
        buildDirectory,
        "-DCMAKE_BUILD_TYPE=Release",
        `-DCMAKE_INSTALL_PREFIX=${prefix}`,
        "-DBUILD_TESTING=OFF",
      ],
      ["--build", buildDirectory, "--parallel"],
      installInvocation,
    ]),
    JSON.stringify(runMake("build-install", [], {})),
    "make build-install must configure, build, and install",
  );

  const installedRoot = join(temporaryRoot, "installed");
  const installedExecutable = join(installedRoot, "bin", "pipetune");
  const installedIcon = join(
    installedRoot,
    "share",
    "PipeTune icons",
    "pipetune.svg",
  );
  const unrelatedFile = join(installedRoot, "bin", "keep");
  mkdirSync(join(installedRoot, "bin"), { recursive: true });
  mkdirSync(join(installedRoot, "share", "PipeTune icons"), {
    recursive: true,
  });
  writeFileSync(installedExecutable, "pipetune\n");
  writeFileSync(installedIcon, "icon\n");
  writeFileSync(unrelatedFile, "keep\n");
  mkdirSync(buildDirectory, { recursive: true });
  writeFileSync(
    join(buildDirectory, "install_manifest.txt"),
    `${installedExecutable}\n${installedIcon}\n`,
  );

  assertEqual(
    JSON.stringify([]),
    JSON.stringify(runMake("uninstall", [], {})),
    "make uninstall must not configure or build",
  );
  assertEqual(
    false,
    existsSync(installedExecutable),
    "make uninstall did not remove the installed executable",
  );
  assertEqual(
    false,
    existsSync(installedIcon),
    "make uninstall did not remove the installed path containing spaces",
  );
  assertEqual(
    true,
    existsSync(unrelatedFile),
    "make uninstall removed a file absent from the install manifest",
  );
  assertEqual(
    JSON.stringify([]),
    JSON.stringify(runMake("uninstall", [], {})),
    "make uninstall must be repeatable",
  );

  const fallbackRoot = join(temporaryRoot, "fallback-installed");
  const fallbackExecutable = join(
    fallbackRoot,
    prefix,
    "bin",
    "pipetune",
  );
  const fallbackIcon = join(
    fallbackRoot,
    prefix,
    "share",
    "PipeTune icons",
    "pipetune.svg",
  );
  const fallbackUnrelated = join(fallbackRoot, prefix, "bin", "keep");
  mkdirSync(join(fallbackRoot, prefix, "bin"), { recursive: true });
  mkdirSync(join(fallbackRoot, prefix, "share", "PipeTune icons"), {
    recursive: true,
  });
  writeFileSync(fallbackExecutable, "installed pipetune\n");
  writeFileSync(fallbackIcon, "installed icon\n");
  writeFileSync(fallbackUnrelated, "keep\n");
  rmSync(join(buildDirectory, "install_manifest.txt"), { force: true });

  assertEqual(
    JSON.stringify([installInvocation]),
    JSON.stringify(
      runMake(
        "uninstall",
        [`DESTDIR=${fallbackRoot}`],
        {
          PIPETUNE_TEST_RECONSTRUCT_MANIFEST: join(
            buildDirectory,
            "install_manifest.txt",
          ),
          PIPETUNE_TEST_RECONSTRUCT_PREFIX: prefix,
        },
      ),
    ),
    "make uninstall must reconstruct a missing install manifest",
  );
  assertEqual(
    false,
    existsSync(fallbackExecutable),
    "fallback uninstall did not remove the installed executable",
  );
  assertEqual(
    false,
    existsSync(fallbackIcon),
    "fallback uninstall did not remove the installed path containing spaces",
  );
  assertEqual(
    true,
    existsSync(fallbackUnrelated),
    "fallback uninstall removed a file absent from the reconstructed manifest",
  );
  assertEqual(
    JSON.stringify([]),
    JSON.stringify(runMake("uninstall", [`DESTDIR=${fallbackRoot}`], {})),
    "fallback uninstall must persist a repeatable manifest",
  );
} finally {
  rmSync(temporaryRoot, { recursive: true, force: true });
}
