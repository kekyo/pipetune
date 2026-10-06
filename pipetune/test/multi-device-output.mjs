import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, rmSync, watch, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [pipewire, wireplumber, driver, scenario = "channels", policyFixture] = process.argv.slice(2);
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

if (scenario.startsWith("policy")) {
  assert.ok(policyFixture, "the PipeTune WirePlumber policy fixture is required");
  const policy = (part) => {
    const result = spawnSync(policyFixture, [part], { encoding: "utf8" });
    assert.equal(result.status, 0, result.stderr);
    return result.stdout;
  };
  if (/libwireplumber 0\.4\./u.test(version.stdout)) {
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
const socketReady = async () => {
  const socket = join(runtime, remote);
  await new Promise((resolve, reject) => {
    const observer = watch(runtime, () => { if (existsSync(socket)) finish(); });
    const timeout = setTimeout(() => finish(new Error("isolated PipeWire socket was not created")), 15000);
    const finish = (error) => {
      clearTimeout(timeout);
      observer.close();
      if (error) reject(error);
      else resolve();
    };
    if (existsSync(socket)) finish();
  });
};

try {
  start("PipeWire", pipewire, []);
  await socketReady();
  start("WirePlumber", "dbus-run-session", ["--", wireplumber]);
  const probe = start("output probe", driver, [scenario]);
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
  }
  if (scenario === "reconnect" || scenario === "policy-reconnect") {
    assert.equal(report.reconnected, true, "a reconnected device must receive its original DSP channels");
    assert.ok(report.survivorFramesWhileDisconnected >= 8192,
      "the remaining device must continue receiving its original DSP channels");
    assert.ok(report.receivedFrames[0] >= 131072, "device A must receive PCM before and after reconnecting");
    assert.ok(report.receivedFrames[1] >= 131072, "device B must remain routed while device A reconnects");
  }
  if (scenario === "volume" || scenario === "policy-volume") {
    for (const output of report.volumeFrames) {
      assert.ok(output.attenuated >= 8192, "master volume must attenuate every selected output");
      assert.ok(output.muted >= 8192, "master mute must silence every selected output");
      assert.ok(output.restored >= 8192, "unmuting must restore every selected output");
    }
    assert.equal(report.volumeFrames.length, 2);
  }
  if (scenario === "latency" || scenario === "latency-off") {
    assert.equal(report.declaredLatencyFrames, 63);
    assert.equal(report.observedCompensationFrames, scenario === "latency" ? 63 : 0,
      "PCM timing must reflect whether compensation is enabled");
  }
  process.stdout.write(`${version.stdout.trim()}\n${JSON.stringify(report)}\n`);
} catch (error) {
  for (const child of children) process.stderr.write(`${child.name}:\n${child.stdout}${child.stderr}\n`);
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
