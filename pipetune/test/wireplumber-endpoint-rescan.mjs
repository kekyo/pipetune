import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawn, spawnSync } from "node:child_process";

const [pipewire, wireplumber, pwCat, pwDump, policyFixture] =
  process.argv.slice(2);

const fail = (message) => {
  throw new Error(message);
};

if (!pipewire || !wireplumber || !pwCat || !pwDump || !policyFixture) {
  fail("WirePlumber endpoint rescan test arguments are incomplete");
}

const version = spawnSync(wireplumber, ["--version"], {
  encoding: "utf8",
});
if (version.status !== 0) {
  fail(`cannot inspect WirePlumber version: ${version.stderr}`);
}
if (!/libwireplumber 0\.4\./u.test(version.stdout)) {
  process.stdout.write("WirePlumber 0.4 is not installed; skipping test\n");
  process.exit(77);
}

const directory = mkdtempSync(join(tmpdir(), "pipetune-wp-rescan-test-"));
const runtimeDirectory = join(directory, "runtime");
const configDirectory = join(directory, "config");
const stateDirectory = join(directory, "state");
const dataDirectory = join(directory, "data");
const homeDirectory = join(directory, "home");
const remote = `pipetune-rescan-${process.pid}`;
const audioPath = join(directory, "silence.wav");
const initialPlayerName = "pipetune_endpoint_rescan_player";
const retryPlayerName = "pipetune_endpoint_rescan_retry_player";
for (const path of [
  runtimeDirectory,
  configDirectory,
  stateDirectory,
  dataDirectory,
  homeDirectory,
]) {
  mkdirSync(path, { recursive: true });
}

const sampleRate = 48000;
const channelCount = 2;
const bytesPerSample = 2;
const dataSize = sampleRate * channelCount * bytesPerSample * 20;
const wave = Buffer.alloc(44 + dataSize);
wave.write("RIFF", 0, "ascii");
wave.writeUInt32LE(36 + dataSize, 4);
wave.write("WAVE", 8, "ascii");
wave.write("fmt ", 12, "ascii");
wave.writeUInt32LE(16, 16);
wave.writeUInt16LE(1, 20);
wave.writeUInt16LE(channelCount, 22);
wave.writeUInt32LE(sampleRate, 24);
wave.writeUInt32LE(sampleRate * channelCount * bytesPerSample, 28);
wave.writeUInt16LE(channelCount * bytesPerSample, 32);
wave.writeUInt16LE(bytesPerSample * 8, 34);
wave.write("data", 36, "ascii");
wave.writeUInt32LE(dataSize, 40);
writeFileSync(audioPath, wave);

const environment = {
  ...process.env,
  HOME: homeDirectory,
  XDG_RUNTIME_DIR: runtimeDirectory,
  PIPEWIRE_RUNTIME_DIR: runtimeDirectory,
  PIPEWIRE_CORE: remote,
  PIPEWIRE_REMOTE: remote,
  XDG_CONFIG_HOME: configDirectory,
  XDG_STATE_HOME: stateDirectory,
  XDG_DATA_HOME: dataDirectory,
  WIREPLUMBER_DEBUG: "3",
};

const mainConfigurationDirectory = join(
  configDirectory,
  "wireplumber",
  "main.lua.d",
);
const bluetoothConfigurationDirectory = join(
  configDirectory,
  "wireplumber",
  "bluetooth.lua.d",
);
const policyConfigurationDirectory = join(
  configDirectory,
  "wireplumber",
  "policy.lua.d",
);
const scriptDirectory = join(configDirectory, "wireplumber", "scripts");
for (const path of [
  mainConfigurationDirectory,
  bluetoothConfigurationDirectory,
  policyConfigurationDirectory,
  scriptDirectory,
]) {
  mkdirSync(path, { recursive: true });
}

writeFileSync(
  join(mainConfigurationDirectory, "60-pipetune-test-disable-monitors.lua"),
  `alsa_monitor.enabled = false
v4l2_monitor.enabled = false
libcamera_monitor.enabled = false
`,
);
writeFileSync(
  join(
    bluetoothConfigurationDirectory,
    "60-pipetune-test-disable-monitors.lua",
  ),
  `bluez_monitor.enabled = false
bluez_midi_monitor.enabled = false
`,
);
writeFileSync(
  join(policyConfigurationDirectory, "60-pipetune-test-policy.lua"),
  `default_policy.policy.roles = {
  ["PipeTune-Test"] = {
    ["alias"] = { "Default", "Music", "Test" },
    ["priority"] = 25,
    ["action.default"] = "mix",
  },
}
default_policy.endpoints = {}

local pipetune_test_enable_default_policy = default_policy.enable
default_policy.enable = function()
  pipetune_test_enable_default_policy()

  if components["policy-endpoint-client.lua"] then
    components["policy-endpoint-client.lua"] = nil
    load_script("pipetune-endpoint-client-test.lua", default_policy.policy)
  end
  if components["policy-node.lua"] then
    components["policy-node.lua"] = nil
  end
end
`,
);

const fixture = spawnSync(policyFixture, [], { encoding: "utf8" });
if (fixture.status !== 0) {
  fail(`cannot render the PipeTune endpoint policy: ${fixture.stderr}`);
}
writeFileSync(
  join(scriptDirectory, "pipetune-endpoint-client-test.lua"),
  `${fixture.stdout}
-- Register an endpoint only after the policy has observed the pre-existing
-- player. The Core.sync barrier makes the registration order deterministic.
local pipetune_test_endpoint = nil
local pipetune_test_endpoint_pending = false

linkables_om:connect("objects-changed", function (om)
  if pipetune_test_endpoint or pipetune_test_endpoint_pending then
    return
  end

  for item in om:iterate() do
    local node = item:get_associated_proxy("node")
    if node and node.properties and
        node.properties["node.name"] == "pipetune_endpoint_rescan_player" then
      pipetune_test_endpoint_pending = true
      Core.sync(function (error)
        if error then
          Log.warning("test synchronization failed: " .. tostring(error))
          return
        end

        local endpoint = SessionItem("si-audio-endpoint")
        if not endpoint or not endpoint:configure {
          ["name"] = "endpoint.pipetune.test",
          ["media.class"] = "Audio/Sink",
          ["role"] = "PipeTune-Test",
          ["priority"] = 1000,
        } then
          Log.warning("failed to configure test endpoint")
          return
        end

        endpoint:activate(Features.ALL, function (item, activation_error)
          if activation_error then
            Log.warning(item, "failed to activate test endpoint: " ..
                tostring(activation_error))
            return
          end
          pipetune_test_endpoint = item
          item:register()
          Log.info(item, "registered delayed PipeTune test endpoint")
        end)
      end)
      return
    end
  end
end)
`,
);

const children = [];
const startProcess = (label, executable, commandArguments) => {
  const child = spawn(executable, commandArguments, {
    env: environment,
    stdio: ["ignore", "pipe", "pipe"],
  });
  const record = { child, label, output: "", exited: false };
  children.push(record);
  const collect = (chunk) => {
    record.output += chunk.toString();
  };
  child.stdout.on("data", collect);
  child.stderr.on("data", collect);
  child.once("exit", () => {
    record.exited = true;
  });
  return record;
};

const delay = (milliseconds) =>
  new Promise((resolve) => {
    setTimeout(resolve, milliseconds);
  });

const waitFor = async (description, predicate, timeoutMilliseconds = 8000) => {
  const deadline = Date.now() + timeoutMilliseconds;
  let lastError;
  while (Date.now() < deadline) {
    try {
      const result = predicate();
      if (result) {
        return result;
      }
    } catch (error) {
      lastError = error;
    }
    await delay(25);
  }
  const detail = lastError ? `: ${lastError.message}` : "";
  fail(`timed out waiting for ${description}${detail}`);
};

const inspectGraph = () => {
  const result = spawnSync(pwDump, [], {
    encoding: "utf8",
    env: environment,
    timeout: 2000,
  });
  if (result.status !== 0) {
    fail(`pw-dump failed: ${result.stderr}`);
  }
  return JSON.parse(result.stdout);
};

const findNode = (graph, name) =>
  graph.find(
    (object) =>
      object.type === "PipeWire:Interface:Node" &&
      object.info?.props?.["node.name"] === name,
  );

const graphHasTestLink = (graph, playerName) => {
  const player = findNode(graph, playerName);
  const endpoint = findNode(graph, "control.endpoint.pipetune.test");
  if (!player || !endpoint) {
    return false;
  }
  return graph.some(
    (object) =>
      object.type === "PipeWire:Interface:Link" &&
      String(object.info?.props?.["link.output.node"]) === String(player.id) &&
      String(object.info?.props?.["link.input.node"]) === String(endpoint.id),
  );
};

const stopChildren = async () => {
  for (const record of children.toReversed()) {
    if (!record.exited) {
      record.child.kill("SIGTERM");
    }
  }
  await Promise.race([
    Promise.all(
      children.map(
        (record) =>
          new Promise((resolve) => {
            if (record.exited) {
              resolve();
            } else {
              record.child.once("exit", resolve);
            }
          }),
      ),
    ),
    delay(2000),
  ]);
  for (const record of children) {
    if (!record.exited) {
      record.child.kill("SIGKILL");
    }
  }
};

let failure;
try {
  const pipewireProcess = startProcess("pipewire", pipewire, []);
  await waitFor("the isolated PipeWire socket", () => {
    if (pipewireProcess.exited) {
      fail("the isolated PipeWire process exited before becoming ready");
    }
    return existsSync(join(runtimeDirectory, remote));
  });

  const playerProcess = startProcess("pw-cat", pwCat, [
    "--playback",
    "--rate=48000",
    "--channels=2",
    "--format=s16",
    "--properties",
    `{ node.name = ${initialPlayerName} media.role = Test }`,
    audioPath,
  ]);
  await waitFor("the pre-existing playback stream", () => {
    if (playerProcess.exited) {
      fail("the playback stream exited before being registered");
    }
    return findNode(inspectGraph(), initialPlayerName);
  });

  const wireplumberProcess = startProcess("wireplumber", wireplumber, []);
  await waitFor("the delayed test endpoint", () => {
    if (wireplumberProcess.exited) {
      fail("WirePlumber exited before registering the test endpoint");
    }
    return findNode(inspectGraph(), "control.endpoint.pipetune.test");
  });
  await waitFor(
    "the pre-existing stream to be linked after endpoint registration",
    () => graphHasTestLink(inspectGraph(), initialPlayerName),
  );

  playerProcess.child.kill("SIGTERM");
  await waitFor("the first playback stream to be removed", () => {
    return !findNode(inspectGraph(), initialPlayerName);
  });

  const retryPlayerProcess = startProcess("pw-cat retry", pwCat, [
    "--playback",
    "--rate=48000",
    "--channels=2",
    "--format=s16",
    "--properties",
    `{ node.name = ${retryPlayerName} media.role = Test }`,
    audioPath,
  ]);
  await waitFor("the retry playback stream", () => {
    if (retryPlayerProcess.exited) {
      fail("the retry playback stream exited before being registered");
    }
    return findNode(inspectGraph(), retryPlayerName);
  });
  await waitFor(
    "the retry stream to be linked after the first stream was removed",
    () => graphHasTestLink(inspectGraph(), retryPlayerName),
  );
} catch (error) {
  failure = error;
} finally {
  await stopChildren();
  if (failure) {
    for (const record of children) {
      process.stderr.write(`\n[${record.label}]\n${record.output}`);
    }
  }
  rmSync(directory, { recursive: true, force: true });
}

if (failure) {
  process.stderr.write(`\n${failure.stack ?? failure.message}\n`);
  process.exitCode = 1;
}
