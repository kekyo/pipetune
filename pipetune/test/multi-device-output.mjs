import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, renameSync, rmSync, statSync, watch, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { createConnection } from "node:net";
import { once } from "node:events";
import { createInterface } from "node:readline";

const [pipewire, wireplumber, driver, scenario = "channels", policyFixture,
  pulseServer, pulseDriver, productDaemon] = process.argv.slice(2);
assert.ok(pipewire && wireplumber && driver, "PipeWire, WirePlumber and probe paths are required");
const version = spawnSync(wireplumber, ["--version"], { encoding: "utf8" });
assert.equal(version.status, 0, version.stderr);
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
if (scenario.startsWith("policy-pulse")) {
  assert.ok(pulseServer && pulseDriver, "PulseAudio server and client probe paths are required");
  writeConfiguration("pipewire/pipewire-pulse.conf.d/99-pipetune-test.conf", `pulse.properties = {
    server.address = [ "unix:${join(runtime, "pulse-native")}" ]
  }
`);
}
if (/libwireplumber 0\.4\./u.test(version.stdout)) {
  // Disable hardware monitors before WirePlumber's 90-enable-all.lua runs.
  writeConfiguration("wireplumber/main.lua.d/60-pipetune-test.lua", `alsa_monitor.enabled = false
v4l2_monitor.enabled = false
libcamera_monitor.enabled = false
`);
  writeConfiguration("wireplumber/bluetooth.lua.d/60-pipetune-test.lua", `bluez_monitor.enabled = false
bluez_midi_monitor.enabled = false
`);
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
  if (/libwireplumber 0\.4\./u.test(version.stdout)) {
    writeConfiguration("wireplumber/main.lua.d/60-pipetune-streams.lua", policy("stream-configuration"));
    writeConfiguration("wireplumber/scripts/pipetune-node-visibility.lua", policy("visibility"));
    writeConfiguration("wireplumber/policy.lua.d/60-pipetune-policy.lua", policy("configuration"));
    writeConfiguration("wireplumber/scripts/pipetune-endpoint-client.lua", policy("endpoint-client"));
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
const start = (name, executable, args) => {
  const child = spawn(executable, args, {
    env: environment,
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

try {
  start("PipeWire", pipewire, []);
  await socketReady(join(runtime, remote), undefined);
  if (scenario.startsWith("policy-pulse")) {
    start("PulseAudio protocol server", "dbus-run-session", ["--", pulseServer]);
    await socketReady(join(runtime, "pulse-native"), undefined);
  }
  const manager = start("WirePlumber", "dbus-run-session", ["--", wireplumber]);
  if (scenario.startsWith("product-output-status")) {
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
      assertPublicOutput();
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
      audio.child.kill("SIGUSR1");
      assert.equal((await audio.completion).code, 0, `${audio.stdout}\n${audio.stderr}`);
      const report = JSON.parse(audio.stdout);
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
        start("restarted WirePlumber", "dbus-run-session", ["--", wireplumber]);
      }
      process.stdout.write(`${JSON.stringify({ attempt, ...report })}\n`);
    }
  } else if (scenario.startsWith("product")) {
    const audio = start("product audio fixture", driver, ["audio", scenario]);
    await waitForMessage(audio, "product:devices-ready");
    const preset = join(directory, "matrix.effetune_preset");
    const pipeline = [
      { name: "Matrix", enabled: true, channel: "All", parameters: { mx: scenario === "product-slots" ? "001102p1304" : "001102p13" } },
    ];
    if (["product-volume", "product-reconnect", "product-profile", "product-restart", "product-restart-mute"].includes(scenario)) pipeline.push({ name: "DC Offset", enabled: true, channel: "All", parameters: { of: 0.125 } });
    if (scenario === "product-time-alignment") pipeline.push({ name: "Time Alignment", enabled: true, channel: "34", parameters: { dl: 1 } });
    writeFileSync(preset, JSON.stringify({ pipeline }));
    const runtime = start("product runtime", driver, ["runtime", scenario === "product-bypass" ? "bypass" : preset, scenario]);
    await waitForMessage(runtime, "product:runtime-ready");
    audio.child.kill("SIGUSR1");
    if (graphClockTest || ["product-controls", "product-output-change", "product-output-timeout", "product-output-clock-unavailable"].includes(scenario)) {
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
      if (scenario === "product-output-clock-unavailable") {
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
      } else if (scenario === "product-output-change") {
        await next(0);
        const initial = await control({ command: "status" });
        const original = initial.outputConfiguration;
        let revision = initial.configurationRevision;
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
        const failedConnection = await control({ command: "set-output", configuration: original, preset: null }, false);
        assert.match(failedConnection.error, /cannot connect PipeWire stream/);
        await waitForMessage(runtime, "product:injected-output-failure");
        const restoredConnection = await control({ command: "status" });
        assert.deepEqual(restoredConnection.outputConfiguration, swapped);
        assert.equal(restoredConnection.preset, preset);
        assert.equal(restoredConnection.processingMode, "preset");
        assert.equal(restoredConnection.configurationRevision, revision);
        audio.child.kill("SIGUSR2");
        await next(4);
        const invalid = structuredClone(swapped);
        invalid.channels[1] = invalid.channels[0];
        await control({ command: "set-output", configuration: invalid }, false);
        const retained = await control({ command: "status" });
        assert.deepEqual(retained.outputConfiguration, swapped);
        assert.equal(retained.configurationRevision, revision);
        const failedNegotiation = await control({ command: "set-output", configuration: original, preset: null }, false);
        assert.match(failedNegotiation.error, /injected output negotiation error/);
        await waitForMessage(runtime, "product:injected-output-negotiation-error");
        const rolledBack = await control({ command: "status" });
        assert.deepEqual(rolledBack.outputConfiguration, swapped);
        assert.equal(rolledBack.configurationRevision, revision);
        assert.equal(rolledBack.inputChannelCount, 2);
        assert.equal(rolledBack.preset, preset);
        assert.equal(rolledBack.processingMode, "preset");
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
    const result = await audio.completion;
    assert.equal(result.code, 0, `${audio.stdout}\n${audio.stderr}`);
    const report = JSON.parse(audio.stdout);
    assert.ok(report.receivedFrames.every((frames) => scenario === "product-disconnected" ? frames === 0 : frames >= 32768));
    assert.ok(report.producedFrames >= 32768);
    assert.equal(report.stages.length, graphClockTest || ["product-controls", "product-output-change"].includes(scenario) ? 9 : scenario === "product-profile" ? 5 : ["product-restart", "product-latency", "product-latency-units", "product-latency-reconnect", "product-time-alignment", "product-output-timeout"].includes(scenario) ? 3 :
      ["product-volume", "product-reconnect", "product-restart-mute"].includes(scenario) ? 4 : 1);
    assert.ok(report.stages.every((stage, index) => stage.every((frames, device) =>
      scenario === "product-disconnected" || (scenario === "product-output-change" && index === 7) || (scenario === "product-latency-reconnect" && index === 1 && device === 1) ||
      ((["product-reconnect", "product-profile"].includes(scenario) && index === 2 || scenario === "product-profile" && index === 3) && device === 0) ? frames === 0 : frames >= 32768)));
    if (scenario === "product-latency-reconnect") {
      assert.equal(report.compensationFrames[1], -1, "a missing output has no measured relative delay");
      for (const index of [0, 2]) assert.ok(report.compensationFrames[index] >= 62 && report.compensationFrames[index] <= 63);
    }
    if (scenario === "product-output-change") assert.ok(report.singleModeFrames >= 32768);
    if (graphClockTest) {
      assert.deepEqual(report.graphRates, [graphRate, ...Array(7).fill(96000), 48000]);
      assert.deepEqual(report.graphQuanta, [graphQuantum, ...Array(7).fill(graphQuantum * 2), graphQuantum]);
    }
    if (scenario === "product-reconnect") {
      assert.ok(report.clockGenerations.every((serial) => serial > 0));
      assert.notEqual(report.clockGenerations[0], report.clockGenerations[1]);
    }
    if (["product-latency", "product-latency-units", "product-time-alignment"].includes(scenario)) {
      for (const [index, declared] of [63, 127, 31].entries())
        assert.ok(report.compensationFrames[index] <= declared && declared - report.compensationFrames[index] <= 1,
          "automatic output compensation must preserve the preset's intentional channel delay");
    }
    runtime.child.kill("SIGTERM");
    assert.equal((await runtime.completion).code, 0, runtime.stderr);
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
  if (["policy-visibility", "policy-recovery", "policy-pulse", "policy-pulse-volume"].includes(scenario) ||
      scenario.startsWith("policy-restart") || scenario.startsWith("policy-defaults")) {
    assert.equal(report.outputsHiddenDuringPlayback, true,
      "physical outputs must be hidden from selectors and playback clients during multi-device playback");
    assert.equal(report.outputsRestored, true,
      "physical outputs must become visible again when the combined output closes");
  }
  if (scenario.startsWith("policy-defaults")) {
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
  for (const child of children) process.stderr.write(`${child.name} (${JSON.stringify(child.result)}):\n${child.stdout}${child.stderr}\n`);
  throw error;
} finally {
  for (const record of [...children].reverse()) {
    if (record.result === undefined) terminate(record, "SIGTERM");
    const timeout = setTimeout(() => terminate(record, "SIGKILL"), 5000);
    await record.completion;
    clearTimeout(timeout);
  }
  rmSync(directory, { recursive: true, force: true });
}
