import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  writeFileSync,
  readFileSync,
  watch,
  existsSync,
  copyFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { once } from "node:events";
import { createInterface } from "node:readline";

const [
  pipewire,
  wireplumber,
  fixturePath,
  modulePath,
  systemModulePath,
  pwDump,
  pwCli,
  routeProbe,
] = process.argv.slice(2);
assert.ok(
  pipewire &&
    wireplumber &&
    fixturePath &&
    modulePath &&
    systemModulePath &&
    pwDump &&
    pwCli &&
    routeProbe,
);
const directory = mkdtempSync(join(tmpdir(), "pipetune-presentation-"));
const config = join(directory, "config");
const runtime = join(directory, "runtime");
for (const path of [
  runtime,
  join(config, "pipewire/pipewire.conf.d"),
  join(directory, "state"),
  join(config, "wireplumber/main.lua.d"),
  join(config, "wireplumber/bluetooth.lua.d"),
])
  mkdirSync(path, { recursive: true, mode: 0o700 });
const useModule = !process.argv.includes("--without-module");
copyFileSync(pwDump, join(directory, "pipetune"));
mkdirSync(join(directory, "controller"));
copyFileSync(pwCli, join(directory, "controller/pipetune"));
writeFileSync(
  join(config, "pipewire/pipewire.conf.d/99-test.conf"),
  useModule
    ? "context.modules = [ { name = libpipewire-module-pipetune-presentation } ]\n"
    : "",
);
writeFileSync(
  join(config, "wireplumber/main.lua.d/60-test.lua"),
  "alsa_monitor.enabled=false\nv4l2_monitor.enabled=false\nlibcamera_monitor.enabled=false\n",
);
writeFileSync(
  join(config, "wireplumber/bluetooth.lua.d/60-test.lua"),
  "bluez_monitor.enabled=false\nbluez_midi_monitor.enabled=false\n",
);
const environment = {
  ...process.env,
  XDG_RUNTIME_DIR: runtime,
  PIPEWIRE_MODULE_DIR: resolve(modulePath) + ":" + systemModulePath,
  PIPEWIRE_RUNTIME_DIR: runtime,
  PIPEWIRE_REMOTE: "pipewire-0",
  PIPEWIRE_CORE: "pipewire-0",
  XDG_CONFIG_HOME: config,
  XDG_STATE_HOME: join(directory, "state"),
  PULSE_SERVER: `unix:${runtime}/pulse/native`,
};
const children = [];
const start = (name, command, args) => {
  const child = spawn(command, args, {
    env: environment,
    stdio: ["pipe", "pipe", "pipe"],
  });
  const record = { child, stdout: "", stderr: "" };
  children.push(record);
  child.stdout.on("data", (chunk) => {
    record.stdout += chunk;
  });
  child.stderr.on("data", (chunk) => {
    record.stderr += chunk;
  });
  child.on("close", () => {
    writeFileSync(join(directory, name + ".out"), record.stdout);
    writeFileSync(join(directory, name + ".err"), record.stderr);
  });
  return record;
};
const ready = async (path) => {
  if (existsSync(path)) return;
  await new Promise((resolveReady, reject) => {
    const observer = watch(runtime, { recursive: true }, () => {
      if (existsSync(path)) finish();
    });
    const timer = setTimeout(
      () => finish(new Error("Missing socket " + path)),
      10000,
    );
    const finish = (error) => {
      clearTimeout(timer);
      observer.close();
      error ? reject(error) : resolveReady();
    };
    if (existsSync(path)) finish();
  });
};
const run = async (command, args, props) => {
  const child = spawn(command, args, {
    env: { ...environment, ...props },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdout = "",
    stderr = "";
  child.stdout.on("data", (chunk) => (stdout += chunk));
  child.stderr.on("data", (chunk) => (stderr += chunk));
  const [code] = await once(child, "close");
  assert.equal(code, 0, stderr + stdout);
  return stdout;
};
const waitState = async (predicate) => {
  const file = join(directory, "state/wireplumber/default-routes");
  const value = () => (existsSync(file) ? readFileSync(file, "utf8") : "");
  if (predicate(value())) return;
  await new Promise((resolveReady, reject) => {
    const observer = watch(
      join(directory, "state"),
      { recursive: true },
      () => {
        if (predicate(value())) finish();
      },
    );
    const timer = setTimeout(
      () => finish(new Error("Route settings were not saved: " + value())),
      15000,
    );
    const finish = (error) => {
      clearTimeout(timer);
      observer.close();
      error ? reject(error) : resolveReady();
    };
    if (predicate(value())) finish();
  });
};
const waitMessage = async (record, predicate) => {
  if (predicate(record.stdout)) return;
  await new Promise((resolveReady, reject) => {
    const changed = () => {
      if (predicate(record.stdout)) finish();
    };
    const timer = setTimeout(
      () => finish(new Error(record.stdout + "\n" + record.stderr)),
      10000,
    );
    const finish = (error) => {
      clearTimeout(timer);
      record.child.stdout.off("data", changed);
      error ? reject(error) : resolveReady();
    };
    record.child.stdout.on("data", changed);
    changed();
  });
};
const monitorCard = (id) => {
  const record = start("desktop-monitor", pwDump, ["--monitor", "--no-colors"]);
  const state = { card: undefined, removed: false };
  let lines = [];
  createInterface({ input: record.child.stdout }).on("line", (line) => {
    lines.push(line);
    if (line !== "]") return;
    const items = JSON.parse(lines.join("\n"));
    lines = [];
    for (const item of items)
      if (item.id === id) {
        if (item.info === null) {
          state.removed = true;
          state.card = undefined;
        } else
          state.card = {
            ...state.card,
            ...item.info,
            params: { ...state.card?.params, ...item.info?.params },
          };
      }
  });
  return { record, state };
};
try {
  const version = await run(wireplumber, ["--version"], {});
  if (!version.includes("libwireplumber 0.4.")) {
    mkdirSync(join(config, "wireplumber/wireplumber.conf.d"), {
      recursive: true,
    });
    writeFileSync(
      join(config, "wireplumber/wireplumber.conf.d/99-test.conf"),
      "wireplumber.profiles = { main = { monitor.alsa=disabled monitor.bluez=disabled monitor.bluez-midi=disabled monitor.v4l2=disabled monitor.libcamera=disabled } }\n",
    );
  }
  start("pw", pipewire, []);
  await ready(join(runtime, "pipewire-0"));
  start("wp", wireplumber, []);
  const fixture = start("fixture", fixturePath, []);
  await waitMessage(fixture, (text) => text.includes("card-bound="));
  await waitMessage(fixture, (text) => text.includes("route-set device=1"));
  const dump = async (backend) => {
    const data = JSON.parse(
      await run(backend ? join(directory, "pipetune") : pwDump, [], {}),
    );
    const card = data.find(
      (item) =>
        item.info?.props?.["device.name"] === "pipetune.visibility.fixture",
    );
    assert(card, "fixture card must remain visible");
    return card;
  };
  const before = await dump(false);
  assert.equal(before.info.params.EnumRoute.length, 4);
  const select = async (index, device, volume) => {
    const previous = fixture.stdout.length;
    await run(
      join(directory, "controller/pipetune"),
      [
        "set-param",
        String(before.id),
        "Route",
        `{ index=${index} device=${device} save=true ${volume === undefined ? "" : `props={ channelVolumes=[${volume} ${volume}] mute=false }`} }`,
      ],
      {},
    );
    await waitMessage(fixture, (text) =>
      text
        .slice(previous)
        .includes(`route-set device=${device} index=${index}`),
    );
  };
  await select(1, 0, undefined);
  await select(3, 1, undefined);
  await waitState(
    (text) =>
      text.includes("profile:duplex=fixture-output-hdmi;fixture-input-line;") ||
      text.includes("profile:duplex=fixture-input-line;fixture-output-hdmi;") ||
      text.includes(
        'profile:duplex=["fixture-output-hdmi", "fixture-input-line"]',
      ),
  );
  await select(1, 0, 0.42);
  await select(3, 1, 0.63);
  const savedVolume = (text, name) => {
    const scalar = text.match(new RegExp(name + ":channelVolumes=([^;]+);"));
    if (scalar) return Number(scalar[1]);
    const json = text.match(new RegExp(name + "=(\\{[^\\n]+\\})"));
    return json ? JSON.parse(json[1]).channelVolumes?.[0] : NaN;
  };
  await waitState(
    (text) =>
      Math.abs(savedVolume(text, "fixture-output-hdmi") - 0.42) < 1e-6 &&
      Math.abs(savedVolume(text, "fixture-input-line") - 0.63) < 1e-6,
  );
  const selected = await dump(true);
  const monitor = monitorCard(before.id);
  await waitMessage(
    monitor.record,
    () => monitor.state.card?.params?.EnumRoute?.length === 4,
  );
  const owner = start("owner", pwCli, []);
  owner.child.stdin.write(
    "create-node adapter { factory.name=support.null-audio-sink node.name=pipetune.presentation.mode media.class=Audio/Sink node.pipetune.aggregate=true audio.position=[FL FR] }\n",
  );
  await waitMessage(owner, (text) =>
    text.includes("pipetune.presentation.mode:playback_0"),
  );
  await waitMessage(
    monitor.record,
    () => monitor.state.card?.params?.EnumRoute?.length === 2,
  );
  const desktop = await dump(false),
    backend = await dump(true);
  await run(routeProbe, [String(before.id)], {});
  writeFileSync(
    join(directory, "routes.json"),
    JSON.stringify({ before, desktop, backend }, null, 2),
  );
  assert.equal(
    backend.info.params.EnumRoute.length,
    4,
    "routing clients retain every route",
  );
  assert.equal(
    desktop.info.params.EnumRoute.length,
    2,
    "desktop clients must see input routes only",
  );
  assert.equal(desktop.id, before.id, "mode switch preserves the card ID");
  assert.deepEqual(
    desktop.info.params.EnumRoute,
    before.info.params.EnumRoute.filter((route) => route.direction === "Input"),
  );
  assert.deepEqual(
    monitor.state.card.params.EnumRoute,
    desktop.info.params.EnumRoute,
    "an already connected desktop client receives the filtered routes",
  );
  assert.deepEqual(
    backend.info.params.Route,
    selected.info.params.Route,
    "presentation must not change input/output selection, volume, mute or save flags",
  );
  await waitState(
    (text) =>
      text.includes("fixture-output-hdmi") &&
      text.includes("fixture-input-line"),
  );
  owner.child.stdin.end();
  await once(owner.child, "close");
  await waitMessage(
    monitor.record,
    () => monitor.state.card?.params?.EnumRoute?.length === 4,
  );
  const restored = await dump(false);
  assert.deepEqual(
    restored.info.params.EnumRoute,
    before.info.params.EnumRoute,
    "Single restores output routes",
  );
  assert.deepEqual(restored.info.params.Route, selected.info.params.Route);
  assert.equal(
    monitor.state.removed,
    false,
    "mode changes never recreate the device",
  );
  const reconnectOwner = start("reconnect-owner", pwCli, []);
  reconnectOwner.child.stdin.write(
    "create-node adapter { factory.name=support.null-audio-sink node.name=pipetune.presentation.mode media.class=Audio/Sink node.pipetune.aggregate=true audio.position=[FL FR] }\n",
  );
  await waitMessage(
    monitor.record,
    () => monitor.state.card?.params?.EnumRoute?.length === 2,
  );
  await select(1, 0, 0.36);
  await waitState(
    (text) => Math.abs(savedVolume(text, "fixture-output-hdmi") - 0.36) < 1e-6,
  );
  const savedMultiple = await dump(true);
  const stopped = once(fixture.child, "close");
  fixture.child.kill();
  await stopped;
  const replacement = start("replacement", fixturePath, []);
  await waitMessage(
    replacement,
    (text) =>
      text.includes("route-set device=0 index=1") &&
      text.includes("route-set device=1 index=3"),
  );
  const reconnected = await dump(true);
  assert.deepEqual(
    reconnected.info.params.Route,
    savedMultiple.info.params.Route,
    "WirePlumber restores saved output and input routes and controls",
  );
  assert.equal(reconnected.info.params.EnumRoute.length, 4);
  assert.equal(
    (await dump(false)).info.params.EnumRoute.length,
    2,
    "replacement cards honor the active presentation mode",
  );
  console.log("PASS client-specific routes, card identity and restoration");
} finally {
  for (const { child } of children.toReversed())
    if (child.exitCode === null && child.signalCode === null) {
      const closed = once(child, "close");
      child.kill();
      await closed;
    }
  console.log("Evidence: " + directory);
}
