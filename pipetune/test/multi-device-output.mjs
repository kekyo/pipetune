import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, renameSync, rmSync, statSync, watch, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { createConnection } from "node:net";
import { once } from "node:events";
import { createInterface } from "node:readline";

const [pipewire, wireplumber, driver, requestedScenario = "channels", policyFixture,
  pulseServer, pulseDriver, productDaemon, presentationDirectory, systemModuleDirectory] = process.argv.slice(2);
const regularRoutePolicy = requestedScenario.endsWith("-route-policy");
const scenario = regularRoutePolicy ? requestedScenario.slice(0, -"-route-policy".length) : requestedScenario;
assert.ok(pipewire && wireplumber && driver, "PipeWire, WirePlumber and probe paths are required");
const version = spawnSync(wireplumber, ["--version"], { encoding: "utf8" });
assert.equal(version.status, 0, version.stderr);
const wirePlumber04 = /libwireplumber 0\.4\./u.test(version.stdout);
const fixedPcmTest = scenario.startsWith("product-sfz-") || scenario.startsWith("product-matrix-freewheel") || scenario === "product-matrix-realtime";
const onePcmOutput = scenario === "product-matrix-freewheel-one-output";
const audioFailurePoint = scenario.startsWith("product-matrix-freewheel-fail-")
  ? scenario.slice("product-matrix-freewheel-fail-".length) : "";
const expectedAudioFailure = {
  "before-source": "injected failure before source start", pcm: "injected PCM verification failure",
  disconnect: "injected device disconnect", timeout: "product PCM test timed out",
  cancel: "audio fixture canceled", "runtime-exit": "producer stopped before PCM verification",
}[audioFailurePoint];
const graphClockTest = ["product-output-clock", "product-output-clock-alt"].includes(scenario);
const graphRate = scenario === "product-output-clock-alt" ? 44100 : 48000;
const graphQuantum = scenario === "product-output-clock-alt" ? 512 : 256;

const directory = mkdtempSync(join(tmpdir(), "pipetune-multi-output-"));
const runtime = join(directory, "runtime");
const config = join(directory, "config");
const state = join(directory, "state");
const data = join(directory, "data");
for (const path of [runtime, config, state, data]) mkdirSync(path, { recursive: true, mode: 0o700 });
const remote = `pipetune-multi-${process.pid}`;
const environment = {
  ...process.env,
  XDG_RUNTIME_DIR: runtime,
  PIPEWIRE_RUNTIME_DIR: runtime,
  PIPEWIRE_CORE: remote,
  PIPEWIRE_REMOTE: remote,
  XDG_CONFIG_HOME: config,
  XDG_STATE_HOME: state,
  XDG_DATA_HOME: data,
  // The isolated audio session does not need remote filesystems or FUSE mounts.
  GIO_USE_VFS: "local",
  PULSE_SERVER: `unix:${join(runtime, "pulse-native")}`,
  PIPETUNE_PRODUCT_SOCKET: join(runtime, "product-control.sock"),
  ...(wirePlumber04 && scenario.startsWith("product-daemon-volume") ? { WIREPLUMBER_DEBUG: "3" } : {}),
  ...(regularRoutePolicy ? { WIREPLUMBER_DEBUG: "4" } : {}),
};
const writeConfiguration = (relative, contents) => {
  const components = relative.split("/");
  components.pop();
  mkdirSync(join(config, ...components), { recursive: true });
  writeFileSync(join(config, relative), contents);
};
if (scenario === "product-output-clock-unavailable") {
  const serverConfiguration = readFileSync("/usr/share/pipewire/pipewire.conf", "utf8");
  // Keep newer versions' conditional module options intact. A missing optional
  // test module suppresses the Profiler on both supported configuration formats.
  const withoutProfiler = serverConfiguration.replace(/name\s*=\s*libpipewire-module-profiler\b/u,
    "name = libpipewire-module-pipetune-test-unavailable flags = [ ifexists nofail ]");
  assert.ok(serverConfiguration !== withoutProfiler, "the fixture must suppress the server Profiler module");
  writeConfiguration("pipewire/pipewire.conf", withoutProfiler);
}
writeConfiguration("pipewire/pipewire.conf.d/99-pipetune-test.conf", `context.properties = {
  core.name = "${remote}"
  default.clock.rate = ${graphRate}
  default.clock.quantum = ${graphQuantum}
  default.clock.min-quantum = ${graphQuantum}
  default.clock.max-quantum = ${graphQuantum}
}
`);
if (presentationDirectory) {
  environment.PIPEWIRE_MODULE_DIR = presentationDirectory + ":" + systemModuleDirectory;
  writeConfiguration("pipewire/pipewire.conf.d/60-pipetune-presentation.conf",
    "context.modules = [ { name = libpipewire-module-pipetune-presentation } ]\n");
}
if (scenario.startsWith("policy-pulse")) {
  assert.ok(pulseServer && pulseDriver, "PulseAudio server and client probe paths are required");
  writeConfiguration("pipewire/pipewire-pulse.conf.d/99-pipetune-test.conf", `pulse.properties = {
    server.address = [ "unix:${join(runtime, "pulse-native")}" ]
  }
`);
}
if (wirePlumber04) {
  // Disable hardware monitors before WirePlumber's 90-enable-all.lua runs.
  writeConfiguration("wireplumber/main.lua.d/60-pipetune-test.lua", `alsa_monitor.enabled = false
v4l2_monitor.enabled = false
libcamera_monitor.enabled = false
`);
  writeConfiguration("wireplumber/bluetooth.lua.d/60-pipetune-test.lua", `bluez_monitor.enabled = false
bluez_midi_monitor.enabled = false
`);
  if (!regularRoutePolicy && (scenario === "product-output-change" || scenario.startsWith("product-output-profile-"))) {
    // The synthetic card deliberately resets input routes during profile changes.
    // Keep its scripted mutations as the external actor under test, as a
    // control alongside the route-policy scenarios, which also
    // exercise WirePlumber's normal selection and persistence of input routes.
    writeConfiguration("wireplumber/scripts/policy-device-routes.lua",
      "-- Synthetic profile fixtures control input-route mutations explicitly.\n");
  }
} else {
  writeConfiguration("wireplumber/wireplumber.conf.d/99-pipetune-test.conf", `wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.bluez = disabled
    monitor.bluez-midi = disabled
    monitor.v4l2 = disabled
    monitor.libcamera = disabled
  }
}
`);
}

if (scenario.startsWith("policy") || scenario.startsWith("product")) {
  assert.ok(policyFixture, "the PipeTune WirePlumber policy fixture is required");
  const policy = (part) => {
    const result = spawnSync(policyFixture, [part], { encoding: "utf8" });
    assert.equal(result.status, 0, result.stderr);
    return result.stdout;
  };
  if (wirePlumber04) {
    writeConfiguration("wireplumber/main.lua.d/60-pipetune-streams.lua", policy("stream-configuration"));
    writeConfiguration("wireplumber/scripts/pipetune-node-visibility.lua", policy("visibility"));
    writeConfiguration("wireplumber/policy.lua.d/60-pipetune-policy.lua", policy("configuration"));
    const endpointClient = policy("endpoint-client");
    const publicInputReady = scenario.startsWith("product-daemon-volume") ? `
-- Signal when the real policy can accept playback at the public input.
local pipetune_test_reported_serial = nil
local pipetune_test_pending_serial = nil
local function pipetune_test_public_input_serial()
  if endpoints_om:get_n_objects() == 0 then return nil end
  for input in public_inputs_om:iterate() do
    return input.properties["object.serial"]
  end
  return nil
end
local function pipetune_test_report_input_ready()
  local serial = pipetune_test_public_input_serial()
  if not serial or serial == pipetune_test_reported_serial or
      serial == pipetune_test_pending_serial then return end
  pipetune_test_pending_serial = serial
  Core.sync(function(error)
    pipetune_test_pending_serial = nil
    if error then
      Log.warning("public input policy synchronization failed: " .. tostring(error))
      return
    end
    if serial == pipetune_test_public_input_serial() and
        serial ~= pipetune_test_reported_serial then
      pipetune_test_reported_serial = serial
      Log.info("PIPETUNE_POLICY_PUBLIC_INPUT_READY " .. serial)
    end
  end)
end
public_inputs_om:connect("objects-changed", pipetune_test_report_input_ready)
endpoints_om:connect("objects-changed", pipetune_test_report_input_ready)
` : "";
    writeConfiguration("wireplumber/scripts/pipetune-endpoint-client.lua", endpointClient + publicInputReady);
    writeConfiguration("wireplumber/scripts/pipetune-endpoint-device.lua", policy("endpoint-device"));
  } else {
    mkdirSync(join(data, "wireplumber", "scripts"), { recursive: true });
    writeFileSync(join(data, "wireplumber", "scripts", "pipetune-node-visibility.lua"),
      policy("visibility"));
    writeConfiguration("wireplumber/wireplumber.conf.d/60-pipetune-visibility.conf",
      policy("visibility-configuration"));
  }
}

const children = [];
const start = (name, executable, args, childEnvironment = environment) => {
  const child = spawn(executable, args, {
    env: childEnvironment,
    stdio: ["ignore", "pipe", "pipe"],
    detached: true,
  });
  const record = { name, child, stdout: "", stderr: "", result: undefined, error: undefined };
  child.stdout.on("data", (chunk) => { record.stdout += chunk; });
  child.stderr.on("data", (chunk) => { record.stderr += chunk; });
  child.on("error", (error) => { record.error = error; });
  record.completion = new Promise((resolve) => {
    child.on("close", (code, signal) => {
      record.result = { code, signal };
      resolve(record.result);
    });
  });
  children.push(record);
  return record;
};
const terminate = (record, signal) => {
  if (record.child.pid === undefined) return;
  try {
    // dbus-run-session starts descendants that keep its output pipes open.
    // Terminate the isolated process group, including those descendants.
    process.kill(-record.child.pid, signal);
  } catch (error) {
    if (error.code !== "ESRCH") throw error;
  }
};
const socketReady = async (socket, previousCreation) => {
  await new Promise((resolve, reject) => {
    const changed = () => {
      const current = statSync(socket, { bigint: true, throwIfNoEntry: false });
      if (current !== undefined && current.ctimeNs !== previousCreation) finish();
    };
    const observer = watch(dirname(socket), changed);
    const timeout = setTimeout(() => finish(new Error(`isolated audio socket was not created: ${socket}`)), 15000);
    const finish = (error) => {
      clearTimeout(timeout);
      observer.close();
      if (error) reject(error);
      else resolve();
    };
    changed();
  });
};

const waitForMessage = async (record, message) => {
  await new Promise((resolve, reject) => {
    const finish = (error) => {
      clearTimeout(timeout);
      record.child.stderr.off("data", changed);
      record.child.off("close", closed);
      if (error) reject(error);
      else resolve();
    };
    const changed = () => { if (record.stderr.includes(message)) finish(); };
    const closed = () => finish(new Error(`${record.name} exited before ${message}`));
    const timeout = setTimeout(() => finish(new Error(`did not receive ${message}`)), 20000);
    record.child.stderr.on("data", changed);
    record.child.on("close", closed);
    if (record.stderr.includes(message)) finish();
    else if (record.result !== undefined) closed();
    else changed();
  });
};
const countMessages = (record, message) => record.stderr.split(message).length - 1;
const waitForNextMessage = async (record, message, previousCount) => {
  await new Promise((resolve, reject) => {
    const finish = (error) => {
      clearTimeout(timeout);
      record.child.stderr.off("data", changed);
      record.child.off("close", closed);
      if (error) reject(error);
      else resolve();
    };
    const changed = () => { if (countMessages(record, message) > previousCount) finish(); };
    const closed = () => finish(new Error(`${record.name} exited before ${message}`));
    const timeout = setTimeout(() => finish(new Error(`did not receive a new ${message}`)), 20000);
    record.child.stderr.on("data", changed);
    record.child.on("close", closed);
    if (record.result !== undefined) closed();
    else changed();
  });
};

let testFailure;
try {
  start("PipeWire", pipewire, []);
  await socketReady(join(runtime, remote), undefined);
  if (scenario.startsWith("policy-pulse")) {
    start("PulseAudio protocol server", "dbus-run-session", ["--", pulseServer]);
    await socketReady(join(runtime, "pulse-native"), undefined);
  }
  let manager = start("WirePlumber", "dbus-run-session", ["--", wireplumber]);
  if (scenario === "product-direct") {
    const audio = start("dual input PCM fixtures", driver, ["audio", scenario]);
    await waitForMessage(audio, "product:devices-ready");
    const preset = join(directory, "direct.effetune_preset");
    writeFileSync(preset, JSON.stringify({ pipeline: [
      { name: "Matrix", enabled: true, channel: "All", parameters: { mx: "001102p13" } },
    ] }));
    const product = start("dual input runtime", driver, ["runtime", preset, scenario]);
    await waitForMessage(product, "product:runtime-ready");
    audio.child.kill("SIGUSR1");
    const mixed = [[0.125, 0.25, 0.5, 1.25], [0.375, 0.75, 0.75, 0.75],
      [0.25, 0.5, 0.25, 0.5], [0.125, 0.25, 0.5, 0.75],
      [0.125, 0.25, 0.5, 0.75], [0.375, 0.75, 0.5, 0.75],
      [0.3125, 0.625, 0.25, 0.375], [0.25, 0.5, 0, 0], [0.375, 0.75, 0.5, 0.75]];
    const dsp = [[0, 0, 0, 0], [0.25, 0.5, 0.25, 0.5], [0.25, 0.5, 0.25, 0.5],
      [0, 0], [0, 0], [0.25, 0.5], [0.25, 0.5], [0.25, 0.5], [0.25, 0.5]];
    for (let stage = 0; stage <= 8; ++stage) {
      await waitForMessage(audio, `product:direct-stage-${stage}`);
      const client = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
      const deadline = setTimeout(() => client.destroy(), 10000);
      let lines;
      let matched = false;
      try {
        await once(client, "connect");
        lines = createInterface({ input: client, crlfDelay: Infinity });
        client.write(JSON.stringify({ command: "subscribe" }) + "\n");
        for await (const line of lines) {
          const status = JSON.parse(line);
          const equal = (actual, expected) => actual?.length === expected.length &&
            actual.every((peak, channel) => Math.abs(peak - expected[channel]) < 0.00001);
          if (equal(status.mixedOutputPeaks, mixed[stage]) && equal(status.outputPeaks, dsp[stage]) &&
              (stage < 4 || status.graphSampleRate === 96000)) { matched = true; break; }
        }
        assert.ok(matched, `stage ${stage}: mixed and preset meters must report their own PCM, with the requested graph rate`);
      } finally {
        clearTimeout(deadline); lines?.close(); client.destroy();
      }
      if (stage === 2 || stage === 3) {
        const request = stage === 2 ? { command: "bypass" } :
          { command: "set-rate", rateMode: "fixed", sampleRate: 96000, enforcement: "force" };
        const client = start("dual input control", driver, ["control", environment.PIPETUNE_PRODUCT_SOCKET, JSON.stringify(request)]);
        assert.equal((await client.completion).code, 0, client.stderr);
        assert.equal(JSON.parse(client.stdout).ok, true, client.stdout);
      }
      audio.child.kill("SIGUSR2");
    }
    assert.equal((await audio.completion).code, 0, audio.stderr);
    terminate(product, "SIGTERM");
    assert.equal((await product.completion).code, 0, product.stderr);
  } else if (scenario.startsWith("product-source-resume")) {
    const audio = start("source switching fixtures", driver, ["audio", scenario]);
    await waitForMessage(audio, "product:devices-ready");
    const preset = join(directory, "source-resume.effetune_preset");
    const identity = Array.from({ length: 256 }, (_, index) => index === 0 || index === 17 ? 1 : 0);
    writeFileSync(preset, JSON.stringify({ pipeline: [
      { name: "Spatial Mapper", enabled: true, channel: "All",
        parameters: { ic: 2, ep: false, dm: identity, fm: identity, rm: identity } },
      { name: "Matrix", enabled: true, channel: "All", parameters: { mx: "001102p13" } },
    ] }));
    const product = start("source switching runtime", driver, ["runtime", preset, scenario]);
    await waitForMessage(product, "product:runtime-ready");
    audio.child.kill("SIGUSR1");
    await waitForMessage(audio, "product:source-a-heard");
    audio.child.kill("SIGUSR2");
    await waitForMessage(audio, "product:source-stopped");
    const client = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
    const deadline = setTimeout(() => client.destroy(), 15000);
    let lines;
    let transportError;
    client.on("error", (error) => { transportError = error; });
    client.on("close", () => lines?.close());
    try {
      await once(client, "connect");
      lines = createInterface({ input: client, crlfDelay: Infinity });
      client.write(`${JSON.stringify({ command: "subscribe" })}\n`);
      let stopped = false;
      for await (const line of lines) {
        const status = JSON.parse(line);
        assert.equal(status.ok, true);
        if (status.inputSampleRate === 0) { stopped = true; break; }
      }
      assert.ok(stopped, transportError?.message ?? "the input graph never stopped after source A closed");
    } finally {
      clearTimeout(deadline);
      lines?.close();
      client.destroy();
    }
    audio.child.kill("SIGUSR2");
    await waitForMessage(audio, "product:source-b-clean");
    audio.child.kill("SIGUSR2");
    assert.equal((await audio.completion).code, 0, audio.stderr);
    terminate(product, "SIGTERM");
    assert.equal((await product.completion).code, 0, product.stderr);
  } else if (scenario === "product-preset-meters") {
    const audio = start("meter PCM fixtures", driver, ["audio", scenario]);
    await waitForMessage(audio, "product:devices-ready");
    const preset = join(directory, "meter.effetune_preset");
    const matrix = (mx) => ({ name: "Matrix", enabled: true, channel: "All", parameters: { mx } });
    writeFileSync(preset, JSON.stringify({ pipeline: [matrix("001102p13")] }));
    const product = start("meter runtime", driver, ["runtime", preset, scenario]);
    await waitForMessage(product, "product:runtime-ready");
    audio.child.kill("SIGUSR1");
    const control = async (request, success = true) => {
      const child = start("meter control", driver, ["control", environment.PIPETUNE_PRODUCT_SOCKET, JSON.stringify(request)]);
      assert.equal((await child.completion).code, 0, child.stderr);
      const response = JSON.parse(child.stdout);
      assert.equal(response.ok, success, child.stdout);
      return response;
    };
    const expectPeaks = async (expected) => {
      const client = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
      const timer = setTimeout(() => client.destroy(new Error("preset output peaks did not arrive")), 15000);
      let lines;
      try {
        await once(client, "connect");
        lines = createInterface({ input: client, crlfDelay: Infinity });
        client.write(`${JSON.stringify({ command: "subscribe" })}\n`);
        for await (const line of lines) {
          const response = JSON.parse(line);
          assert.equal(response.ok, true);
          if (response.outputPeaks.length === expected.length && response.outputPeaks.every(
            (peak, channel) => Math.abs(peak - expected[channel]) < 0.00001)) return response;
        }
        assert.fail("status subscription ended before the expected preset peaks");
      } finally {
        clearTimeout(timer);
        lines?.close();
        client.destroy();
      }
    };
    const initial = await expectPeaks([0.25, 0.5, 0.25, 0.5]);
    assert.equal(initial.outputConfiguration.mode, "single", "four generated channels must be measured with stereo hardware");
    const stereo = join(directory, "stereo.effetune_preset");
    writeFileSync(stereo, JSON.stringify({ pipeline: [{ name: "Volume", channel: "All", parameters: { vl: 0 } }] }));
    await control({ command: "load", preset: stereo });
    await expectPeaks([0.25, 0.5]);
    await control({ command: "set-output", configuration: { ...initial.outputConfiguration, mode: "multiple" } });
    await expectPeaks([0.25, 0.5]);
    writeFileSync(preset, JSON.stringify({ pipeline: [matrix("001102p1304p15")] }));
    await control({ command: "load", preset });
    await expectPeaks([0.25, 0.5, 0.25, 0.5, 0.25, 0.5]);
    await control({ command: "set-rate", rateMode: "fixed", sampleRate: 96000, enforcement: "suggest" });
    await control({ command: "set-dsp-backend", backend: "simd", simdVariant: "auto" });
    await expectPeaks([0.25, 0.5, 0.25, 0.5, 0.25, 0.5]);
    writeFileSync(`${preset}.new`, JSON.stringify({ pipeline: [matrix("001102p1304p15"), matrix("0011")] }));
    renameSync(`${preset}.new`, preset);
    await expectPeaks([0.25, 0.5]);
    writeFileSync(`${preset}.new`, JSON.stringify({ pipeline: [matrix("001102p1304p15")] }));
    renameSync(`${preset}.new`, preset);
    await expectPeaks([0.25, 0.5, 0.25, 0.5, 0.25, 0.5]);
    const invalid = join(directory, "invalid.effetune_preset");
    writeFileSync(invalid, "{invalid");
    await control({ command: "load", preset: invalid }, false);
    await expectPeaks([0.25, 0.5, 0.25, 0.5, 0.25, 0.5]);
    await control({ command: "bypass" });
    const final = await expectPeaks([0.25, 0.5]);
    assert.equal(final.processingErrors, 0);
    audio.child.kill("SIGUSR2");
    assert.equal((await audio.completion).code, 0, audio.stderr);
    terminate(product, "SIGTERM");
    assert.equal((await product.completion).code, 0, product.stderr);
  } else if (scenario.startsWith("product-output-status")) {
    const audio = start("inventory fixtures", driver, ["audio", scenario]);
    await waitForMessage(audio, "product:devices-ready");
    const product = start("product runtime", driver, ["runtime", "bypass", scenario]);
    await waitForMessage(product, "product:runtime-ready");
    const client = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
    const deadline = setTimeout(() => client.destroy(), 10000);
    let lines;
    let transportError;
    client.on("error", (error) => { transportError = error; });
    client.on("close", () => lines?.close());
    let stage = 0;
    let saved;
    try {
      await once(client, "connect");
      lines = createInterface({ input: client, crlfDelay: Infinity });
      client.write(`${JSON.stringify({ command: "subscribe" })}\n`);
      for await (const line of lines) {
        const status = JSON.parse(line);
        assert.equal(status.ok, true, line);
        assert.equal(status.outputConfiguration.mode, scenario.endsWith("-single") ? "single" : "multiple");
        assert.equal(status.outputPeaks.length, scenario.endsWith("-single") ? 2 : 4,
          "output meters must follow the running stream width in either mode");
        assert.ok(status.outputPeaks.every((peak) => Number.isFinite(peak) && peak >= 0));
        if (!saved) saved = status.outputConfiguration;
        assert.deepEqual(status.outputConfiguration, saved, "inventory changes must preserve every saved slot");
        if (!status.outputInventoryReady) continue;
        assert.equal(status.outputInventoryError, null);
        const first = status.availableOutputs.find((output) => output.nodeName === "pipetune_product_device_0");
        const expected = stage === 1 ? !first : first?.device.profile === (stage === 2 ? "changed" : "");
        if (!expected) continue;
        assert.equal(status.availableOutputs.length, stage === 1 ? 2 : 3,
          `internal nodes must not enter the inventory: ${status.availableOutputs.map((output) => output.nodeName).join(", ")}`);
        assert.deepEqual(status.outputVolumes.map((output) => output.nodeSerial).sort(),
          status.availableOutputs.map((output) => output.nodeSerial).sort(),
          "volume reports must belong to the current output generation without retaining disconnected devices");
        if (first) {
          assert.deepEqual(first.device.channelPositions, ["FL", "FR"]);
          assert.equal(typeof first.nodeSerial, "string");
        }
        audio.child.kill("SIGUSR2");
        if (++stage === 4) break;
      }
    } finally {
      clearTimeout(deadline);
      lines?.close();
      client.destroy();
    }
    assert.equal(transportError, undefined);
    assert.equal(stage, 4, "subscriptions must report initial, missing, changed-profile, and restored outputs");
    assert.equal((await audio.completion).code, 0, audio.stderr);
    terminate(product, "SIGTERM");
    assert.equal((await product.completion).code, 0, product.stderr);
    process.stdout.write(`${JSON.stringify({ inventoryStages: stage, configuration: saved })}\n`);
  } else if (scenario.startsWith("product-daemon")) {
    assert.ok(productDaemon, "the product daemon executable is required");
    const volumeRestore = scenario.startsWith("product-daemon-volume");
    const savedMasterGain = scenario.endsWith("-zero") ? 0 : 0.5;
    environment.PIPETUNE_PRODUCT_INPUT = "pipetune_sink";
    const socket = join(runtime, "pipetune", "control.sock");
    mkdirSync(dirname(socket), { recursive: true });
    const preset = join(directory, "saved.effetune_preset");
    const pipeline = [
      { name: "Matrix", enabled: true, channel: "All", parameters: { mx: "001102p13" } },
    ];
    if (volumeRestore) pipeline.push({ name: "DC Offset", enabled: true, channel: "All", parameters: { of: 0.125 } });
    writeFileSync(preset, JSON.stringify({ pipeline }));
    const outputs = [0, 1].map((index) => {
      const name = `pipetune_product_device_${index}`;
      return { id: name, enabled: true, device: {
        identity: { api: "node", location: name, port: name, vendor: "", product: "", serial: "" },
        name, profile: "", channelPositions: ["FL", "FR"],
      } };
    });
    const routing = { mode: "multiple", outputs, channels: outputs.flatMap((output) =>
      [0, 1].map((deviceChannel) => ({ outputId: output.id, deviceChannel, label: "" }))) };
    writeConfiguration("pipetune/environment", `PIPETUNE_PRESET=${JSON.stringify(preset)}\nPIPETUNE_RATE=48000\nPIPETUNE_OUTPUT=${JSON.stringify(JSON.stringify(routing))}\n`);
    const cli = async (arguments_, success = true) => {
      const command = start("output CLI", productDaemon, ["output", ...arguments_]);
      const result = await command.completion;
      assert.equal(result.code === 0, success, `${command.stdout}\n${command.stderr}`);
      return command.stdout;
    };
    const endpoints = ["--socket", socket, "--config", join(config, "pipetune", "environment")];
    const assertPublicOutput = () => {
      const dump = spawnSync("pw-dump", ["--no-colors"], { env: environment, encoding: "utf8" });
      assert.equal(dump.status, 0, dump.stderr);
      const graph = JSON.parse("[" + dump.stdout.replace(/\]\s*\[/gu, "],[") + "]").flat();
      const output = graph.find((item) => item.info?.props?.["node.name"] === "pipetune_sink");
      assert.equal(output?.info.props["node.description"], "PipeTune Processed Audio",
        "startup and live mode switches must publish the same output name");
    };
    // Restart both the daemon and fixtures. The next run must recover saved
    // routing after the devices have been recreated as new runtime objects.
    for (let attempt = 0; attempt < (volumeRestore ? 3 : 2); ++attempt) {
      const inputReadyMessage = "PIPETUNE_POLICY_PUBLIC_INPUT_READY";
      const previousInputReadyCount = wirePlumber04 && volumeRestore ?
        countMessages(manager, inputReadyMessage) : 0;
      const audioScenario = volumeRestore ? `${scenario}-${["save", "restored", "unmuted"][attempt]}` : scenario;
      const audio = start("daemon audio fixture", driver, ["audio", audioScenario]);
      await waitForMessage(audio, "product:devices-ready");
      if (attempt === 0 && !volumeRestore) {
        assert.match(await cli(["mode", "single", ...endpoints]), /next daemon start/u);
        assert.match(await cli(["select", ...outputs.map((output) => output.id), ...endpoints]), /next daemon start/u);
      }
      const previousCreation = statSync(socket, { bigint: true, throwIfNoEntry: false })?.ctimeNs;
      const daemon = start("product daemon", productDaemon, ["daemon", "--config", join(config, "pipetune", "environment")]);
      await socketReady(socket, previousCreation);
      const status = start("daemon status", driver, ["control", socket, JSON.stringify({ command: "status" })]);
      assert.equal((await status.completion).code, 0, status.stderr);
      const response = JSON.parse(status.stdout);
      assert.equal(response.ok, true, status.stdout);
      assert.equal(response.processingMode, "preset", status.stdout);
      assert.deepEqual(JSON.parse(await cli(["get", "--json", "--socket", socket])).outputConfiguration, routing);
      if (attempt === 0 && !volumeRestore) {
        assert.match(await cli(["mode", "single", ...endpoints]), /OS selects/u);
        assert.match(await cli(["mode", "multiple", ...endpoints]), /Ch 4/u);
        assertPublicOutput();
        // Selecting the existing devices in reverse order must retain their
        // saved Ch numbers; labels then survive a real daemon restart.
        await cli(["select", outputs[1].id, outputs[0].id, ...endpoints]);
        assert.deepEqual(JSON.parse(await cli(["get", "--json", "--socket", socket])).outputConfiguration, routing);
        routing.channels[2].label = "サブ左";
        await cli(["set", JSON.stringify(routing), ...endpoints]);
        await cli(["select", "missing-output", ...endpoints], false);
        assert.deepEqual(JSON.parse(await cli(["get", "--json", "--socket", socket])).outputConfiguration, routing);
        assert.match(await cli(["get", "--socket", socket]), /Ch 3 \|.*1 \(FL\).*サブ左/u);
      }
      if (wirePlumber04 && volumeRestore) {
        // Before its endpoint is ready, WirePlumber 0.4 can route a pinned
        // stream to the default device despite node.dont-fallback. Start the
        // volume check when the public input is available to its policy.
        await waitForNextMessage(manager, inputReadyMessage, previousInputReadyCount);
      }
      audio.child.kill("SIGUSR1");
      assert.equal((await audio.completion).code, 0, `${audio.stdout}\n${audio.stderr}`);
      const report = JSON.parse(audio.stdout);
      // The control socket precedes asynchronous graph publication at startup.
      // Successful PCM is the readiness barrier for a fresh desktop query.
      if (volumeRestore)
        assert.equal(report.processedOutputNamed, true, "the PCM fixture must observe the public Processed output name");
      else assertPublicOutput();
      assert.ok(report.receivedFrames.every((frames) => frames >= 32768));
      if (volumeRestore) {
        assert.equal(report.stages.length, attempt === 0 ? 3 : 4);
        assert.ok(report.stages.slice([0, 2, 3][attempt]).every((stage) => stage.every((frames) => frames >= 32768)),
          "both outputs must preserve the saved gain and mute, including the DSP's added DC component");
      }
      const killed = volumeRestore && attempt === 1;
      terminate(daemon, killed ? "SIGKILL" : "SIGTERM");
      const result = await daemon.completion;
      assert.equal(killed ? result.signal : result.code, killed ? "SIGKILL" : 0, daemon.stderr);
      if (killed) {
        // Observe the durable state before restarting its owner. No fixed
        // sleep or process-local cache can establish persistence here.
        const path = join(state, "wireplumber", "pipetune-master-output");
        await new Promise((resolve, reject) => {
          const changed = () => {
            if (!existsSync(path)) return;
            const saved = readFileSync(path, "utf8").split(/\r?\n/u);
            if (saved.includes(`pipetune_sink:gain=${savedMasterGain}`) && saved.includes("pipetune_sink:mute=false")) finish();
          };
          const observer = watch(dirname(path), changed);
          const deadline = setTimeout(() => finish(new Error("master controls were not saved to disk")), 10000);
          const finish = (error) => {
            clearTimeout(deadline);
            observer.close();
            if (error) reject(error);
            else resolve();
          };
          changed();
        });
        terminate(manager, "SIGTERM");
        await manager.completion;
        manager = start("restarted WirePlumber", "dbus-run-session", ["--", wireplumber]);
      }
      process.stdout.write(`${JSON.stringify({ attempt, ...report })}\n`);
    }
  } else if (scenario.startsWith("product")) {
    const audio = start("product audio fixture", driver, ["audio", scenario]);
    await waitForMessage(audio, "product:devices-ready");
    let profileFixture;
    if (["product-output-change", "product-output-profile-unknown", "product-output-profile-shutdown",
      "product-output-profile-restore-failure", "product-output-profile-unavailable",
      "product-output-profile-settle-input"].includes(scenario)) {
      profileFixture = start("product profile fixture", pulseServer, [], {
        ...environment, PIPETUNE_PROFILE_FIXTURE: "1", PIPETUNE_PROFILE_RECREATE: "1",
        ...(scenario === "product-output-change" ? { PIPETUNE_PROFILE_MONITOR: "1" } : {}),
        ...(scenario === "product-output-profile-unknown" ? { PIPETUNE_PROFILE_NO_CLASSES: "1" } : {}),
      });
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("synthetic profile card did not bind")), 10000);
        const changed = () => {
          if (!profileFixture.stdout.includes("card-bound=")) return;
          clearTimeout(timeout);
          profileFixture.child.stdout.off("data", changed);
          resolve();
        };
        profileFixture.child.stdout.on("data", changed);
        changed();
      });
    }
    const preset = join(directory, "matrix.effetune_preset");
    const pipeline = [
      { name: "Matrix", enabled: true, channel: "All", parameters: { mx: scenario === "product-slots" ? "001102p1304" : onePcmOutput ? "0011" : "001102p13" } },
    ];
    if (["product-volume", "product-reconnect", "product-profile", "product-restart", "product-restart-mute"].includes(scenario)) pipeline.push({ name: "DC Offset", enabled: true, channel: "All", parameters: { of: 0.125 } });
    if (scenario === "product-time-alignment") pipeline.push({ name: "Time Alignment", enabled: true, channel: "34", parameters: { dl: 1 } });
    if (scenario.startsWith("product-sfz-")) {
      const root = join(config, "effetune");
      writeConfiguration("effetune/main.sfz", "<region> sample=*sine key=83\n");
      writeConfiguration("effetune/sfz-references.json", JSON.stringify([{
        id: "0123456789abcdef01234567", name: "Output alignment", root,
        path: join(root, "main.sfz"),
      }]));
      environment.XDG_CACHE_HOME = join(directory, "cache");
      pipeline.push({ name: "SFZ Note Player", enabled: true, channel: "34", parameters: {
        sf: "0123456789abcdef01234567", dt: scenario.startsWith("product-sfz-event") ? "Event" : "Classic",
        mn: 83, mx: 83, dm: 100, wm: 0,
      } });
    }
    writeFileSync(preset, JSON.stringify({ pipeline }));
    const runtime = start("product runtime", driver, ["runtime", scenario === "product-bypass" ? "bypass" : preset, scenario]);
    await waitForMessage(runtime, "product:runtime-ready");
    audio.child.kill("SIGUSR1");
    if (graphClockTest || ["product-controls", "product-output-change", "product-output-profile-unknown",
      "product-output-profile-shutdown", "product-output-profile-restore-failure",
      "product-output-profile-unavailable", "product-output-profile-settle-input", "product-output-timeout",
      "product-output-clock-unavailable", "product-output-recovery"].includes(scenario)) {
      const control = async (request, success = true) => {
        const client = start("product control", driver, ["control", environment.PIPETUNE_PRODUCT_SOCKET, JSON.stringify(request)]);
        const timeout = setTimeout(() => terminate(client, "SIGTERM"), scenario === "product-output-timeout" ? 20000 : 10000);
        const result = await client.completion;
        clearTimeout(timeout);
        assert.equal(result.code, 0, client.stderr);
        const response = JSON.parse(client.stdout);
        assert.equal(response.ok, success, client.stdout);
        return response;
      };
      const next = async (stage) => {
        await waitForMessage(audio, `product:controls-stage-${stage}`);
        const clockUnavailable = scenario === "product-output-clock-unavailable";
        if (!graphClockTest && !clockUnavailable) return;
        const rate = stage === 0 ? graphRate : stage === 8 ? 48000 : 96000;
        const quantum = stage === 0 || stage === 8 ? graphQuantum : graphQuantum * 2;
        const expected = (0.5 * quantum + 64) * 1e9 / rate + 1e6;
        const client = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
        const deadline = setTimeout(() => client.destroy(), 10000);
        let lines;
        let transportError;
        let observed = false;
        client.on("error", (error) => { transportError = error; });
        client.on("close", () => lines?.close());
        try {
          await once(client, "connect");
          lines = createInterface({ input: client, crlfDelay: Infinity });
          client.write(`${JSON.stringify({ command: "subscribe" })}\n`);
          for await (const line of lines) {
            const status = JSON.parse(line);
            assert.equal(status.ok, true, line);
            const a = status.outputTimings.find((entry) => entry.outputId === "pipetune_product_device_0");
            const b = status.outputTimings.find((entry) => entry.outputId === "pipetune_product_device_1");
            if (a?.activity !== "active" || b?.activity !== "active") continue;
            if (clockUnavailable) {
              for (const entry of [a, b]) {
                assert.equal(entry.reportedLatencyNanoseconds, null);
                assert.equal(entry.estimatedCompensationNanoseconds, null);
              }
            } else if (a.estimatedCompensationNanoseconds === null || b.estimatedCompensationNanoseconds === null ||
                Math.abs(a.estimatedCompensationNanoseconds - expected) > 1 || b.estimatedCompensationNanoseconds !== 0) continue;
            assert.equal(status.outputTimings.length, 2);
            for (const entry of [a, b]) assert.ok(status.availableOutputs.some((output) => output.nodeSerial === entry.nodeSerial));
            observed = true;
            break;
          }
        } finally {
          clearTimeout(deadline);
          lines?.close();
          client.destroy();
        }
        assert.equal(transportError, undefined);
        assert.ok(observed, `stage ${stage} must publish both active paths and the estimated compensation`);
      };
      if (scenario === "product-output-recovery") {
        const initial = await control({ command: "status" });
        const inventories = [[0,1,2], [1,2], [2], [1,2], [0,1,2], [0,1],
          [0,1,2], [0,1,2], [0,1,2], [1,2], [0,1,2], [0,1,2]];
        for (let stage = 0; stage < inventories.length; ++stage) {
          if (stage === 11) await control({ command: "set-output", configuration: initial.outputConfiguration });
          if (stage > 0) audio.child.kill("SIGUSR2");
          const single = stage === 9 || stage === 10;
          if (stage === 9) await control({ command: "set-output",
            configuration: { ...initial.outputConfiguration, mode: single ? "single" : "multiple" } });
          const client = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
          const deadline = setTimeout(() => client.destroy(new Error(`inventory stage ${stage} stalled`)), 10000);
          const lines = createInterface({ input: client });
          let transportError;
          client.on("error", (error) => { transportError = error; lines.close(); });
          let observed;
          try {
            await once(client, "connect");
            client.write(`${JSON.stringify({ command: "subscribe" })}\n`);
            for await (const line of lines) {
              const status = JSON.parse(line);
              assert.equal(status.ok, true, line);
              assert.deepEqual(status.outputConfiguration, { ...initial.outputConfiguration,
                mode: single ? "single" : "multiple" }, "hotplug must preserve every saved device and channel");
              assert.equal(status.preset, initial.preset);
              assert.equal(status.processingMode, "preset");
              assert.equal(status.activePluginCount, initial.activePluginCount);
              assert.equal(status.configurationRevision, initial.configurationRevision + (stage >= 11 ? 2 : stage >= 9 ? 1 : 0));
              const outputs = status.availableOutputs.filter((output) => output.nodeName.startsWith("pipetune_product_device_"));
              const names = outputs.map((output) => Number(output.nodeName.slice(-1))).sort();
              if (JSON.stringify(names) !== JSON.stringify(inventories[stage])) continue;
              if (outputs.some((output) => output.device.profile !== (stage === 7 && output.nodeName.endsWith("_0") ? "changed" : ""))) continue;
              observed = status;
              break;
            }
          } finally {
            clearTimeout(deadline);
            lines.close();
            client.destroy();
          }
          assert.equal(transportError, undefined);
          assert.ok(observed, `inventory stage ${stage} must complete`);
          await waitForMessage(audio, `product:recovery-stage-${stage}`);
          const dump = spawnSync("pw-dump", ["--no-colors"], { env: environment, encoding: "utf8" });
          assert.equal(dump.status, 0, dump.stderr);
          const graph = JSON.parse("[" + dump.stdout.replace(/\]\s*\[/gu, "],[") + "]").flat();
          if (!single) {
            const output = graph.find((item) => item.info?.props?.["node.name"] === "pipetune_product_input");
            assert.equal(output?.info.props["node.description"], "PipeTune Processed Audio");
            assert.ok(graph.some((item) => item.metadata?.some((entry) =>
              entry.key === "default.audio.sink" && entry.value?.name === "pipetune_product_input")),
              "the public output must stay selected even when every configured device is absent");
          }
        }
        audio.child.kill("SIGUSR2");
      } else if (scenario === "product-output-clock-unavailable") {
        await next(0);
        audio.child.kill("SIGUSR2");
      } else if (scenario === "product-output-timeout") {
        await next(0);
        const initial = await control({ command: "status" });
        const wide = structuredClone(initial.outputConfiguration);
        while (wide.channels.length < 16) wide.channels.push({ outputId: "", deviceChannel: 0, label: "Reserved" });
        const request = { command: "set-output", configuration: wide, preset: null, expectedRevision: initial.configurationRevision };
        const rejected = await control(request, false);
        assert.match(rejected.error, /timed out while negotiating output configuration/);
        await waitForMessage(runtime, "product:unconfigured-output-stream");
        await waitForMessage(runtime, "product:unconfigured-output-retired");
        const restored = await control({ command: "status" });
        assert.deepEqual(restored.outputConfiguration, initial.outputConfiguration);
        assert.equal(restored.configurationRevision, initial.configurationRevision);
        assert.equal(restored.preset, preset);
        assert.equal(restored.processingMode, "preset");
        assert.equal(restored.activePluginCount, initial.activePluginCount);
        assert.equal(restored.inputChannelCount, 2);
        audio.child.kill("SIGUSR2");
        await next(1);
        // A timeout must release the pending request and staged DSP so the
        // identical change can subsequently succeed with normal negotiation.
        const retried = await control(request);
        assert.deepEqual(retried.outputConfiguration, wide);
        assert.equal(retried.configurationRevision, initial.configurationRevision + 1);
        assert.equal(retried.preset, null);
        assert.equal(retried.processingMode, "bypass");
        audio.child.kill("SIGUSR2");
      } else if (scenario === "product-output-profile-shutdown") {
        await next(0);
        audio.child.kill("SIGUSR2");
      } else if (scenario === "product-output-profile-unknown") {
        await next(0);
        const initial = await control({ command: "status" });
        const active = initial.availableOutputProfiles.find((item) => item.card.api === "pipetune-fixture" && item.active);
        const optical = initial.availableOutputProfiles.find((item) => item.card.api === "pipetune-fixture" &&
          item.outputKey === "iec958-stereo");
        assert.ok(active && optical);
        const failed = await control({ command: "set-output", configuration: initial.outputConfiguration,
          profileSelections: [{ card: active.card, cardProfileName: optical.cardProfileName,
            outputKey: optical.outputKey, allowMissingForRestore: false }] }, false);
        assert.match(failed.error, /did not report how many output Nodes/u);
        const retained = await control({ command: "status" });
        assert.deepEqual(retained.outputConfiguration, initial.outputConfiguration);
        assert.equal(retained.configurationRevision, initial.configurationRevision);
        assert.equal(retained.availableOutputProfiles.find((item) => item.card.api === "pipetune-fixture" && item.active)?.cardProfileName,
          active.cardProfileName);
        assert.ok(retained.availableOutputProfiles.find((item) => item.card.api === "pipetune-fixture" &&
          item.outputKey === "iec958-stereo")?.observedOutputs.length);
        audio.child.kill("SIGUSR2");
      } else if (["product-output-profile-restore-failure", "product-output-profile-unavailable",
        "product-output-profile-settle-input"].includes(scenario)) {
        await next(0);
        audio.child.kill("SIGUSR2");
        assert.equal((await audio.completion).code, 0, audio.stderr);
        const initial = await control({ command: "status" });
        const original = initial.outputConfiguration;
        const analog = initial.availableOutputProfiles.find((item) =>
          item.card.api === "pipetune-fixture" && item.active);
        const optical = initial.availableOutputProfiles.find((item) =>
          item.card.api === "pipetune-fixture" && item.outputKey === "iec958-stereo");
        assert.ok(analog && optical);
        const analogOutput = initial.availableOutputs.find((item) => item.nodeName === "fixture.output");
        assert.ok(analogOutput);
        const selected = await control({ command: "set-output", configuration: original,
          profileSelections: [{ card: analog.card, cardProfileName: optical.cardProfileName,
            outputKey: optical.outputKey, allowMissingForRestore: false }] });
        const opticalOutput = selected.availableOutputs.find((item) => item.nodeName === "fixture.output.iec958");
        assert.ok(opticalOutput);
        await control({ command: "set-output", configuration: original,
          profileSelections: [{ card: analog.card, cardProfileName: analog.cardProfileName,
            outputKey: analog.outputKey, allowMissingForRestore: false }] });
        const savedIntent = structuredClone(original);
        savedIntent.mode = "single";
        savedIntent.outputs.push({ id: "fixture-output", enabled: true, device: opticalOutput.device,
          profileIntent: { card: analog.card, outputKey: optical.outputKey } });
        savedIntent.channels.push({ outputId: "fixture-output", deviceChannel: 0, label: "" },
          { outputId: "fixture-output", deviceChannel: 1, label: "" });
        await control({ command: "set-output", configuration: savedIntent, preset: null });
        terminate(profileFixture, "SIGTERM");
        await profileFixture.completion;
        profileFixture = start("overridden product profile fixture", pulseServer, [], {
          ...environment, PIPETUNE_PROFILE_FIXTURE: "1", PIPETUNE_PROFILE_RECREATE: "1",
          ...(scenario === "product-output-profile-restore-failure" ? {
            PIPETUNE_PROFILE_OVERRIDE_SIGNAL: "1", PIPETUNE_PROFILE_INITIAL_INPUT_ONLY: "1",
          } : scenario === "product-output-profile-settle-input" ? {
            PIPETUNE_PROFILE_OVERRIDE_INPUT_SIGNAL: "1", PIPETUNE_PROFILE_INITIAL_INPUT_ONLY: "1",
            PIPETUNE_PROFILE_DEFER_INPUT_NOTIFICATION: "1",
          } : { PIPETUNE_PROFILE_OMIT_OPTICAL: "1" }),
        });
        if (scenario === "product-output-profile-unavailable") {
          const subscription = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
          const deadline = setTimeout(() => subscription.destroy(new Error("missing profile diagnosis did not arrive")), 10000);
          const lines = createInterface({ input: subscription });
          let missing;
          try {
            await once(subscription, "connect");
            subscription.write(`${JSON.stringify({ command: "subscribe" })}\n`);
            for await (const line of lines) {
              const status = JSON.parse(line);
              if (status.availableOutputProfiles.some((item) => item.card.api === "pipetune-fixture" && item.active) &&
                  status.outputProfileRestorationError) { missing = status; break; }
            }
          } finally {
            clearTimeout(deadline);
            lines.close();
            subscription.destroy();
          }
          assert.match(missing?.outputProfileRestorationError ?? "", /saved output profile.*unavailable/u);
          assert.equal(missing.outputProfileRestoring, false);
          assert.deepEqual(missing.outputConfiguration, savedIntent);
          assert.doesNotMatch(profileFixture.stdout, /profile-set index=1 save=0/u);
          const corrected = structuredClone(savedIntent);
          corrected.outputs.at(-1).device = analogOutput.device;
          corrected.outputs.at(-1).profileIntent.outputKey = analog.outputKey;
          const applied = await control({ command: "set-output", configuration: corrected,
            profileSelections: [{ card: analog.card, cardProfileName: analog.cardProfileName,
              outputKey: analog.outputKey, allowMissingForRestore: false }] });
          assert.deepEqual(applied.outputConfiguration, corrected);
          assert.equal(applied.outputProfileRestorationError, null);
          terminate(profileFixture, "SIGTERM");
          await profileFixture.completion;
          profileFixture = start("profile available after correction fixture", pulseServer, [], {
            ...environment, PIPETUNE_PROFILE_FIXTURE: "1", PIPETUNE_PROFILE_RECREATE: "1",
          });
          const latest = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
          const latestDeadline = setTimeout(() => latest.destroy(new Error("corrected profile status did not arrive")), 10000);
          const latestLines = createInterface({ input: latest });
          let retained;
          try {
            await once(latest, "connect");
            latest.write(`${JSON.stringify({ command: "subscribe" })}\n`);
            for await (const line of latestLines) {
              const status = JSON.parse(line);
              if (status.availableOutputProfiles.some((item) => item.card.api === "pipetune-fixture" && item.active) &&
                  !status.outputProfileRestoring) { retained = status; break; }
            }
          } finally {
            clearTimeout(latestDeadline);
            latestLines.close();
            latest.destroy();
          }
          assert.equal(retained.availableOutputProfiles.find((item) =>
            item.card.api === "pipetune-fixture" && item.active)?.cardProfileName, analog.cardProfileName);
          assert.deepEqual(retained.outputConfiguration, corrected);
          assert.doesNotMatch(profileFixture.stdout, /profile-set index=1 save=0/u);
        } else if (scenario === "product-output-profile-settle-input") {
          // Hold the Route notification until a public status round trip has
          // observed the old snapshot. A fixture write alone is not an ack.
          await new Promise((resolve, reject) => {
            const changed = () => { if (profileFixture.stdout.includes("input-notification-pending")) finish(); };
            const timeout = setTimeout(() => finish(new Error("input notification was not deferred")), 5000);
            const finish = (error) => {
              clearTimeout(timeout);
              profileFixture.child.stdout.off("data", changed);
              error ? reject(error) : resolve();
            };
            profileFixture.child.stdout.on("data", changed);
            changed();
          });
          const beforeNotification = await control({ command: "status" });
          assert.equal(beforeNotification.outputProfileRestoring, true);
          assert.ok(!beforeNotification.availableOutputProfiles.some((item) =>
            item.description === "Alternative stereo duplex (input restored)"));
          profileFixture.child.kill("SIGCONT");
          const subscription = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
          const deadline = setTimeout(() => subscription.destroy(new Error("input settling diagnosis did not arrive")), 10000);
          const lines = createInterface({ input: subscription });
          let latestStatus;
          let overridden = false;
          let finished;
          const changeInputAfterRestoration = () => {
            if (overridden || !latestStatus?.outputProfileRestoring) return;
            const active = latestStatus.availableOutputProfiles.find((item) => item.card.api === "pipetune-fixture" &&
              item.active && item.cardProfileName === optical.cardProfileName);
            const ready = latestStatus.availableOutputs.some((item) => item.nodeName === "fixture.output.iec958");
            // This description is published with the restored Route parameters.
            // Wait for the product's inventory, rather than the fixture's write.
            const restoredInput = active?.description === "Alternative stereo duplex (input restored)";
            if (active && ready && restoredInput) {
              profileFixture.child.kill("SIGURG");
              overridden = true;
            }
          };
          try {
            await once(subscription, "connect");
            subscription.write(`${JSON.stringify({ command: "subscribe" })}\n`);
            for await (const line of lines) {
              const status = JSON.parse(line);
              latestStatus = status;
              changeInputAfterRestoration();
              if (overridden && !status.outputProfileRestoring) { finished = status; break; }
            }
          } finally {
            clearTimeout(deadline);
            lines.close();
            subscription.destroy();
          }
          assert.equal(overridden, true);
          assert.match(profileFixture.stdout, /input-props-overridden profile=1 route=2/u);
          assert.match(finished?.outputProfileRestorationError ?? "", /input settings.*settling/u);
          assert.equal(finished.availableOutputProfiles.find((item) =>
            item.card.api === "pipetune-fixture" && item.active)?.cardProfileName, "input:analog-stereo");
          assert.deepEqual(finished.outputConfiguration, savedIntent);
        } else {
          const subscription = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
          const deadline = setTimeout(() => subscription.destroy(new Error("profile rollback diagnosis did not arrive")), 20000);
          const lines = createInterface({ input: subscription });
          let overrides = 0;
          let lastOverriddenSerial = null;
          let failed;
          let latestStatus;
          const observedProfiles = [];
          const overrideObservedProfile = () => {
            if (!latestStatus?.outputProfileRestoring || overrides >= 3) return;
            const targetActive = latestStatus.availableOutputProfiles.some((item) => item.card.api === "pipetune-fixture" &&
              item.active && item.cardProfileName === optical.cardProfileName);
            const output = latestStatus.availableOutputs.find((item) => item.nodeName === "fixture.output.iec958");
            const observation = JSON.stringify({ targetActive, serial: output?.nodeSerial, overrides });
            if (observedProfiles.at(-1) !== observation) observedProfiles.push(observation);
            // Each retry must expose its own output generation to the product
            // before the next external profile operation is injected.
            if (targetActive && output && output.nodeSerial !== lastOverriddenSerial) {
              profileFixture.child.kill("SIGUSR2");
              ++overrides;
              lastOverriddenSerial = output.nodeSerial;
            }
          };
          try {
            await once(subscription, "connect");
            subscription.write(`${JSON.stringify({ command: "subscribe" })}\n`);
            for await (const line of lines) {
              const status = JSON.parse(line);
              latestStatus = status;
              overrideObservedProfile();
              if (status.outputProfileRestorationError) { failed = status; break; }
            }
          } finally {
            clearTimeout(deadline);
            lines.close();
            subscription.destroy();
          }
          assert.ok(overrides >= 2);
          // The normal policy can replace the last target before its Profile
          // notification reaches the product. That bounded failure is a
          // timeout, whereas the isolated case must diagnose observed overrides.
          const expectedFailure = regularRoutePolicy ?
            /repeatedly overridden|timed out while restoring the saved output profile/u : /repeatedly overridden/u;
          assert.match(failed?.outputProfileRestorationError ?? "", expectedFailure,
            observedProfiles.join("\n"));
          assert.equal((profileFixture.stdout.match(/profile-set index=1 save=0/gu) ?? []).length, 3);
          assert.deepEqual(failed.outputConfiguration, savedIntent);
          assert.equal(failed.availableOutputProfiles.find((item) => item.card.api === "pipetune-fixture" &&
            item.active)?.cardProfileName, "input:analog-stereo");
          profileFixture.child.kill("SIGWINCH");
          await new Promise((resolve, reject) => {
            const changed = () => {
              if (profileFixture.stdout.includes("fixture-state profile=")) finish();
            };
            const timer = setTimeout(() => finish(new Error("fixture input state was not reported")), 5000);
            const finish = (error) => {
              clearTimeout(timer);
              profileFixture.child.stdout.off("data", changed);
              error ? reject(error) : resolve();
            };
            profileFixture.child.stdout.on("data", changed);
            changed();
          });
          const originalInput = profileFixture.stdout.match(/input-before-target route=(\d+) mute=(\d+) volume=([\d.]+),([\d.]+)/u);
          assert.ok(originalInput);
          assert.ok(profileFixture.stdout.includes(`fixture-state profile=3 input-route=${originalInput[1]} ` +
            `mute=${originalInput[2]} volume=${originalInput[3]},${originalInput[4]}`));
        }
      } else if (scenario === "product-output-change") {
        await next(0);
        const waitForCardStatus = async (predicate) => {
          const subscription = createConnection(environment.PIPETUNE_PRODUCT_SOCKET);
          // Initial selection plus two bounded retries may take 15.3 seconds.
          let latest;
          const deadline = setTimeout(() => subscription.destroy(new Error(
            `output profile status did not settle: ${JSON.stringify(latest)}`)), 20000);
          const lines = createInterface({ input: subscription });
          let observed;
          try {
            await once(subscription, "connect");
            subscription.write(`${JSON.stringify({ command: "subscribe" })}\n`);
            for await (const line of lines) {
              const status = JSON.parse(line);
              latest = { restoring: status.outputProfileRestoring,
                error: status.outputProfileRestorationError,
                active: status.availableOutputProfiles.filter((item) => item.card.api === "pipetune-fixture" && item.active)
                  .map((item) => item.cardProfileName),
                outputs: status.availableOutputs.filter((item) => item.nodeName.startsWith("fixture."))
                  .map((item) => item.nodeName) };
              if (predicate(status)) { observed = status; break; }
            }
          } finally {
            clearTimeout(deadline);
            lines.close();
            subscription.destroy();
          }
          assert.ok(observed, "output profile status did not settle");
          return observed;
        };
        if (regularRoutePolicy) {
          const previousInput = await control({ command: "status" });
          const cardId = profileFixture.stdout.match(/card-bound=(\d+)/u)?.[1];
          assert.ok(cardId);
          const changed = spawnSync("pw-cli", ["set-param", cardId, "Route",
            "{ index = 2 device = 1 save = true props = { mute = true channelVolumes = [ 0.37 0.37 ] } }"],
          { env: environment, encoding: "utf8" });
          assert.equal(changed.status, 0, changed.stderr);
          // A non-default user setting must be observed by both the session
          // manager and the product before testing profile preservation.
          const path = join(state, "wireplumber", "default-routes");
          mkdirSync(dirname(path), { recursive: true });
          await new Promise((resolve, reject) => {
            const changed = () => {
              if (existsSync(path) && readFileSync(path, "utf8").split(/\r?\n/u).some((line) =>
                line.includes("fixture-input-mic") && line.includes("0.37"))) finish();
            };
            const observer = watch(dirname(path), changed);
            const deadline = setTimeout(() => finish(new Error("user input gain was not saved by WirePlumber")), 10000);
            const finish = (error) => {
              clearTimeout(deadline);
              observer.close();
              error ? reject(error) : resolve();
            };
            changed();
          });
          await waitForCardStatus((status) => status.outputInventoryRevision > previousInput.outputInventoryRevision &&
            status.availableOutputProfiles.some((item) => item.active && item.description === "Stereo duplex (saved input)"));
        }
        const initial = await control({ command: "status" });
        const original = initial.outputConfiguration;
        let revision = initial.configurationRevision;
        const cardProfile = initial.availableOutputProfiles.find((item) =>
          item.card.api === "pipetune-fixture" && item.active);
        const opticalProfile = initial.availableOutputProfiles.find((item) =>
          item.card.api === "pipetune-fixture" && item.outputKey === "iec958-stereo");
        assert.ok(cardProfile && opticalProfile);
        const opticalSelection = { card: cardProfile.card,
          cardProfileName: opticalProfile.cardProfileName, outputKey: opticalProfile.outputKey,
          allowMissingForRestore: false };
        const wrongLayout = structuredClone(original);
        const syntheticOutput = initial.availableOutputs.find((item) => item.nodeName === "fixture.output");
        assert.ok(syntheticOutput);
        wrongLayout.outputs.push({ id: "fixture-output", enabled: true,
          device: { ...syntheticOutput.device, profile: "unknown-profile" },
          profileIntent: { card: cardProfile.card, outputKey: opticalProfile.outputKey } });
        wrongLayout.channels.push({ outputId: "fixture-output", deviceChannel: 0, label: "" },
          { outputId: "fixture-output", deviceChannel: 1, label: "" });
        const mismatched = await control({ command: "set-output", configuration: wrongLayout,
          profileSelections: [opticalSelection], expectedOutputInventoryRevision: initial.outputInventoryRevision }, false);
        assert.match(mismatched.error, /layout differs/u);
        const afterMismatch = await control({ command: "status" });
        assert.deepEqual(afterMismatch.outputConfiguration, original);
        assert.equal(afterMismatch.configurationRevision, revision);
        assert.equal(afterMismatch.availableOutputProfiles.find((item) =>
          item.card.api === "pipetune-fixture" && item.active)?.cardProfileName, cardProfile.cardProfileName);
        assert.ok(afterMismatch.availableOutputProfiles.find((item) =>
          item.card.api === "pipetune-fixture" && item.outputKey === "iec958-stereo")?.observedOutputs.length);
        const changedProfile = await control({ command: "set-output", configuration: original,
          profileSelections: [opticalSelection], expectedRevision: revision,
          expectedOutputInventoryRevision: afterMismatch.outputInventoryRevision });
        assert.equal(changedProfile.configurationRevision, ++revision);
        assert.equal(changedProfile.availableOutputProfiles.find((item) =>
          item.card.api === "pipetune-fixture" && item.active)?.cardProfileName, opticalProfile.cardProfileName);
        assert.match(profileFixture.stdout, /profile-set index=1 save=0/u);
        assert.match(profileFixture.stdout, /profile-set index=0 save=0/u);
        assert.match(profileFixture.stdout, regularRoutePolicy
          ? /route-set device=1 index=2 mute=1 volume=0\.370000,0\.370000/u
          : /route-set device=1 index=2 mute=0 volume=1\.000000,1\.000000/u);
        const opticalOutput = changedProfile.availableOutputs.find((item) => item.nodeName === "fixture.output.iec958");
        assert.ok(opticalOutput);
        const opticalRouted = structuredClone(original);
        opticalRouted.outputs[1].device = opticalOutput.device;
        opticalRouted.outputs[1].profileIntent = { card: cardProfile.card, outputKey: opticalProfile.outputKey };
        const routed = await control({ command: "set-output", configuration: opticalRouted });
        assert.deepEqual(routed.outputConfiguration, opticalRouted);
        assert.equal(routed.configurationRevision, ++revision);
        // This probe observes the selected device directly. The link group
        // excludes it from the desktop's automatic capture-endpoint routing.
        for (const channel of ["FL", "FR"]) {
          const linked = spawnSync("pw-link", [`fixture.output.iec958:monitor_${channel}`,
            `fixture.optical.monitor:input_${channel}`], { env: environment, encoding: "utf8" });
          // A session manager may have connected this exact pair already.
          // Verify the graph edge rather than requiring our request to create it.
          const links = spawnSync("pw-link", ["-l"], { env: environment, encoding: "utf8" });
          assert.equal(links.status, 0, links.stderr);
          const source = `fixture.output.iec958:monitor_${channel}`;
          const target = `fixture.optical.monitor:input_${channel}`;
          const connections = links.stdout.split(/\r?\n/u);
          const start = connections.indexOf(source);
          const following = connections.slice(start + 1);
          const end = following.findIndex((line) => line !== "" && !/^\s/u.test(line));
          assert.ok(start >= 0 && following.slice(0, end < 0 ? undefined : end).some((line) =>
            line.trim() === `|-> ${target}`), linked.stderr + links.stdout);
        }
        await new Promise((resolve, reject) => {
          const changed = () => { if (profileFixture.stdout.includes("optical-pcm-frames=4096")) finish(); };
          const timeout = setTimeout(() => finish(new Error("no mapped stereo PCM reached the optical output; links=" +
            spawnSync("pw-link", ["-l"], { env: environment, encoding: "utf8" }).stdout)), 10000);
          const finish = (error) => {
            clearTimeout(timeout);
            profileFixture.child.stdout.off("data", changed);
            error ? reject(error) : resolve();
          };
          profileFixture.child.stdout.on("data", changed);
          changed();
        });
        const restoredRouted = await control({ command: "set-output", configuration: original });
        assert.equal(restoredRouted.configurationRevision, ++revision);
        const singleSaved = structuredClone(original);
        singleSaved.mode = "single";
        singleSaved.outputs.push({ id: "fixture-output", enabled: true, device: opticalOutput.device,
          profileIntent: { card: cardProfile.card, outputKey: opticalProfile.outputKey } });
        singleSaved.channels.push({ outputId: "fixture-output", deviceChannel: 0, label: "" },
          { outputId: "fixture-output", deviceChannel: 1, label: "" });
        const saved = await control({ command: "set-output", configuration: singleSaved, preset: null });
        assert.equal(saved.configurationRevision, ++revision);
        terminate(profileFixture, "SIGTERM");
        await profileFixture.completion;
        // Observe removal before reusing the card identity; the previous
        // optical profile must not satisfy the new device's restoration check.
        await waitForCardStatus((status) => !status.availableOutputProfiles.some((item) => item.card.api === "pipetune-fixture"));
        profileFixture = start("reconnected product profile fixture", pulseServer, [], {
          ...environment, PIPETUNE_PROFILE_FIXTURE: "1", PIPETUNE_PROFILE_RECREATE: "1",
          ...(regularRoutePolicy ? { PIPETUNE_PROFILE_INITIAL_SAVED_INPUT: "1" } : {}),
        });
        const restored = await waitForCardStatus((status) => !status.outputProfileRestoring &&
          status.availableOutputProfiles.some((item) => item.card.api === "pipetune-fixture" &&
            item.active && item.cardProfileName === opticalProfile.cardProfileName) &&
          status.availableOutputs.some((output) => output.nodeName === "fixture.output.iec958"));
        assert.deepEqual(restored.outputConfiguration, singleSaved);
        assert.notEqual(restored.availableOutputs.find((output) => output.nodeName === "fixture.output.iec958").nodeSerial,
          opticalOutput.nodeSerial, "a reconnected card must have a new output generation");
        const staleInventory = await control({ command: "set-output", configuration: singleSaved,
          profileSelections: [opticalSelection], expectedOutputInventoryRevision: saved.outputInventoryRevision }, false);
        assert.match(staleInventory.error, /inventory.*changed/iu);
        assert.deepEqual((await control({ command: "status" })).outputConfiguration, singleSaved);
        assert.match(profileFixture.stdout, /profile-set index=1 save=0/u);
        const resumed = await control({ command: "set-output", configuration: original, preset });
        assert.equal(resumed.configurationRevision, ++revision);
        terminate(profileFixture, "SIGTERM");
        await profileFixture.completion;
        await waitForCardStatus((status) => !status.availableOutputProfiles.some((item) => item.card.api === "pipetune-fixture"));
        const emptySingle = { mode: "single", outputs: [], channels: [] };
        const pendingCancel = await control({ command: "set-output", configuration: emptySingle, preset: null,
          profileSelections: [{ card: cardProfile.card, cardProfileName: opticalProfile.cardProfileName,
            outputKey: opticalProfile.outputKey, allowMissingForRestore: true }] });
        assert.deepEqual(pendingCancel.outputConfiguration, emptySingle);
        assert.equal(pendingCancel.configurationRevision, ++revision);
        profileFixture = start("cancel restoration profile fixture", pulseServer, [], {
          ...environment, PIPETUNE_PROFILE_FIXTURE: "1", PIPETUNE_PROFILE_RECREATE: "1",
          ...(regularRoutePolicy ? { PIPETUNE_PROFILE_INITIAL_SAVED_INPUT: "1" } : {}),
        });
        const cancelRestored = await waitForCardStatus((status) => !status.outputProfileRestoring &&
          status.availableOutputProfiles.some((item) => item.card.api === "pipetune-fixture" &&
            item.active && item.cardProfileName === opticalProfile.cardProfileName));
        assert.deepEqual(cancelRestored.outputConfiguration, emptySingle);
        assert.match(profileFixture.stdout, /profile-set index=1 save=0/u);
        if (regularRoutePolicy) {
          profileFixture.child.kill("SIGWINCH");
          await new Promise((resolve, reject) => {
            const changed = () => { if (profileFixture.stdout.includes("fixture-state profile=")) finish(); };
            const deadline = setTimeout(() => finish(new Error("restored input state was not reported")), 5000);
            const finish = (error) => {
              clearTimeout(deadline);
              profileFixture.child.stdout.off("data", changed);
              error ? reject(error) : resolve();
            };
            profileFixture.child.stdout.on("data", changed);
            changed();
          });
          assert.match(profileFixture.stdout,
            /fixture-state profile=1 input-route=2 mute=1 volume=0\.370000,0\.370000/u);
        }
        const restoredAfterCancel = await control({ command: "set-output", configuration: original, preset });
        assert.equal(restoredAfterCancel.configurationRevision, ++revision);
        const absentProfile = await control({ command: "set-output", configuration: original,
          profileSelections: [{ card: { api: "alsa", location: "missing-card", vendor: "", product: "", serial: "" },
            cardProfileName: "output:analog-stereo", outputKey: "analog-stereo", allowMissingForRestore: false }] }, false);
        assert.match(absentProfile.error, /card.*unavailable|unavailable.*card/u);
        assert.equal((await control({ command: "status" })).configurationRevision, revision);
        const setOutput = async (configuration) => {
          const status = await control({ command: "set-output", configuration });
          assert.deepEqual(status.outputConfiguration, configuration);
          assert.equal(status.inputChannelCount, 2);
          assert.equal(status.configurationRevision, ++revision);
        };
        const swapped = structuredClone(original);
        swapped.channels = [...swapped.channels.slice(2), ...swapped.channels.slice(0, 2)];
        await setOutput(swapped);
        audio.child.kill("SIGUSR2");
        await next(1);
        const stale = await control({ command: "set-output", configuration: original, expectedRevision: revision - 1 }, false);
        assert.match(stale.error, /configuration changed/);
        assert.deepEqual((await control({ command: "status" })).outputConfiguration, swapped);
        while (swapped.channels.length < 16) swapped.channels.push({ outputId: "", deviceChannel: 0, label: "Reserved" });
        await setOutput(swapped);
        audio.child.kill("SIGUSR2");
        await next(2);
        swapped.outputs[0].enabled = false;
        await setOutput(swapped);
        audio.child.kill("SIGUSR2");
        await next(3);
        swapped.channels[0].label = "サブ左";
        await setOutput(swapped);
        const failureProfileSelection = [{ card: cardProfile.card,
          cardProfileName: cardProfile.cardProfileName, outputKey: cardProfile.outputKey,
          allowMissingForRestore: false }];
        const failedConnection = await control({ command: "set-output", configuration: original, preset: null,
          profileSelections: failureProfileSelection }, false);
        assert.match(failedConnection.error, /cannot connect PipeWire stream/);
        await waitForMessage(runtime, "product:injected-output-failure");
        const restoredConnection = await control({ command: "status" });
        assert.deepEqual(restoredConnection.outputConfiguration, swapped);
        assert.equal(restoredConnection.preset, preset);
        assert.equal(restoredConnection.processingMode, "preset");
        assert.equal(restoredConnection.configurationRevision, revision);
        assert.equal(restoredConnection.availableOutputProfiles.find((item) =>
          item.card.api === "pipetune-fixture" && item.active)?.cardProfileName, opticalProfile.cardProfileName);
        audio.child.kill("SIGUSR2");
        await next(4);
        const invalid = structuredClone(swapped);
        invalid.channels[1] = invalid.channels[0];
        await control({ command: "set-output", configuration: invalid }, false);
        const retained = await control({ command: "status" });
        assert.deepEqual(retained.outputConfiguration, swapped);
        assert.equal(retained.configurationRevision, revision);
        const failedNegotiation = await control({ command: "set-output", configuration: original, preset: null,
          profileSelections: failureProfileSelection }, false);
        assert.match(failedNegotiation.error, /injected output negotiation error/);
        await waitForMessage(runtime, "product:injected-output-negotiation-error");
        const rolledBack = await control({ command: "status" });
        assert.deepEqual(rolledBack.outputConfiguration, swapped);
        assert.equal(rolledBack.configurationRevision, revision);
        assert.equal(rolledBack.inputChannelCount, 2);
        assert.equal(rolledBack.preset, preset);
        assert.equal(rolledBack.processingMode, "preset");
        assert.equal(rolledBack.availableOutputProfiles.find((item) =>
          item.card.api === "pipetune-fixture" && item.active)?.cardProfileName, opticalProfile.cardProfileName);
        audio.child.kill("SIGUSR2");
        await next(5);
        await setOutput(original);
        audio.child.kill("SIGUSR2");
        await next(6);
        const bypass = await control({ command: "set-output", configuration: { ...original, mode: "single" }, preset: null });
        assert.equal(bypass.outputConfiguration.mode, "single");
        assert.equal(bypass.processingMode, "bypass");
        assert.equal(bypass.preset, null);
        assert.equal(bypass.configurationRevision, ++revision);
        audio.child.kill("SIGUSR2");
        await next(7);
        const stereoOnly = join(directory, "stereo-only.effetune_preset");
        writeFileSync(stereoOnly, JSON.stringify({ pipeline: [{ name: "Bass Extender", channel: "All" }] }));
        const stereoStatus = await control({ command: "load", preset: stereoOnly });
        revision = stereoStatus.configurationRevision;
        const rejected = await control({ command: "set-output", configuration: original }, false);
        assert.match(rejected.error, /Bass Extender/);
        const afterRejection = await control({ command: "status" });
        assert.equal(afterRejection.configurationRevision, revision);
        assert.deepEqual(afterRejection.outputConfiguration, { ...original, mode: "single" });
        assert.equal(afterRejection.preset, stereoOnly);
        const missingPreset = await control({ command: "set-output", configuration: original, preset: join(directory, "missing.effetune_preset") }, false);
        assert.ok(missingPreset.error.length > 0);
        assert.equal((await control({ command: "status" })).preset, stereoOnly);
        // A joint restoration must prepare the requested preset at four
        // channels, without first expanding the active stereo-only DSP.
        const combined = await control({ command: "set-output", configuration: original, preset, expectedRevision: revision });
        assert.deepEqual(combined.outputConfiguration, original);
        assert.equal(combined.preset, preset);
        assert.equal(combined.processingMode, "preset");
        assert.equal(combined.configurationRevision, ++revision);
        const unchanged = await control({ command: "set-output", configuration: original });
        assert.equal(unchanged.configurationRevision, revision);
        audio.child.kill("SIGUSR2");
      } else {
      const rate = (sampleRate) => control({ command: "set-rate", rateMode: "fixed", sampleRate, enforcement: graphClockTest ? "force" : "suggest" });
      const backend = (backend) => control({ command: "set-dsp-backend", backend, simdVariant: "auto" });
      await next(0);
      if (scenario === "product-controls") {
        const deadline = Date.now() + 2000;
        let peaks = [];
        while (Date.now() < deadline) {
          peaks = (await control({ command: "status" })).outputPeaks;
          if (peaks.length === 4 && peaks.every((peak) => peak > 0)) break;
          await new Promise((resolve) => setTimeout(resolve, 20));
        }
        assert.equal(peaks.length, 4, "the running multi-output stream must report four channels");
        assert.ok(peaks.every((peak) => peak > 0), "each active output channel must report PCM");
      }
      assert.equal((await rate(96000)).dspSampleRate, 96000);
      audio.child.kill("SIGUSR2");
      await next(1);
      assert.equal((await backend("simd")).effectiveDspBackend, "simd");
      audio.child.kill("SIGUSR2");
      await next(2);
      assert.equal((await control({ command: "bypass" })).processingMode, "bypass");
      audio.child.kill("SIGUSR2");
      await next(3);
      pipeline.push({ name: "DC Offset", enabled: true, channel: "All", parameters: { of: 0.125 } });
      writeFileSync(preset, JSON.stringify({ pipeline }));
      assert.equal((await control({ command: "load", preset })).activePluginCount, 2);
      audio.child.kill("SIGUSR2");
      await next(4);
      pipeline[1].parameters.of = 0.25;
      writeFileSync(`${preset}.new`, JSON.stringify({ pipeline }));
      renameSync(`${preset}.new`, preset);
      audio.child.kill("SIGUSR2");
      await next(5);
      const invalid = join(directory, "invalid.effetune_preset");
      writeFileSync(invalid, "{invalid");
      await control({ command: "load", preset: invalid }, false);
      assert.equal((await control({ command: "status" })).preset, preset);
      assert.equal((await control({ command: "set-dsp-idle", timeoutMilliseconds: 100 })).dspIdleTimeoutMilliseconds, 100);
      audio.child.kill("SIGUSR2");
      await next(6);
      assert.equal((await control({ command: "status" })).dspActivity, "sleeping");
      audio.child.kill("SIGUSR2");
      await next(7);
      assert.equal((await control({ command: "status" })).dspActivity, "active");
      assert.equal((await rate(48000)).dspSampleRate, 48000);
      assert.equal((await backend("scalar")).effectiveDspBackend, "scalar");
      audio.child.kill("SIGUSR2");
      if (graphClockTest) {
        await next(8);
        audio.child.kill("SIGUSR2");
      }
      }
    }
    if (scenario.startsWith("product-restart")) {
      await waitForMessage(audio, "product:restart-ready");
      terminate(manager, "SIGTERM");
      await manager.completion;
      start("restarted WirePlumber", "dbus-run-session", ["--", wireplumber]);
      audio.child.kill("SIGUSR2");
    }
    const stopRuntime = async () => {
      runtime.child.kill("SIGTERM");
      const shutdownDeadline = setTimeout(() => terminate(runtime, "SIGKILL"), 5000);
      const stopped = await runtime.completion;
      clearTimeout(shutdownDeadline);
      assert.equal(stopped.code, 0, runtime.stderr);
    };
    if (fixedPcmTest) {
      // Freewheel keeps advancing while the runtime is connected. Finish its
      // shutdown before asking the capture loop to destroy its streams.
      if (["cancel", "runtime-exit"].includes(audioFailurePoint)) {
        await waitForMessage(audio, "product:control-needed");
        if (audioFailurePoint === "cancel" && !audio.stderr.includes("product:audio-failed")) audio.child.kill("SIGTERM");
      }
      if (audioFailurePoint !== "runtime-exit") await waitForMessage(audio, "product:pcm-finished");
      await stopRuntime();
      audio.child.kill("SIGUSR2");
    }
    const result = await audio.completion;
    assert.equal(result.code, expectedAudioFailure ? 1 : 0, `${audio.stdout}\n${audio.stderr}`);
    const report = JSON.parse(audio.stdout);
    if (regularRoutePolicy) {
      report.routePolicy = { version: version.stdout.trim(), fixtures: children
        .filter((child) => child.stdout.includes("card-bound="))
        .map((child) => ({ name: child.name, events: child.stdout.trim().split("\n") })) };
      for (const fixture of report.routePolicy.fixtures) {
        assert.ok(fixture.events.filter((line) => line.startsWith("profile-set")).length <= 24,
          "profile requests must remain bounded during external overrides and rollback");
        assert.ok(fixture.events.filter((line) => line.startsWith("route-set")).length <= 64,
          "route restoration must converge without repeated policy feedback");
      }
    }
    if (expectedAudioFailure) {
      assert.ok(audio.stderr.includes(expectedAudioFailure), audio.stderr);
      assert.equal(report.shutdownAcknowledged, true, "failed capture must wait for the producer to stop");
    } else {
      if (scenario === "product-matrix-freewheel-delayed-observer")
        assert.ok(report.deferredFrames >= 2048, "the observer must fall behind by eight graph quanta without losing PCM");
      if (onePcmOutput) assert.equal(report.compensationFrames[0], -1);
      else if (fixedPcmTest)
        assert.ok(report.compensationFrames[0] >= 62 && report.compensationFrames[0] <= 63,
          "Matrix-only freewheel must verify tagged PCM and the declared output delay");
      assert.ok(report.receivedFrames.every((frames, device) => scenario === "product-disconnected" || (onePcmOutput && device === 1) ? frames === 0 : frames >= 32768));
      assert.ok(report.producedFrames >= 32768);
      assert.equal(report.stages.length, scenario === "product-output-recovery" ? 12 : graphClockTest || ["product-controls", "product-output-change"].includes(scenario) ? 9 : scenario === "product-profile" ? 5 : ["product-restart", "product-latency", "product-latency-units", "product-latency-reconnect", "product-time-alignment", "product-output-timeout"].includes(scenario) ? 3 :
        ["product-volume", "product-reconnect", "product-restart-mute"].includes(scenario) ? 4 : 1);
      assert.ok(report.stages.every((stage, index) => stage.every((frames, device) =>
        scenario === "product-disconnected" || (onePcmOutput && device === 1) || (scenario === "product-output-change" && index === 7) || (scenario === "product-latency-reconnect" && index === 1 && device === 1) ||
        (scenario === "product-output-recovery" && ([2,9,10].includes(index) || ([1,3,7].includes(index) && device === 0))) ||
        ((["product-reconnect", "product-profile"].includes(scenario) && index === 2 || scenario === "product-profile" && index === 3) && device === 0) ? frames === 0 : frames >= 32768)));
      if (scenario === "product-latency-reconnect") {
        assert.equal(report.compensationFrames[1], -1, "a missing output has no measured relative delay");
        for (const index of [0, 2]) assert.ok(report.compensationFrames[index] >= 62 && report.compensationFrames[index] <= 63);
      }
      if (scenario === "product-output-change") assert.ok(report.singleModeFrames >= 32768);
      if (scenario === "product-output-recovery") assert.ok(report.singleModeFrames >= 32768);
      if (graphClockTest) {
        assert.deepEqual(report.graphRates, [graphRate, ...Array(7).fill(96000), 48000]);
        assert.deepEqual(report.graphQuanta, [graphQuantum, ...Array(7).fill(graphQuantum * 2), graphQuantum]);
      }
      if (scenario === "product-reconnect") {
        assert.ok(report.clockGenerations.every((serial) => serial > 0));
        assert.notEqual(report.clockGenerations[0], report.clockGenerations[1]);
      }
      if (["product-latency", "product-latency-units", "product-time-alignment", "product-sfz-classic", "product-sfz-event"].includes(scenario)) {
        for (const [index, declared] of (scenario.startsWith("product-sfz-") ? [63] : [63, 127, 31]).entries())
          assert.ok(report.compensationFrames[index] <= declared && declared - report.compensationFrames[index] <= 1,
            "automatic output compensation must preserve the preset's intentional channel delay");
      }
    }
    let shutdownControl;
    if (scenario === "product-output-profile-shutdown") {
      const observedFixtureMessage = async (message) => {
        await new Promise((resolve, reject) => {
          const changed = () => { if (profileFixture.stdout.includes(message)) finish(); };
          const timeout = setTimeout(() => finish(new Error(`fixture did not acknowledge ${message}`)), 10000);
          const finish = (error) => {
            clearTimeout(timeout);
            profileFixture.child.stdout.off("data", changed);
            error ? reject(error) : resolve();
          };
          profileFixture.child.stdout.on("data", changed);
          changed();
        });
      };
      const statusClient = start("profile shutdown status", driver,
        ["control", environment.PIPETUNE_PRODUCT_SOCKET, JSON.stringify({ command: "status" })]);
      assert.equal((await statusClient.completion).code, 0, statusClient.stderr);
      const status = JSON.parse(statusClient.stdout);
      const optical = status.availableOutputProfiles.find((profile) => profile.card.api === "pipetune-fixture" &&
        profile.outputKey === "iec958-stereo" && profile.preservesInput);
      assert.ok(optical && !optical.active);
      profileFixture.child.kill("SIGUSR1");
      await observedFixtureMessage("output-node-hold-ready");
      shutdownControl = start("profile shutdown request", driver,
        ["control", environment.PIPETUNE_PRODUCT_SOCKET, JSON.stringify({ command: "set-output",
          configuration: status.outputConfiguration, profileSelections: [{ card: optical.card,
            cardProfileName: optical.cardProfileName, outputKey: optical.outputKey, allowMissingForRestore: false }] })]);
      // The fixture has accepted the profile but deliberately cannot supply its
      // output Node. Shutdown must release this pending request without success.
      await observedFixtureMessage("output-node-held=1");
      assert.equal(shutdownControl.result, undefined);
    }
    if (!fixedPcmTest) await stopRuntime();
    if (shutdownControl) {
      await shutdownControl.completion;
      if (shutdownControl.stdout.trim()) assert.equal(JSON.parse(shutdownControl.stdout).ok, false);
    }
    process.stdout.write(`${JSON.stringify(report)}\n`);
  } else {
  const probe = start("output probe", driver, [scenario]);
  if (scenario.startsWith("policy-pulse")) {
    await waitForMessage(probe, "pipetune-probe:pulse-ready");
    const pulse = start("PulseAudio client probe", pulseDriver, []);
    await waitForMessage(pulse, "pulse-probe:outputs-hidden");
    if (scenario === "policy-pulse-volume") {
      for (const [index, phase] of ["attenuate", "mute", "restore"].entries()) {
        await waitForMessage(probe, `pipetune-probe:volume-${phase}`);
        pulse.child.kill("SIGUSR1");
        await waitForMessage(pulse, `pulse-probe:volume-state-${index + 1}`);
      }
    }
    await waitForMessage(probe, "pipetune-probe:outputs-hidden");
    probe.child.kill("SIGUSR1");
    await waitForMessage(probe, "pipetune-probe:outputs-restored");
    await waitForMessage(pulse, "pulse-probe:outputs-restored");
    const pulseResult = await pulse.completion;
    assert.equal(pulseResult.code, 0, `${pulse.stdout}\n${pulse.stderr}`);
    const pulseReport = JSON.parse(pulse.stdout);
    assert.equal(pulseReport.outputsHidden, true);
    assert.equal(pulseReport.outputsRestored, true);
    assert.equal(pulseReport.clients, 2);
    assert.equal(pulseReport.volumeStatesObserved, scenario === "policy-pulse-volume" ? 3 : 0);
    assert.ok(pulseReport.producedFrames >= 65536);
    probe.child.kill("SIGUSR1");
    process.stdout.write(`${JSON.stringify(pulseReport)}\n`);
  }
  if (scenario === "policy-recovery" || scenario === "policy-defaults-recovery" || scenario.startsWith("policy-restart")) {
    await waitForMessage(probe, "pipetune-probe:outputs-hidden");
    terminate(manager, "SIGKILL");
    await manager.completion;
    await waitForMessage(probe, scenario.endsWith("recovery") ?
      "pipetune-probe:aggregate-closed" : "pipetune-probe:policy-stopped");
    start("restarted WirePlumber", "dbus-run-session", ["--", wireplumber]);
  }
  const result = await probe.completion;
  assert.equal(result.code, 0, `${probe.stdout}\n${probe.stderr}`);
  const report = JSON.parse(probe.stdout);
  assert.equal(report.success, true);
  assert.equal(report.channels, 4);
  assert.ok(report.receivedFrames[0] >= 65536, "device A must receive DSP channels 1 and 2");
  assert.ok(report.receivedFrames[1] >= 65536, "device B must receive DSP channels 3 and 4");
  assert.equal(report.receivedFrames[2], 0, "an unselected output must receive no signal");
  assert.equal(report.channelErrors, 0, "received PCM must preserve channel identity and sample order");
  if (scenario.startsWith("policy")) {
    assert.equal(report.inputChannels, 2, "desktop input must remain stereo");
    assert.ok(report.processedFrames >= 65536, "default playback must pass through the processor");
    assert.equal(typeof report.applicationClientId, "number");
    assert.equal(typeof report.processorClientId, "number");
    assert.notEqual(report.applicationClientId, report.processorClientId,
      "playback must cross the permission boundary from a separate application client");
    assert.equal(report.captureFeedsPlayback, false,
      "a capture endpoint must never feed a playback device");
  }
  if (["policy-visibility", "policy-dual-sink", "policy-recovery", "policy-pulse", "policy-pulse-volume"].includes(scenario) ||
      scenario.startsWith("policy-restart") || scenario.startsWith("policy-defaults")) {
    assert.equal(report.outputsHiddenDuringPlayback, true,
      "physical outputs must be hidden from selectors and playback clients during multi-device playback");
    assert.equal(report.outputsRestored, true,
      "physical outputs must become visible again when the combined output closes");
  }
  if (scenario.startsWith("policy-defaults") || scenario === "policy-dual-sink") {
    assert.equal(report.configuredDefaultRestored, true,
      "the exact configured default must survive even when that device is unavailable");
  }
  if (scenario === "policy-recovery" || scenario === "policy-defaults-recovery" || scenario.startsWith("policy-restart")) {
    assert.equal(report.metadataInstances, 2,
      "restoration must occur after WirePlumber restarts");
  }
  if (scenario.startsWith("policy-restart")) {
    for (const frames of report.framesAfterRestart) {
      assert.ok(frames >= 65536, "each selected output must resume its DSP channels after a policy restart");
    }
    assert.equal(report.framesAfterRestart.length, 2);
    if (scenario !== "policy-restart") {
      assert.equal(report.controlsRestoredAfterRestart, true,
        "the recreated logical output must expose the previous master volume and mute state");
      for (const output of report.volumeFrames) {
        assert.ok(output.attenuated >= 8192, "master volume must be applied before restarting");
        if (scenario === "policy-restart-mute") assert.ok(output.muted >= 8192);
      }
    }
    if (scenario === "policy-restart-mute") {
      for (const frames of report.mutedFramesAfterRestart) {
        assert.ok(frames >= 8192, "each selected output must remain muted after a policy restart");
      }
      assert.equal(report.mutedFramesAfterRestart.length, 2);
    }
  }
  if (scenario === "reconnect" || scenario === "policy-reconnect") {
    assert.equal(report.reconnected, true, "a reconnected device must receive its original DSP channels");
    assert.ok(report.survivorFramesWhileDisconnected >= 8192,
      "the remaining device must continue receiving its original DSP channels");
    assert.ok(report.receivedFrames[0] >= 131072, "device A must receive PCM before and after reconnecting");
    assert.ok(report.receivedFrames[1] >= 131072, "device B must remain routed while device A reconnects");
    if (scenario === "policy-reconnect") {
      assert.ok(report.reconnectMutedFrames >= 256,
        "the reconnecting output must pass silence through all linked channels before playback resumes");
    }
  }
  if (scenario === "volume" || scenario === "policy-volume" || scenario === "policy-pulse-volume") {
    if (scenario === "policy-pulse-volume" || scenario === "policy-volume") {
      assert.ok(report.unmutedInputFramesDuringMute >= 8192,
        "master mute must preserve the DSP input while silencing the physical outputs");
    }
    for (const output of report.volumeFrames) {
      assert.ok(output.attenuated >= 8192, "master volume must attenuate every selected output");
      assert.ok(output.muted >= 8192, "master mute must silence every selected output");
      assert.ok(output.restored >= 8192, "unmuting must restore every selected output");
    }
    assert.equal(report.volumeFrames.length, 2);
  }
  if (scenario === "latency" || scenario === "latency-off") {
    assert.equal(report.declaredLatencyFrames, 63);
    assert.equal(report.reportedDelayFrames[1] - report.reportedDelayFrames[0], 63,
      "public output-port latency must expose the downstream device delay difference");
    assert.equal(report.observedCompensationFrames, scenario === "latency" ? 63 : 0,
      "PCM timing must reflect whether compensation is enabled");
  }
  if (scenario === "latency-change" || scenario === "latency-reconnect") {
    assert.deepEqual(report.latencyStages.map((stage) => stage.declaredFrames),
      scenario === "latency-change" ? [63, 127, 31] : [63, 0, 63]);
    for (const [index, stage] of report.latencyStages.entries()) {
      assert.ok(stage.compensationFrames <= stage.declaredFrames &&
        stage.declaredFrames - stage.compensationFrames <= 1,
        "compensation must follow device latency within one frame of integer-time truncation");
      assert.equal(stage.steadyFrames.length, 2);
      assert.ok(stage.steadyFrames[0] >= 65536,
        "the faster output must preserve PCM order for a complete interval after each delay change");
      if (scenario === "latency-reconnect" && index === 1) {
        assert.equal(stage.steadyFrames[1], 0, "the disconnected output must receive no audio");
      } else {
        assert.ok(stage.steadyFrames[1] >= 65536,
          "the slower output must preserve PCM order before and after reconfiguration");
      }
    }
  }
  process.stdout.write(`${version.stdout.trim()}\n${JSON.stringify(report)}\n`);
  }
} catch (error) {
  testFailure = error;
  for (const child of children) process.stderr.write(`${child.name} (${JSON.stringify(child.result)}):\n${child.stdout}${child.stderr}\n`);
  throw error;
} finally {
  const forced = [];
  for (const record of [...children].reverse()) {
    if (record.result === undefined) {
      if (fixedPcmTest && record.name === "product audio fixture") record.child.kill("SIGUSR2");
      else terminate(record, "SIGTERM");
    }
    const timeout = setTimeout(() => {
      forced.push(new Error(`${record.name} required SIGKILL during cleanup`));
      terminate(record, "SIGKILL");
    }, 5000);
    await record.completion;
    clearTimeout(timeout);
  }
  rmSync(directory, { recursive: true, force: true });
  if (forced.length !== 0) throw new AggregateError(testFailure ? [testFailure, ...forced] : forced,
    "isolated audio cleanup did not finish normally");
}
