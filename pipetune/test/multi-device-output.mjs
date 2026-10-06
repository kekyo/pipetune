import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, renameSync, rmSync, watch, writeFileSync } from "node:fs";
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
writeConfiguration("pipewire/pipewire.conf.d/99-pipetune-test.conf", `context.properties = {
  core.name = "${remote}"
  default.clock.rate = 48000
  default.clock.quantum = 256
  default.clock.min-quantum = 256
  default.clock.max-quantum = 256
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
const socketReady = async (socket) => {
  await new Promise((resolve, reject) => {
    const observer = watch(dirname(socket), () => { if (existsSync(socket)) finish(); });
    const timeout = setTimeout(() => finish(new Error(`isolated audio socket was not created: ${socket}`)), 15000);
    const finish = (error) => {
      clearTimeout(timeout);
      observer.close();
      if (error) reject(error);
      else resolve();
    };
    if (existsSync(socket)) finish();
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
  await socketReady(join(runtime, remote));
  if (scenario.startsWith("policy-pulse")) {
    start("PulseAudio protocol server", "dbus-run-session", ["--", pulseServer]);
    await socketReady(join(runtime, "pulse-native"));
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
  } else if (scenario === "product-daemon") {
    assert.ok(productDaemon, "the product daemon executable is required");
    environment.PIPETUNE_PRODUCT_INPUT = "pipetune_sink";
    const socket = join(runtime, "pipetune", "control.sock");
    mkdirSync(dirname(socket), { recursive: true });
    const preset = join(directory, "saved.effetune_preset");
    writeFileSync(preset, JSON.stringify({ pipeline: [
      { name: "Matrix", enabled: true, channel: "All", parameters: { mx: "001102p13" } },
    ] }));
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
    // Restart both the daemon and fixtures. The next run must recover saved
    // routing after the devices have been recreated as new runtime objects.
    for (let attempt = 0; attempt < 2; ++attempt) {
      const audio = start("daemon audio fixture", driver, ["audio", scenario]);
      await waitForMessage(audio, "product:devices-ready");
      const daemon = start("product daemon", productDaemon, ["daemon", "--config", join(config, "pipetune", "environment")]);
      await socketReady(socket);
      const status = start("daemon status", driver, ["control", socket, JSON.stringify({ command: "status" })]);
      assert.equal((await status.completion).code, 0, status.stderr);
      const response = JSON.parse(status.stdout);
      assert.equal(response.ok, true, status.stdout);
      assert.equal(response.processingMode, "preset", status.stdout);
      audio.child.kill("SIGUSR1");
      assert.equal((await audio.completion).code, 0, `${audio.stdout}\n${audio.stderr}`);
      const report = JSON.parse(audio.stdout);
      assert.ok(report.receivedFrames.every((frames) => frames >= 32768));
      terminate(daemon, "SIGTERM");
      assert.equal((await daemon.completion).code, 0, daemon.stderr);
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
    if (scenario === "product-controls") {
      const control = async (request, success = true) => {
        const client = start("product control", driver, ["control", environment.PIPETUNE_PRODUCT_SOCKET, JSON.stringify(request)]);
        const timeout = setTimeout(() => terminate(client, "SIGTERM"), 10000);
        const result = await client.completion;
        clearTimeout(timeout);
        assert.equal(result.code, 0, client.stderr);
        const response = JSON.parse(client.stdout);
        assert.equal(response.ok, success, client.stdout);
        return response;
      };
      const next = (stage) => waitForMessage(audio, `product:controls-stage-${stage}`);
      const rate = (sampleRate) => control({ command: "set-rate", rateMode: "fixed", sampleRate, enforcement: "suggest" });
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
    assert.equal(report.stages.length, scenario === "product-controls" ? 9 : scenario === "product-profile" ? 5 : ["product-restart", "product-latency", "product-latency-reconnect", "product-time-alignment"].includes(scenario) ? 3 :
      ["product-volume", "product-reconnect", "product-restart-mute"].includes(scenario) ? 4 : 1);
    assert.ok(report.stages.every((stage, index) => stage.every((frames, device) =>
      scenario === "product-disconnected" || (scenario === "product-latency-reconnect" && index === 1 && device === 1) ||
      ((["product-reconnect", "product-profile"].includes(scenario) && index === 2 || scenario === "product-profile" && index === 3) && device === 0) ? frames === 0 : frames >= 32768)));
    if (scenario === "product-latency-reconnect") {
      assert.equal(report.compensationFrames[1], -1, "a missing output has no measured relative delay");
      for (const index of [0, 2]) assert.ok(report.compensationFrames[index] >= 62 && report.compensationFrames[index] <= 63);
    }
    if (["product-latency", "product-time-alignment"].includes(scenario)) {
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
