import {
  existsSync,
  lstatSync,
  mkdtempSync,
  readFileSync,
  renameSync,
  rmSync,
  unlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import {
  isAbsolute,
  join,
  parse,
  relative,
  resolve,
  sep,
} from "node:path";
import process from "node:process";
import { spawnSync } from "node:child_process";

const [buildDirectoryArgument, prefix, destinationRootArgument] =
  process.argv.slice(2);

const fail = (message) => {
  throw new Error(message);
};

const readManifest = (manifestPath) =>
  readFileSync(manifestPath, "utf8")
    .split("\n")
    .filter((installedPath) => installedPath.length !== 0);

const reconstructManifest = (
  buildDirectory,
  manifestPath,
  installPrefix,
  destinationRoot,
) => {
  if (!isAbsolute(installPrefix)) {
    fail(`Install prefix must be absolute: ${installPrefix}`);
  }
  if (destinationRoot.length !== 0 && !isAbsolute(destinationRoot)) {
    fail(`DESTDIR must be absolute: ${destinationRoot}`);
  }

  const stagingRoot = mkdtempSync(join(tmpdir(), "pipetune-uninstall-"));
  const temporaryManifest = `${manifestPath}.reconstructed-${process.pid}`;
  let completed = false;
  try {
    process.stderr.write(
      `Install manifest not found; reconstructing from ${buildDirectory}\n`,
    );
    const installed = spawnSync(
      "cmake",
      ["--install", buildDirectory, "--prefix", installPrefix, "--strip"],
      {
        env: { ...process.env, DESTDIR: stagingRoot },
        stdio: "inherit",
      },
    );
    if (installed.error !== undefined) {
      fail(`Cannot reconstruct install manifest: ${installed.error.message}`);
    }
    if (installed.status !== 0) {
      fail(
        `Cannot reconstruct install manifest: cmake exited with ${installed.status}`,
      );
    }
    if (!existsSync(manifestPath)) {
      fail(`CMake did not create an install manifest: ${manifestPath}`);
    }

    const stagedPaths = readManifest(manifestPath);
    if (stagedPaths.length === 0) {
      fail("CMake created an empty install manifest");
    }
    const targetRoot =
      destinationRoot.length === 0 ? undefined : resolve(destinationRoot);
    const installedPaths = stagedPaths.map((installedPath) => {
      // CMake copies below DESTDIR but records paths relative to the target root.
      if (!isAbsolute(installedPath)) {
        fail(`CMake manifest contains a relative path: ${installedPath}`);
      }
      const normalizedInstalledPath = resolve(installedPath);
      const relativePath = relative(
        parse(normalizedInstalledPath).root,
        normalizedInstalledPath,
      );
      if (
        relativePath.length === 0 ||
        relativePath === ".." ||
        relativePath.startsWith(`..${sep}`) ||
        isAbsolute(relativePath)
      ) {
        fail(`CMake manifest contains an invalid path: ${installedPath}`);
      }
      const stagedPath = join(stagingRoot, relativePath);
      try {
        lstatSync(stagedPath);
      } catch (error) {
        if (error?.code === "ENOENT") {
          fail(`CMake did not stage its manifest entry: ${installedPath}`);
        }
        throw error;
      }
      return targetRoot === undefined
        ? normalizedInstalledPath
        : join(targetRoot, relativePath);
    });
    writeFileSync(temporaryManifest, `${installedPaths.join("\n")}\n`);
    renameSync(temporaryManifest, manifestPath);
    completed = true;
  } finally {
    rmSync(stagingRoot, { recursive: true, force: true });
    rmSync(temporaryManifest, { force: true });
    if (!completed) {
      rmSync(manifestPath, { force: true });
    }
  }
};

const removeInstalledFiles = (manifestPath) => {
  for (const installedPath of readManifest(manifestPath)) {
    if (!isAbsolute(installedPath)) {
      fail(`Install manifest contains a relative path: ${installedPath}`);
    }
    process.stdout.write(`Removing ${installedPath}\n`);
    try {
      unlinkSync(installedPath);
    } catch (error) {
      if (error?.code !== "ENOENT") {
        throw error;
      }
    }
  }
};

try {
  if (
    buildDirectoryArgument === undefined ||
    prefix === undefined ||
    destinationRootArgument === undefined
  ) {
    fail("Usage: uninstall.mjs BUILD_DIR PREFIX DESTDIR");
  }
  const buildDirectory = resolve(buildDirectoryArgument);
  const manifestPath = join(buildDirectory, "install_manifest.txt");
  if (!existsSync(manifestPath)) {
    reconstructManifest(
      buildDirectory,
      manifestPath,
      prefix,
      destinationRootArgument,
    );
  }
  removeInstalledFiles(manifestPath);
} catch (error) {
  const message = error instanceof Error ? error.message : String(error);
  process.stderr.write(`pipetune uninstall: ${message}\n`);
  process.exitCode = 1;
}
