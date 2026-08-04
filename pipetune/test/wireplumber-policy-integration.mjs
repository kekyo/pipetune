import assert from "node:assert/strict";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";

const [
  policy04Loader,
  policy04Script,
  policy05Config,
  policy05Script,
  setupPipeWireProbe,
] = process.argv.slice(2);

const commandExists = (command) =>
  spawnSync("sh", ["-c", `command -v ${command}`], { stdio: "ignore" })
    .status === 0;

if (!["pipewire", "wireplumber", "pw-metadata"].every(commandExists)) {
  console.log(
    "PipeWire or WirePlumber test tools are unavailable; skipping policy integration test",
  );
  process.exit(77);
}

const versionResult = spawnSync("wireplumber", ["--version"], {
  encoding: "utf8",
});
assert.equal(versionResult.status, 0, versionResult.stderr);
const versionMatch = `${versionResult.stdout}\n${versionResult.stderr}`.match(
  /(?:Compiled|Linked) with libwireplumber (\d+)\.(\d+)/,
);
assert.notEqual(
  versionMatch,
  null,
  "cannot determine the WirePlumber runtime version",
);
const majorVersion = Number(versionMatch[1]);
if (majorVersion !== 0 || !["4", "5"].includes(versionMatch[2])) {
  console.log(
    `WirePlumber ${versionMatch[1]}.${versionMatch[2]} is outside the supported integration matrix`,
  );
  process.exit(77);
}
const minorVersion = Number(versionMatch[2]);

const temporaryRoot = await fs.mkdtemp(
  path.join(os.tmpdir(), "pipetune-wireplumber-policy-"),
);
const runtimeDirectory = path.join(temporaryRoot, "runtime");
const configDirectory = path.join(temporaryRoot, "config");
const dataDirectory = path.join(temporaryRoot, "data");
await fs.mkdir(runtimeDirectory, { mode: 0o700 });

const policyDestination = (root, relativeDestination) =>
  path.join(root, "wireplumber", relativeDestination);

const copyPolicy = async (root, source, relativeDestination) => {
  const destination = policyDestination(root, relativeDestination);
  await fs.mkdir(path.dirname(destination), { recursive: true });
  await fs.copyFile(source, destination);
};

const writePolicy = async (root, relativeDestination, contents) => {
  const destination = policyDestination(root, relativeDestination);
  await fs.mkdir(path.dirname(destination), { recursive: true });
  await fs.writeFile(destination, contents);
};

if (minorVersion === 4) {
  await copyPolicy(
    configDirectory,
    policy04Loader,
    "main.lua.d/85-pipetune.lua",
  );
  await writePolicy(
    configDirectory,
    "policy.lua.d/85-pipetune.lua",
    `-- The obsolete loader path is empty after a PipeTune upgrade.
`,
  );
  await copyPolicy(
    configDirectory,
    policy04Script,
    "scripts/pipetune/policy-0.4.lua",
  );
  await copyPolicy(
    configDirectory,
    policy05Config,
    "wireplumber.conf.d/90-pipetune.conf",
  );
  await copyPolicy(
    configDirectory,
    policy05Script,
    "scripts/pipetune/policy-0.5.lua",
  );
  await writePolicy(
    configDirectory,
    "main.lua.d/86-pipetune-test-blocker.lua",
    `load_script("pipetune/test-main-blocker-0.4.lua")
`,
  );
  await writePolicy(
    configDirectory,
    "scripts/pipetune/test-main-blocker-0.4.lua",
    `Script.async_activation = true

local activation_finished = false
pipetune_test_main_blocker_metadata = ImplMetadata("pipetune-test-main-blocker")
pipetune_test_main_blocker_metadata:activate(Features.ALL, function(metadata, error)
  if error then
    Script:finish_activation_with_error(
        "failed to activate PipeTune main blocker metadata: " .. tostring(error))
    return
  end
  metadata:connect("changed", function(_, subject, key, _, value)
    if activation_finished or subject ~= 0 or key ~= "release" or
        tostring(value) ~= "true" then
      return
    end
    activation_finished = true
    metadata:set(0, "state", "Spa:String", "released")
    Script:finish_activation()
  end)
  metadata:set(0, "state", "Spa:String", "waiting")
end)
`,
  );
} else {
  await copyPolicy(
    configDirectory,
    policy05Config,
    "wireplumber.conf.d/90-pipetune.conf",
  );
  await copyPolicy(
    dataDirectory,
    policy05Script,
    "scripts/pipetune/policy-0.5.lua",
  );
  await writePolicy(
    configDirectory,
    "wireplumber.conf.d/91-pipetune-test-default.conf",
    `wireplumber.profiles = {
  policy = {
    pipetune.test-default = required
  }
}

wireplumber.components = [
  {
    name = pipetune/test-default-0.5.lua, type = script/lua
    provides = pipetune.test-default
    requires = [ pipetune.policy ]
  }
]
`,
  );
  await writePolicy(
    dataDirectory,
    "scripts/pipetune/test-default-0.5.lua",
    `Script.async_activation = true

pipetune_test_default_metadata = ImplMetadata("default")
pipetune_test_default_metadata:activate(Features.ALL, function(metadata, error)
  if error then
    Script:finish_activation_with_error(
        "failed to activate PipeTune test default metadata: " .. tostring(error))
    return
  end
  Script:finish_activation()
end)
`,
  );
}

const environment = {
  ...process.env,
  XDG_RUNTIME_DIR: runtimeDirectory,
  PIPEWIRE_RUNTIME_DIR: runtimeDirectory,
  XDG_CONFIG_HOME: configDirectory,
  XDG_DATA_HOME: dataDirectory,
  WIREPLUMBER_DEBUG: "2",
};
delete environment.PIPEWIRE_REMOTE;

const children = [];
const start = (command, commandArguments) => {
  const child = spawn(command, commandArguments, {
    env: environment,
    stdio: ["ignore", "ignore", "pipe"],
  });
  children.push(child);
  return child;
};

const waitFor = async (predicate, timeoutMilliseconds) => {
  const deadline = Date.now() + timeoutMilliseconds;
  while (Date.now() < deadline) {
    if (await predicate()) return true;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  return false;
};

const stopChildren = async () => {
  for (const child of children.toReversed()) {
    if (child.exitCode !== null || child.signalCode !== null) continue;
    child.kill("SIGTERM");
    await new Promise((resolve) => child.once("exit", resolve));
  }
};

try {
  const pipewire = start("pipewire", []);
  let pipewireDiagnostic = "";
  pipewire.stderr.on("data", (chunk) => {
    pipewireDiagnostic += chunk;
  });
  assert.equal(
    await waitFor(
      () =>
        fs.access(path.join(runtimeDirectory, "pipewire-0")).then(
          () => true,
          () => false,
        ),
      5000,
    ),
    true,
    `isolated PipeWire did not create its socket: ${pipewireDiagnostic}`,
  );

  const wireplumberArguments =
    minorVersion === 4 ? [] : ["--profile", "policy"];
  const wireplumber = start("wireplumber", wireplumberArguments);
  let wireplumberDiagnostic = "";
  wireplumber.stderr.on("data", (chunk) => {
    wireplumberDiagnostic += chunk;
  });

  let metadataOutput = "";
  const metadataReady = await waitFor(() => {
    const result = spawnSync("pw-metadata", ["-n", "pipetune-policy"], {
      env: environment,
      encoding: "utf8",
    });
    metadataOutput = `${result.stdout ?? ""}\n${result.stderr ?? ""}`;
    return result.status === 0 && metadataOutput.includes("protocol.version");
  }, 10000);
  assert.equal(
    metadataReady,
    true,
    `WirePlumber did not publish the PipeTune policy handshake: ${wireplumberDiagnostic}`,
  );
  assert.match(metadataOutput, /protocol\.version[^\n]*1/);
  assert.match(metadataOutput, new RegExp(`wireplumber-0\\.${minorVersion}`));
  if (minorVersion === 4) {
    let blockerMetadataOutput = "";
    const blockerWaiting = await waitFor(() => {
      const result = spawnSync(
        "pw-metadata",
        ["-n", "pipetune-test-main-blocker"],
        { env: environment, encoding: "utf8" },
      );
      blockerMetadataOutput = `${result.stdout ?? ""}\n${result.stderr ?? ""}`;
      return result.status === 0 && blockerMetadataOutput.includes("waiting");
    }, 5000);
    assert.equal(
      blockerWaiting,
      true,
      `WirePlumber main activation was not held for the ordering test: ${blockerMetadataOutput}\n${wireplumberDiagnostic}`,
    );
    const released = spawnSync(
      "pw-metadata",
      ["-n", "pipetune-test-main-blocker", "0", "release", "true", "Spa:Bool"],
      { env: environment, encoding: "utf8" },
    );
    assert.equal(
      released.status,
      0,
      `${released.stdout ?? ""}\n${released.stderr ?? ""}`,
    );
  }
  const policyMetadataList = spawnSync("pw-metadata", ["-l"], {
    env: environment,
    encoding: "utf8",
  });
  const policyMetadataListOutput = `${policyMetadataList.stdout ?? ""}\n${policyMetadataList.stderr ?? ""}`;
  assert.equal(policyMetadataList.status, 0, policyMetadataListOutput);
  assert.equal(
    policyMetadataListOutput.match(/Found "pipetune-policy"/gu)?.length ?? 0,
    1,
    `WirePlumber published duplicate PipeTune policy metadata:\n${policyMetadataListOutput}\n${wireplumberDiagnostic}`,
  );

  const configuredDefaultKey = "default.configured.audio.sink";
  const effectiveDefaultKey = "default.audio.sink";
  let defaultMetadataOutput = "";
  const defaultMetadataReady = await waitFor(() => {
    const result = spawnSync("pw-metadata", ["-n", "default"], {
      env: environment,
      encoding: "utf8",
    });
    defaultMetadataOutput = `${result.stdout ?? ""}\n${result.stderr ?? ""}`;
    return result.status === 0 && defaultMetadataOutput.includes('"default"');
  }, 5000);
  assert.equal(
    defaultMetadataReady,
    true,
    `default metadata was not observable: ${defaultMetadataOutput}\n${wireplumberDiagnostic}`,
  );

  const seedResult = spawnSync(
    "pw-metadata",
    [
      "-n",
      "default",
      "0",
      configuredDefaultKey,
      '{"name":"pipetune_sink"}',
      "Spa:String:JSON",
    ],
    { env: environment, encoding: "utf8" },
  );
  assert.equal(
    seedResult.status,
    0,
    `${seedResult.stdout ?? ""}\n${seedResult.stderr ?? ""}`,
  );
  const physicalDefaultSeed = spawnSync(
    "pw-metadata",
    [
      "-n",
      "default",
      "0",
      effectiveDefaultKey,
      '{"name":"physical_sink"}',
      "Spa:String:JSON",
    ],
    { env: environment, encoding: "utf8" },
  );
  assert.equal(
    physicalDefaultSeed.status,
    0,
    `${physicalDefaultSeed.stdout ?? ""}\n${physicalDefaultSeed.stderr ?? ""}`,
  );

  let seededMetadataOutput = "";
  const seededMetadataReady = await waitFor(() => {
    const result = spawnSync(
      "pw-metadata",
      ["-n", "default", "0", configuredDefaultKey],
      { env: environment, encoding: "utf8" },
    );
    seededMetadataOutput = `${result.stdout ?? ""}\n${result.stderr ?? ""}`;
    return (
      result.status === 0 && seededMetadataOutput.includes("pipetune_sink")
    );
  }, 5000);
  assert.equal(
    seededMetadataReady,
    true,
    `obsolete PipeTune default was not observable: ${seededMetadataOutput}\n${wireplumberDiagnostic}`,
  );

  const prepared = spawnSync(setupPipeWireProbe, [], {
    env: environment,
    encoding: "utf8",
  });
  assert.equal(
    prepared.status,
    0,
    `setup-time PipeWire preparation failed: ${prepared.stderr}`,
  );
  assert.match(
    prepared.stdout,
    new RegExp(`policyBackend=wireplumber-0\\.${minorVersion}`),
  );
  assert.match(prepared.stdout, /legacyDefaultCleared=true/);

  const migratedDefault = spawnSync(
    "pw-metadata",
    ["-n", "default", "0", configuredDefaultKey],
    { env: environment, encoding: "utf8" },
  );
  const migratedOutput = `${migratedDefault.stdout ?? ""}\n${migratedDefault.stderr ?? ""}`;
  assert.equal(migratedDefault.status, 0, migratedOutput);
  assert.doesNotMatch(migratedOutput, /pipetune_sink/);

  const preservedDefault = spawnSync(
    "pw-metadata",
    ["-n", "default", "0", effectiveDefaultKey],
    { env: environment, encoding: "utf8" },
  );
  const preservedOutput = `${preservedDefault.stdout ?? ""}\n${preservedDefault.stderr ?? ""}`;
  assert.equal(preservedDefault.status, 0, preservedOutput);
  assert.match(preservedOutput, /physical_sink/);
} finally {
  await stopChildren();
  await fs.rm(temporaryRoot, { recursive: true, force: true });
}
