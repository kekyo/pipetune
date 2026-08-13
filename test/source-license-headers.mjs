import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import process from "node:process";

const expectedHeader = `/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
`;

const sourceExtensions = new Set([".c", ".cpp", ".h"]);
const excludedDirectories = new Set(["node_modules"]);

const collectSourceFiles = async (directory) => {
  const entries = await readdir(directory, { withFileTypes: true });
  const nested = await Promise.all(
    entries.map(async (entry) => {
      const target = path.join(directory, entry.name);
      if (entry.isDirectory()) {
        return excludedDirectories.has(entry.name)
          ? []
          : collectSourceFiles(target);
      }
      return entry.isFile() && sourceExtensions.has(path.extname(entry.name))
        ? [target]
        : [];
    }),
  );
  return nested.flat();
};

const workspace = process.argv[2];
if (workspace === undefined) {
  throw new Error("workspace path is required");
}

const ownedRoots = ["pipetune", "pipetune-gtk"].map((directory) =>
  path.join(workspace, directory),
);
const sourceFiles = (await Promise.all(ownedRoots.map(collectSourceFiles)))
  .flat()
  .sort();
const missingHeaders = [];
for (const sourceFile of sourceFiles) {
  const contents = await readFile(sourceFile, "utf8");
  if (!contents.startsWith(expectedHeader)) {
    missingHeaders.push(path.relative(workspace, sourceFile));
  }
}

if (sourceFiles.length === 0) {
  throw new Error("no PipeTune-owned C/C++ source files were found");
}
if (missingHeaders.length !== 0) {
  process.stderr.write(
    `Missing PipeTune license header:\n${missingHeaders.join("\n")}\n`,
  );
  process.exitCode = 1;
}
