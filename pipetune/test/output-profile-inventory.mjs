import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, rmSync, watch, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [pipewire, fixture, observer] = process.argv.slice(2);
assert.ok(pipewire && fixture && observer);
const directory = mkdtempSync(join(tmpdir(), "pipetune-output-profile-"));
const runtime = join(directory, "runtime");
const config = join(directory, "config");
mkdirSync(runtime, { mode: 0o700 });
mkdirSync(join(config, "pipewire", "pipewire.conf.d"), { recursive: true });
const remote = `profiles-${process.pid}`;
writeFileSync(join(config, "pipewire", "pipewire.conf.d", "99-profile.conf"),
  `context.properties = { core.name = ${remote} }\n`);
const environment = {
  ...process.env,
  XDG_RUNTIME_DIR: runtime,
  PIPEWIRE_RUNTIME_DIR: runtime,
  PIPEWIRE_REMOTE: remote,
  PIPEWIRE_CORE: remote,
  XDG_CONFIG_HOME: config,
};
const server = spawn(pipewire, ["-c", "pipewire.conf"], {
  env: { ...environment, PIPEWIRE_DEBUG: "0" },
  stdio: ["ignore", "ignore", "pipe"],
});
let serverError = "";
server.stderr.on("data", (data) => { serverError += data; });
const serverClosed = new Promise((resolve) => { server.on("close", resolve); });
let card;
try {
  await new Promise((resolve, reject) => {
    const target = join(runtime, remote);
    const observer = watch(runtime, () => { if (existsSync(target)) finish(); });
    const timeout = setTimeout(() => finish(new Error(`PipeWire did not start: ${serverError}`)), 10000);
    const finish = (error) => {
      clearTimeout(timeout);
      observer.close();
      error ? reject(error) : resolve();
    };
    if (existsSync(target)) finish();
  });
  card = spawn(fixture, [], { env: { ...environment, PIPETUNE_PROFILE_FIXTURE: "1" },
    stdio: ["ignore", "pipe", "pipe"] });
  let fixtureOutput = "";
  await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error(`Synthetic card did not bind: ${fixtureOutput}`)), 10000);
    card.stdout.on("data", (data) => {
      fixtureOutput += data;
      if (fixtureOutput.includes("card-bound=")) { clearTimeout(timeout); resolve(); }
    });
    card.once("error", reject);
  });
  const probe = spawn(observer, [], { env: environment, stdio: ["ignore", "pipe", "pipe"] });
  let error = "";
  probe.stderr.on("data", (data) => { error += data; });
  const code = await new Promise((resolve, reject) => {
    probe.once("error", reject);
    probe.once("close", resolve);
  });
  assert.equal(code, 0, error);
  assert.match(fixtureOutput, /profile-set index=1 save=0/u);
  assert.match(fixtureOutput, /profile-set index=3 save=0/u);
  assert.match(fixtureOutput, /profile-set index=0 save=0/u);
  assert.match(fixtureOutput, /route-set device=1 index=2 mute=0 volume=1\.000000,1\.000000/u);
} finally {
  if (card) card.kill("SIGTERM");
  server.kill("SIGTERM");
  await serverClosed;
  rmSync(directory, { recursive: true, force: true });
}
