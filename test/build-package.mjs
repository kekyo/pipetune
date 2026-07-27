import {
  chmodSync,
  copyFileSync,
  cpSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { spawnSync } from "node:child_process";

const [projectRoot, dpkgDeb] = process.argv.slice(2);

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
    fail(`${message}\nexpected: ${expected}\nactual:   ${actual}`);
  }
};

const assertIncludes = (actual, expected, message) => {
  if (!actual.includes(expected)) {
    fail(`${message}\nmissing: ${expected}\nactual:\n${actual}`);
  }
};

const assertSuccess = (result, message) => {
  if (result.status !== 0) {
    fail(message, result);
  }
};

const run = (command, commandArguments, environment) =>
  spawnSync(command, commandArguments, {
    encoding: "utf8",
    env: environment,
  });

const runSourced = (body, extraArguments, environment) =>
  run(
    "sh",
    [
      "-c",
      `
PIPETUNE_PACKAGE_PROJECT_ROOT=$1
PIPETUNE_PACKAGE_SOURCE_ONLY=1
. "$1/build_package.sh"
${body}
`,
      "pipetune-package-test",
      projectRoot,
      ...extraArguments,
    ],
    environment,
  );

const writeExecutable = (path, contents) => {
  writeFileSync(path, contents);
  chmodSync(path, 0o755);
};

const createPackageStage = (
  stageRoot,
  includeAutostart,
  debianArchitecture,
) => {
  const paths = [
    "DEBIAN",
    "usr/bin",
    "usr/lib/systemd/user",
    "usr/share/applications",
    "usr/share/icons/hicolor/scalable/apps",
    "usr/share/doc/pipetune",
  ];
  if (includeAutostart) {
    paths.push("etc/xdg/autostart");
  }
  for (const path of paths) {
    mkdirSync(join(stageRoot, path), { recursive: true });
  }

  writeFileSync(
    join(stageRoot, "DEBIAN/control"),
    `Package: pipetune
Version: 1.2.3
Section: sound
Priority: optional
Architecture: ${debianArchitecture}
Maintainer: PipeTune packager <packager@localhost>
Depends: libc6, systemd, dbus-user-session, pipewire, wireplumber, hicolor-icon-theme
Description: PipeWire system-wide DSP and GTK control application
`,
  );
  copyFileSync("/bin/true", join(stageRoot, "usr/bin/pipetune"));
  copyFileSync("/bin/true", join(stageRoot, "usr/bin/pipetune-gtk"));
  writeFileSync(
    join(stageRoot, "usr/lib/systemd/user/pipetune.service"),
    "[Service]\nExecStart=/usr/bin/pipetune\n",
  );
  writeFileSync(
    join(stageRoot, "usr/share/applications/net.kekyo.pipetune-gtk.desktop"),
    "[Desktop Entry]\nType=Application\nExec=pipetune-gtk\n",
  );
  if (includeAutostart) {
    cpSync(
      join(stageRoot, "usr/share/applications/net.kekyo.pipetune-gtk.desktop"),
      join(stageRoot, "etc/xdg/autostart/net.kekyo.pipetune-gtk.desktop"),
    );
  }
  writeFileSync(
    join(stageRoot, "usr/share/icons/hicolor/scalable/apps/pipetune.svg"),
    "<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n",
  );
  for (const documentName of [
    "README.md",
    "README.daemon.md",
    "README.gtk.md",
    "architecture.md",
    "copyright",
    "environment.example",
  ]) {
    writeFileSync(
      join(stageRoot, "usr/share/doc/pipetune", documentName),
      `${documentName}\n`,
    );
  }
};

if (!projectRoot || !dpkgDeb) {
  fail("package build test arguments are incomplete");
}
if (existsSync(join(projectRoot, "VERSION"))) {
  fail("legacy VERSION file is still present");
}

const temporaryRoot = mkdtempSync(join(tmpdir(), "pipetune-package-test-"));

try {
  const hostArchitectureResult = run(
    join(dirname(dpkgDeb), "dpkg"),
    ["--print-architecture"],
    process.env,
  );
  assertSuccess(
    hostArchitectureResult,
    "could not determine the host Debian architecture",
  );
  const hostDebianArchitecture = hostArchitectureResult.stdout.trim();
  const canonicalHostArchitectures = new Map([
    ["amd64", "x86_64"],
    ["i386", "i686"],
    ["arm64", "arm64"],
    ["armhf", "armv7l"],
    ["riscv64", "riscv64"],
  ]);
  const canonicalHostArchitecture = canonicalHostArchitectures.get(
    hostDebianArchitecture,
  );
  if (!canonicalHostArchitecture) {
    fail(`unsupported test host architecture: ${hostDebianArchitecture}`);
  }

  const versionBinDirectory = join(temporaryRoot, "version-bin");
  const versionInvocation = join(temporaryRoot, "version-invocation.txt");
  mkdirSync(versionBinDirectory);
  writeExecutable(
    join(versionBinDirectory, "npx"),
    `#!/bin/sh
set -eu
[ "\${PIPETUNE_TEST_FORBID_VERSION_LOOKUP:-0}" = 0 ] || exit 99
printf '%s\\n' "$@" >"$PIPETUNE_TEST_VERSION_INVOCATION"
printf '7.6.5-test\\n'
`,
  );
  const printVersion = run(
    join(projectRoot, "build_package.sh"),
    ["--print-version"],
    {
      ...process.env,
      PATH: `${versionBinDirectory}:${process.env.PATH}`,
      PIPETUNE_TEST_VERSION_INVOCATION: versionInvocation,
    },
  );
  assertSuccess(printVersion, "build_package.sh --print-version failed");
  assertEqual(
    "7.6.5-test\n",
    printVersion.stdout,
    "screw-up version was not used",
  );
  assertEqual(
    ["screw-up", "format", "-e", "{version}", "-f", ""].join("\n"),
    readFileSync(versionInvocation, "utf8"),
    "screw-up was not invoked with the documented arguments",
  );

  const overriddenVersion = run(
    join(projectRoot, "build_package.sh"),
    ["--version", "2.4.6-custom", "--print-version"],
    {
      ...process.env,
      PATH: `${versionBinDirectory}:${process.env.PATH}`,
      PIPETUNE_TEST_FORBID_VERSION_LOOKUP: "1",
      PIPETUNE_TEST_VERSION_INVOCATION: versionInvocation,
    },
  );
  assertSuccess(overriddenVersion, "explicit package version failed");
  assertEqual(
    "2.4.6-custom\n",
    overriddenVersion.stdout,
    "explicit package version was not preserved",
  );

  const mappings = runSourced(
    `
DISTRO_FILTER=''
RELEASE_FILTER=''
ARCH_FILTER=''
VERSION=1.2.3
canonical_arch amd64
canonical_arch i386
canonical_arch armhf
canonical_release noble
canonical_release resolute
count_deb_builds
deb_artifact_path pipetune ubuntu 24.04 x86_64
prereq_image_for_target debian bookworm x86_64
container_image_for_target debian trixie riscv64
`,
    [],
    process.env,
  );
  assertSuccess(mappings, "package target mappings failed");
  assertEqual(
    [
      "x86_64",
      "i686",
      "armv7l",
      "24.04",
      "26.04",
      "13",
      join(
        projectRoot,
        "artifacts/deb/pipetune-1.2.3-ubuntu-24.04-amd64.deb",
      ),
      "localhost/pipetune-pack-deb-debian-bookworm-x86_64:latest",
      "docker.io/library/debian:trixie",
      "",
    ].join("\n"),
    mappings.stdout,
    "package target mappings differ from the supported matrix",
  );

  const missingEngine = join(temporaryRoot, "missing-engine");
  writeExecutable(
    missingEngine,
    `#!/bin/sh
if [ "\${1:-}" = image ] && [ "\${2:-}" = exists ]; then
  exit 1
fi
exit 2
`,
  );
  const missingImage = runSourced(
    `
CONTAINER_ENGINE_BIN=$2
assert_prereq_image localhost/missing:latest
`,
    [missingEngine],
    process.env,
  );
  if (missingImage.status === 0) {
    fail("missing prerequisite image was accepted");
  }
  assertIncludes(
    missingImage.stderr,
    "Missing prerequisite image: localhost/missing:latest. Run ./prereq.sh first.",
    "missing prerequisite image guidance was not reported",
  );

  const fakeProject = join(temporaryRoot, "project");
  const binDirectory = join(temporaryRoot, "bin");
  const containerRecords = join(temporaryRoot, "container-records.txt");
  const dpkgRecords = join(temporaryRoot, "dpkg-records.txt");
  mkdirSync(fakeProject);
  mkdirSync(binDirectory);

  const containerEngine = join(binDirectory, "container-engine");
  writeExecutable(
    containerEngine,
    `#!/bin/sh
set -eu
if [ "\${1:-}" = image ] && [ "\${2:-}" = exists ]; then
  printf 'exists %s\\n' "\${3:-}" >>"$PIPETUNE_TEST_CONTAINER_RECORDS"
  exit 0
fi
if [ "\${1:-}" != run ]; then
  printf 'Unexpected container command: %s\\n' "$*" >&2
  exit 2
fi
printf 'run %s\\n' "$*" >>"$PIPETUNE_TEST_CONTAINER_RECORDS"
workspace=''
previous=''
for argument in "$@"; do
  if [ "$previous" = -v ]; then
    workspace=\${argument%%:/workspace*}
  fi
  previous=$argument
done
case " $* " in
  *" --validate-package "*)
    exit 0
    ;;
esac
[ -n "$workspace" ] || exit 3
stage="$workspace/artifacts/.tmp/test-run/deb/debian/bookworm/x86_64/work/stage/pipetune"
mkdir -p "$stage/DEBIAN"
printf 'Package: pipetune\\n' >"$stage/DEBIAN/control"
`,
  );
  writeExecutable(
    join(binDirectory, "dpkg-deb"),
    `#!/bin/sh
set -eu
output_path=''
for argument in "$@"; do
  output_path=$argument
done
mkdir -p "$(dirname "$output_path")"
printf 'stub-deb\\n' >"$output_path"
printf '%s\\n' "$*" >>"$PIPETUNE_TEST_DPKG_RECORDS"
`,
  );

  const buildPackage = runSourced(
    `
PROJECT_ROOT=$2
ARTIFACT_ROOT=$PROJECT_ROOT/artifacts
DEB_ARTIFACT_ROOT=$ARTIFACT_ROOT/deb
RUN_ID=test-run
TMP_ROOT=$ARTIFACT_ROOT/.tmp/$RUN_ID
VERSION=1.2.3-test
CONTAINER_ENGINE_BIN=$3
MAKE_JOBS=1
BUILD_TYPE=Release
build_deb_package debian bookworm x86_64 linux/amd64
`,
    [fakeProject, containerEngine],
    {
      ...process.env,
      PATH: `${binDirectory}:${process.env.PATH}`,
      PIPETUNE_TEST_CONTAINER_RECORDS: containerRecords,
      PIPETUNE_TEST_DPKG_RECORDS: dpkgRecords,
    },
  );
  assertSuccess(buildPackage, "deb package orchestration failed");
  const recordedContainers = readFileSync(containerRecords, "utf8");
  assertIncludes(
    recordedContainers,
    "exists localhost/pipetune-pack-deb-debian-bookworm-x86_64:latest",
    "prerequisite image was not checked",
  );
  const containerRuns = recordedContainers
    .split("\n")
    .filter((line) => line.startsWith("run "));
  assertEqual(
    "2",
    `${containerRuns.length}`,
    "build and installation validation must use separate container runs",
  );
  assertIncludes(
    recordedContainers,
    "./scripts/build_linux_dist_container.sh",
    "container package builder was not invoked",
  );
  assertIncludes(
    recordedContainers,
    "--validate-package /workspace/artifacts/deb/pipetune-1.2.3-test-debian-bookworm-amd64.deb",
    "built package was not installation-tested",
  );
  assertIncludes(
    readFileSync(dpkgRecords, "utf8"),
    join(
      fakeProject,
      "artifacts/deb/pipetune-1.2.3-test-debian-bookworm-amd64.deb",
    ),
    "dpkg-deb did not write the expected artifact",
  );

  const prereqProject = join(temporaryRoot, "prereq-project");
  const prereqRecords = join(temporaryRoot, "prereq-records.txt");
  mkdirSync(prereqProject);
  symlinkSync(
    join(projectRoot, "build_package.sh"),
    join(prereqProject, "build_package.sh"),
  );
  const prereqEngine = join(binDirectory, "prereq-engine");
  writeExecutable(
    prereqEngine,
    `#!/bin/sh
set -eu
if [ "\${1:-}" != build ]; then
  printf 'Unexpected container command: %s\\n' "$*" >&2
  exit 2
fi
containerfile=''
previous=''
for argument in "$@"; do
  if [ "$previous" = -f ]; then
    containerfile=$argument
  fi
  previous=$argument
done
printf '%s\\n' "$*" >"$PIPETUNE_TEST_PREREQ_RECORDS"
cp "$containerfile" "$PIPETUNE_TEST_PREREQ_RECORDS.containerfile"
`,
  );
  const prereq = run(
    join(projectRoot, "prereq.sh"),
    [
      "--distro",
      "debian",
      "--release",
      "bookworm",
      "--arch",
      "amd64",
      "--jobs",
      "1",
      "--force",
    ],
    {
      ...process.env,
      CONTAINER_ENGINE: prereqEngine,
      PIPETUNE_PREREQ_PROJECT_ROOT: prereqProject,
      PIPETUNE_TEST_PREREQ_RECORDS: prereqRecords,
    },
  );
  assertSuccess(prereq, "filtered prerequisite image build failed");
  const prereqInvocation = readFileSync(prereqRecords, "utf8");
  assertIncludes(
    prereqInvocation,
    "build --no-cache --platform linux/amd64",
    "forced prerequisite build did not disable the cache",
  );
  assertIncludes(
    prereqInvocation,
    "BASE_IMAGE=docker.io/amd64/debian:bookworm",
    "prerequisite build used the wrong base image",
  );
  assertIncludes(
    prereqInvocation,
    "localhost/pipetune-pack-deb-debian-bookworm-x86_64:latest",
    "prerequisite build used the wrong image tag",
  );
  const containerfile = readFileSync(
    `${prereqRecords}.containerfile`,
    "utf8",
  );
  for (const dependency of [
    "libgtk-3-dev",
    "libpipewire-0.3-dev",
    "nodejs",
    "wireplumber",
  ]) {
    assertIncludes(
      containerfile,
      dependency,
      `prerequisite image omitted ${dependency}`,
    );
  }
  assertIncludes(
    prereq.stdout,
    "Prerequisite images are ready.",
    "prerequisite completion was not reported",
  );

  const wrapperCapture = join(temporaryRoot, "wrapper-arguments.txt");
  const wrapperStub = join(binDirectory, "build-package-wrapper-stub");
  writeExecutable(
    wrapperStub,
    `#!/bin/sh
set -eu
printf '%s\\n' "$@" >"$PIPETUNE_TEST_WRAPPER_CAPTURE"
`,
  );
  const wrapper = run(
    join(projectRoot, "build_package_all.sh"),
    ["--jobs", "3", "--arch", "amd64", "--refresh-base"],
    {
      ...process.env,
      BUILD_PACKAGE_SCRIPT: wrapperStub,
      PIPETUNE_TEST_WRAPPER_CAPTURE: wrapperCapture,
    },
  );
  assertSuccess(wrapper, "all-target wrapper failed");
  assertEqual(
    ["--target", "all", "--jobs", "3", "--arch", "amd64", ""].join(
      "\n",
    ),
    readFileSync(wrapperCapture, "utf8"),
    "all-target wrapper did not preserve arguments",
  );
  assertIncludes(
    wrapper.stderr,
    "Run ./prereq.sh --force to rebuild prerequisite images.",
    "all-target wrapper did not explain --refresh-base",
  );

  const goodStage = join(temporaryRoot, "good-stage");
  const badStage = join(temporaryRoot, "bad-stage");
  const goodPackage = join(temporaryRoot, "pipetune-good.deb");
  const badPackage = join(temporaryRoot, "pipetune-bad.deb");
  createPackageStage(goodStage, true, hostDebianArchitecture);
  createPackageStage(badStage, false, hostDebianArchitecture);
  for (const [stage, output] of [
    [goodStage, goodPackage],
    [badStage, badPackage],
  ]) {
    const built = run(
      dpkgDeb,
      ["--root-owner-group", "--build", stage, output],
      process.env,
    );
    assertSuccess(built, `could not create test package ${output}`);
  }

  const validateGoodPackage = runSourced(
    `
VERSION=1.2.3
validate_deb_package "$2" "$3"
`,
    [goodPackage, canonicalHostArchitecture],
    process.env,
  );
  assertSuccess(
    validateGoodPackage,
    "complete deb package did not pass validation",
  );
  const validateBadPackage = runSourced(
    `
VERSION=1.2.3
validate_deb_package "$2" "$3"
`,
    [badPackage, canonicalHostArchitecture],
    process.env,
  );
  if (validateBadPackage.status === 0) {
    fail("deb package without XDG autostart passed validation");
  }
  assertIncludes(
    validateBadPackage.stderr,
    "net.kekyo.pipetune-gtk.desktop",
    "deb validation did not identify the missing autostart entry",
  );
} finally {
  rmSync(temporaryRoot, { recursive: true, force: true });
}
