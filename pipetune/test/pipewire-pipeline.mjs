import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, rmSync, watch, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [pipewire, wireplumber, driver] = process.argv.slice(2);
const directory = mkdtempSync(join(tmpdir(), "pipetune-pipeline-"));
const runtime = join(directory, "runtime");
const config = join(directory, "config");
const remote = `pipeline-${process.pid}`;
const environment = { ...process.env, XDG_RUNTIME_DIR: runtime, PIPEWIRE_RUNTIME_DIR: runtime,
  PIPEWIRE_CORE: remote, PIPEWIRE_REMOTE: remote, XDG_CONFIG_HOME: config,
  PIPETUNE_TEST_SILENCE_WAV: join(directory, "silence.wav"),
  XDG_STATE_HOME: join(directory, "state"), XDG_DATA_HOME: join(directory, "data"), GIO_USE_VFS: "local" };
mkdirSync(runtime, { mode: 0o700 });
// A WAV fixture works with both old and new pw-cat stdin-format conventions.
const silence = Buffer.alloc(44 + 96000 * 2 * 2 * 10);
silence.write("RIFF", 0); silence.writeUInt32LE(silence.length - 8, 4);
silence.write("WAVEfmt ", 8); silence.writeUInt32LE(16, 16);
silence.writeUInt16LE(1, 20); silence.writeUInt16LE(2, 22);
silence.writeUInt32LE(96000, 24); silence.writeUInt32LE(96000 * 4, 28);
silence.writeUInt16LE(4, 32); silence.writeUInt16LE(16, 34);
silence.write("data", 36); silence.writeUInt32LE(silence.length - 44, 40);
writeFileSync(environment.PIPETUNE_TEST_SILENCE_WAV, silence);
const configure = (path, text) => {
  const components = path.split("/");
  components.pop();
  mkdirSync(join(config, ...components), { recursive: true });
  writeFileSync(join(config, path), text);
};
configure("pipewire/pipewire.conf.d/99-pipeline-test.conf", `
context.properties = { core.name = "${remote}" }
context.objects = [ { factory = adapter args = {
  factory.name = support.null-audio-sink node.name = pipeline.default
  media.class = Audio/Sink audio.position = [ FL FR ]
  adapter.auto-port-config = { mode = dsp monitor = true position = preserve }
} } ]
`);
const version = spawnSync(wireplumber, ["--version"], { encoding: "utf8" });
assert.equal(version.status, 0, version.stderr);
if (/libwireplumber 0\.4\./u.test(version.stdout)) {
  configure("wireplumber/main.lua.d/60-pipeline-test.lua", "alsa_monitor.enabled = false\nv4l2_monitor.enabled = false\nlibcamera_monitor.enabled = false\n");
  configure("wireplumber/bluetooth.lua.d/60-pipeline-test.lua", "bluez_monitor.enabled = false\nbluez_midi_monitor.enabled = false\n");
} else {
  configure("wireplumber/wireplumber.conf.d/60-pipeline-test.conf", `wireplumber.profiles = {
    main = { monitor.alsa = disabled monitor.bluez = disabled monitor.bluez-midi = disabled
      monitor.v4l2 = disabled monitor.libcamera = disabled }
  }`);
}
const children = [];
const start = (executable, args) => {
  const child = spawn(executable, args, { env: environment, detached: true, stdio: ["ignore", "pipe", "pipe"] });
  const record = { child, stdout: "", stderr: "", completion: undefined };
  child.stdout.on("data", (chunk) => { record.stdout += chunk; });
  child.stderr.on("data", (chunk) => { record.stderr += chunk; });
  record.completion = new Promise((resolve, reject) => { child.on("error", reject); child.on("close", resolve); });
  children.push(record);
  return record;
};
const stop = (record, signal) => {
  if (record.child.pid === undefined) return;
  try { process.kill(-record.child.pid, signal); } catch (error) { if (error.code !== "ESRCH") throw error; }
};
try {
  const server = start("dbus-run-session", ["--", pipewire]);
  await new Promise((resolve, reject) => {
    const finish = (error) => { clearTimeout(timer); observer.close(); if (error) reject(error); else resolve(); };
    const changed = () => { if (existsSync(join(runtime, remote))) finish(); };
    const observer = watch(runtime, changed);
    const timer = setTimeout(() => finish(new Error(`PipeWire did not start: ${server.stderr}`)), 10000);
    changed();
  });
  start("dbus-run-session", ["--", wireplumber]);
  const test = start(driver, []);
  assert.equal(await test.completion, 0, test.stdout + test.stderr);
} finally {
  for (const record of children.toReversed()) {
    stop(record, "SIGTERM");
    const timer = setTimeout(() => stop(record, "SIGKILL"), 3000);
    try { await record.completion; } finally { clearTimeout(timer); }
  }
  rmSync(directory, { recursive: true, force: true });
}
