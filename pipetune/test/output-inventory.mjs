import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, rmSync, watch, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [pipewire, pipetune, observer] = process.argv.slice(2);
assert.ok(pipewire && pipetune && observer);
const directory = mkdtempSync(join(tmpdir(), "pipetune-inventory-"));
const runtime = join(directory, "runtime");
const config = join(directory, "config");
mkdirSync(runtime, { mode: 0o700 });
mkdirSync(join(config, "pipewire", "pipewire.conf.d"), { recursive: true });
const remote = `inventory-${process.pid}`;
const environment = { ...process.env, XDG_RUNTIME_DIR: runtime,
  PIPEWIRE_RUNTIME_DIR: runtime, XDG_CONFIG_HOME: config,
  PIPEWIRE_CORE: remote, PIPEWIRE_REMOTE: remote };
const wideChannels = Array.from({ length: 18 }, (_, index) => `AUX${index}`);
const sink = (name, positions, properties = "") => `{ factory = adapter args = {
  factory.name = support.null-audio-sink node.name = ${name}
  node.description = ${JSON.stringify(`Output "${name}"`)}
  media.class = Audio/Sink audio.position = ${JSON.stringify(positions)}
  adapter.auto-port-config = { mode = dsp monitor = true position = preserve }
  ${properties}
} }`;
writeFileSync(join(config, "pipewire", "pipewire.conf.d", "99-inventory.conf"), `
context.properties = { core.name = ${remote} }
context.objects = [
  ${sink("inventory.alsa", ["FL", "FR"], `device.api = alsa device.bus-path = "usb:1"
    device.vendor.id = 0x1234 device.product.id = 0x5678 device.serial = Model
    alsa.card = 7 alsa.device = 2 alsa.subdevice = 0 node.device.profile.name = analog-stereo`)}
  ${sink("inventory.surround", ["FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"])}
  ${sink("inventory.wide", wideChannels)}
  ${sink("inventory.internal", ["FL", "FR"], "node.pipetune.internal = true")}
  ${sink("inventory.public", ["FL", "FR"], "node.pipetune.public-input = true")}
  ${sink("inventory.aggregate", ["FL", "FR"], "node.pipetune.aggregate = true")}
  ${sink("inventory.source", ["FL", "FR"]).replace("media.class = Audio/Sink", "media.class = Audio/Source")}
]
`);

const run = async (executable, arguments_, env = environment) => {
  const child = spawn(executable, arguments_, { env, stdio: ["ignore", "pipe", "pipe"] });
  let stdout = "";
  let stderr = "";
  child.stdout.on("data", (data) => { stdout += data; });
  child.stderr.on("data", (data) => { stderr += data; });
  const code = await new Promise((resolve, reject) => {
    child.on("error", reject);
    child.on("close", resolve);
  });
  return { code, stdout, stderr };
};

const server = spawn(pipewire, [], { env: environment, stdio: ["ignore", "ignore", "pipe"] });
let serverError = "";
server.stderr.on("data", (data) => { serverError += data; });
const serverClosed = new Promise((resolve) => { server.on("close", resolve); });
try {
  await new Promise((resolve, reject) => {
    const finish = (error) => {
      clearTimeout(timer);
      observer.close();
      server.off("error", finish);
      server.off("close", failed);
      if (error) reject(error);
      else resolve();
    };
    const failed = () => finish(new Error(serverError));
    const observer = watch(runtime, () => { if (existsSync(join(runtime, remote))) finish(); });
    const timer = setTimeout(() => finish(new Error("isolated PipeWire did not start")), 10000);
    server.once("error", finish);
    server.once("close", failed);
    if (existsSync(join(runtime, remote))) finish();
  });
  const result = await run(pipetune, ["output", "list", "--json"]);
  assert.equal(result.code, 0, result.stderr);
  const { outputs } = JSON.parse(result.stdout);
  assert.deepEqual(outputs.map((output) => output.nodeName).sort(),
    ["inventory.alsa", "inventory.surround", "inventory.wide"]);
  const alsa = outputs.find((output) => output.nodeName === "inventory.alsa");
  assert.deepEqual(alsa.identity, { api: "alsa", location: "usb:1", port: "pcm:2:0",
    vendor: "0x1234", product: "0x5678", serial: "Model" });
  assert.equal(alsa.name, 'Output "inventory.alsa"');
  assert.equal(alsa.profile, "analog-stereo");
  assert.deepEqual(alsa.channelPositions, ["FL", "FR"]);
  assert.equal(alsa.selectable, true);
  assert.ok(Number.isInteger(alsa.nodeId));
  const surround = outputs.find((output) => output.nodeName === "inventory.surround");
  assert.deepEqual(surround.channelPositions, ["FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"]);
  assert.equal(surround.identity.api, "node");
  assert.equal(surround.identity.location, "inventory.surround");
  const wide = outputs.find((output) => output.nodeName === "inventory.wide");
  assert.deepEqual(wide.channelPositions, wideChannels);
  assert.equal(wide.selectable, false);
  assert.match(wide.error, /sixteen|16/u);
  const plain = await run(pipetune, ["output", "list"]);
  assert.equal(plain.code, 0, plain.stderr);
  assert.match(plain.stdout, /1: FL, 2: FR/u);
  assert.match(plain.stdout, /18: AUX17/u);
  assert.match(plain.stdout, /analog-stereo/u);
  const missing = await run(pipetune, ["output", "list", "--json"],
    { ...environment, PIPEWIRE_REMOTE: "missing-session" });
  assert.notEqual(missing.code, 0, "connection errors must not become empty successful inventories");
  assert.match(missing.stderr, /PipeWire/u);
  const observed = await run(observer, []);
  assert.equal(observed.code, 0, observed.stderr);
  console.log("Output inventory: identity, profiles, full channel layouts, exclusions and connection failures passed");
} finally {
  server.kill("SIGTERM");
  await serverClosed;
  rmSync(directory, { recursive: true, force: true });
}
