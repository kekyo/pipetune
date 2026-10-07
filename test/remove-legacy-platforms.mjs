import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { join } from "node:path";

const root = process.argv[2];
const checks = [
  ["bookworm release selection", spawnSync("sh", ["-c", `
PIPETUNE_PACKAGE_SOURCE_ONLY=1
PIPETUNE_PACKAGE_PROJECT_ROOT=$1
. "$1/build_package.sh"
canonical_release bookworm
`, "legacy-platform-test", root], { encoding: "utf8" })],
  ["pre-0.3.73 headers", spawnSync("c++", [
    "-std=c++20", "-fsyntax-only",
    `-I${join(root, "pipetune/test/fixtures/pipewire-pre-0.3.73")}`,
    `-I${join(root, "pipetune/src")}`,
    join(root, "pipetune/test/pipewire_stream_flags_test.cpp"),
  ], { encoding: "utf8" })],
];
for (const [name, result] of checks) {
  assert.equal(result.error, undefined);
  console.log(`${name}: ${result.status === 0 ? "still supported" : "rejected"}`);
}
assert.ok(checks.every(([, result]) => result.status !== 0),
  "legacy distribution and stream flag fallback must be removed");
