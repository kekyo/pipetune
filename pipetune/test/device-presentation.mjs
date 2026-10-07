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
  statSync,
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
  pwCat,
  pwLink,
  policyFixture,
  pulseServer,
  desktopProbe,
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
const checkPcm = process.argv.includes("--pcm");
const checkDesktop = process.argv.includes("--desktop");
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
  const record = { name, child, stdout: "", stderr: "" };
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
const graph = async (backend) => {
  const text = await run(
    backend ? join(directory, "pipetune") : pwDump,
    [],
    {},
  );
  const objects = new Map();
  for (const item of JSON.parse(
    "[" + text.replace(/\]\s*\[/gu, "],[") + "]",
  ).flat()) {
    if (item.info === null) objects.delete(item.id);
    else {
      const previous = objects.get(item.id);
      objects.set(item.id, {
        ...previous,
        ...item,
        info: {
          ...previous?.info,
          ...item.info,
          props: { ...previous?.info?.props, ...item.info?.props },
          params: { ...previous?.info?.params, ...item.info?.params },
        },
      });
    }
  }
  return [...objects.values()];
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
      () =>
        finish(
          new Error(
            "Waiting for " +
              record.name +
              " failed. See " +
              directory +
              "; " +
              record.stderr.slice(-1500),
          ),
        ),
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
const startAudio = async () => {
  assert.ok(pwCat && pwLink);
  const samples = Buffer.alloc(44 + 48000 * 60 * 8);
  samples.write("RIFF");
  samples.writeUInt32LE(samples.length - 8, 4);
  samples.write("WAVEfmt ", 8);
  samples.writeUInt32LE(16, 16);
  samples.writeUInt16LE(3, 20);
  samples.writeUInt16LE(2, 22);
  samples.writeUInt32LE(48000, 24);
  samples.writeUInt32LE(48000 * 8, 28);
  samples.writeUInt16LE(8, 32);
  samples.writeUInt16LE(32, 34);
  samples.write("data", 36);
  samples.writeUInt32LE(samples.length - 44, 40);
  for (let offset = 44; offset < samples.length; offset += 8) {
    samples.writeFloatLE(0.125, offset);
    samples.writeFloatLE(0.25, offset + 4);
  }
  const signalPath = join(directory, "signal.wav");
  writeFileSync(signalPath, samples);
  const observer = start("audio-observer", pwCli, []);
  await waitMessage(observer, (text) => text.includes("remote 0 is named"));
  start("feed", pwCat, [
    "--playback",
    "--target",
    "0",
    "--latency",
    "256",
    "--properties",
    "media.class=Audio/Source node.name=presentation.feed node.virtual=true priority.session=0 node.autoconnect=false",
    signalPath,
  ]);
  await waitMessage(observer, (text) =>
    text.includes('node.name = "presentation.feed"'),
  );
  const graph = JSON.parse(await run(join(directory, "pipetune"), [], {}));
  const feed = graph.find(
    (item) => item.info?.props?.["node.name"] === "presentation.feed",
  );
  assert.ok(feed);
  const input = graph.find(
    (item) => item.info?.props?.["node.name"] === "fixture.input",
  );
  assert.ok(input);
  await run(
    pwCli,
    [
      "set-param",
      String(feed.id),
      "PortConfig",
      "{ direction=Output mode=dsp monitor=false format={ mediaType=audio mediaSubtype=raw format=F32P rate=48000 channels=2 position=[FL FR] } }",
    ],
    {},
  );
  await waitMessage(observer, (text) =>
    text.includes("presentation.feed:capture_0"),
  );
  for (const channel of ["FL", "FR"]) {
    await run(
      pwLink,
      [
        `presentation.feed:capture_${channel}`,
        `fixture.input:input_${channel}`,
      ],
      {},
    );
    await run(
      pwLink,
      [
        `presentation.feed:capture_${channel}`,
        `fixture.output:playback_${channel}`,
      ],
      {},
    );
  }
  const captures = ["input", "output"].map((direction) => {
    const path = join(directory, `capture-${direction}.wav`);
    const record = start("capture-" + direction, pwCat, [
      "--record",
      "--target",
      "fixture." + direction,
      "--rate",
      "48000",
      "--channels",
      "2",
      "--channel-map",
      "FL,FR",
      "--format",
      "f32",
      "--latency",
      "256",
      "--properties",
      // Measure the specified device even when WirePlumber follows a new default.
      `node.name=presentation.capture.${direction} node.dont-fallback=true node.dont-move=true stream.capture.sink=${direction === "output"}`,
      path,
    ]);
    return { path, record };
  });
  const verify = async (inputGain, outputGain) => {
    for (const [index, { path, record }] of captures.entries()) {
      const initialSize = statSync(path, { throwIfNoEntry: false })?.size ?? 0;
      const gain = index === 0 ? inputGain : outputGain;
      const matches = () => {
        if (
          (statSync(path, { throwIfNoEntry: false })?.size ?? 0) <
          initialSize + 8192 * 8
        )
          return false;
        assert.equal(
          record.child.exitCode,
          null,
          "recording must remain active",
        );
        const bytes = readFileSync(path);
        let dataOffset = 12;
        while (
          dataOffset + 8 <= bytes.length &&
          bytes.toString("ascii", dataOffset, dataOffset + 4) !== "data"
        ) {
          const size = bytes.readUInt32LE(dataOffset + 4);
          dataOffset += 8 + size + (size % 2);
        }
        dataOffset += 8;
        const end =
          dataOffset + Math.floor((bytes.length - dataOffset) / 8) * 8;
        for (let offset = end - 4096 * 8; offset < end; offset += 8)
          if (
            Math.abs(bytes.readFloatLE(offset) - 0.125 * gain) > 1e-6 ||
            Math.abs(bytes.readFloatLE(offset + 4) - 0.25 * gain) > 1e-6
          )
            return false;
        return true;
      };
      await new Promise((resolveReady, reject) => {
        const observer = watch(directory, () => {
          if (matches()) finish();
        });
        const timer = setTimeout(
          () =>
            finish(
              new Error("PCM controls mismatch: " + path + " gain=" + gain),
            ),
          10000,
        );
        const finish = (error) => {
          clearTimeout(timer);
          observer.close();
          error ? reject(error) : resolveReady();
        };
        if (matches()) finish();
      });
    }
    const current = JSON.parse(await run(join(directory, "pipetune"), [], {}));
    assert.equal(
      current.find(
        (item) => item.info?.props?.["node.name"] === "fixture.input",
      )?.id,
      input.id,
      "mode changes and control operations preserve the input node",
    );
  };
  return { verify };
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
  if (checkDesktop) {
    const policy = async (part) => await run(policyFixture, [part], {});
    const write = (path, text) => {
      const full = join(config, path);
      mkdirSync(join(full, ".."), { recursive: true });
      writeFileSync(full, text);
    };
    if (version.includes("libwireplumber 0.4.")) {
      write(
        "wireplumber/policy.lua.d/60-pipetune.lua",
        await policy("configuration"),
      );
      for (const part of [
        "endpoint-client",
        "endpoint-device",
        "node-visibility",
      ])
        write(
          `wireplumber/scripts/pipetune-${part}.lua`,
          await policy(part === "node-visibility" ? "visibility" : part),
        );
    } else {
      environment.XDG_DATA_HOME = join(directory, "data");
      mkdirSync(join(environment.XDG_DATA_HOME, "wireplumber/scripts"), {
        recursive: true,
      });
      writeFileSync(
        join(
          environment.XDG_DATA_HOME,
          "wireplumber/scripts/pipetune-node-visibility.lua",
        ),
        await policy("visibility"),
      );
      write(
        "wireplumber/wireplumber.conf.d/60-pipetune.conf",
        await policy("visibility-configuration"),
      );
    }
  }
  start("pw", pipewire, []);
  await ready(join(runtime, "pipewire-0"));
  start("wp", wireplumber, []);
  const fixture = start("fixture", fixturePath, []);
  await waitMessage(fixture, (text) => text.includes("card-bound="));
  await waitMessage(fixture, (text) => text.includes("route-set device=1"));
  const desktopOutputs = async () =>
    (await run(desktopProbe, [], {})).trim().split("\n").sort();
  if (checkDesktop) {
    start("pulse", pulseServer, []);
    await ready(join(runtime, "pulse/native"));
    const internal = start("internal", pwCli, []);
    for (const [name, properties] of [
      ["pipetune_sink", "node.pipetune.internal=true"],
      ["control.endpoint.pipetune.playback", "node.pipetune.internal=true"],
      ["fixture.virtual", "node.virtual=true"],
    ])
      internal.child.stdin.write(
        `create-node adapter { factory.name=support.null-audio-sink node.name=${name} media.class=Audio/Sink audio.position=[FL FR] ${properties} }\n`,
      );
    await waitMessage(internal, (text) =>
      text.includes("fixture.virtual:playback"),
    );
    assert.deepEqual(
      await desktopOutputs(),
      ["fixture.output", "fixture.virtual"],
      "an active input peak meter must not expose internal outputs in Single",
    );
  }
  const dump = async (backend) => {
    const data = await graph(backend);
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
  const audio = checkPcm ? await startAudio() : undefined;
  if (audio) await audio.verify(0.63, 0.42);
  const monitor = monitorCard(before.id);
  await waitMessage(
    monitor.record,
    () => monitor.state.card?.params?.EnumRoute?.length === 4,
  );
  const owner = start("owner", pwCli, []);
  owner.child.stdin.write(
    "create-node adapter { factory.name=support.null-audio-sink node.name=pipetune.presentation.mode media.class=Audio/Sink node.pipetune.aggregate=true audio.position=[FL FR] }\n",
  );
  await waitMessage(
    monitor.record,
    () => monitor.state.card?.params?.EnumRoute?.length === 2,
  );
  const desktop = await dump(false),
    backend = await dump(true);
  if (checkDesktop)
    assert.deepEqual(
      await desktopOutputs(),
      ["pipetune.presentation.mode"],
      "Multiple must expose only the public output, including to recording clients",
    );
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
  if (audio) {
    await audio.verify(0.63, 0.42);
    for (const profile of [1, 0]) {
      await run(
        pwCli,
        [
          "set-param",
          String(before.id),
          "Profile",
          `{ index=${profile} save=true }`,
        ],
        {},
      );
      await waitMessage(
        monitor.record,
        () => monitor.state.card?.params?.Profile?.[0]?.index === profile,
      );
    }
    await audio.verify(0.63, 0.42);
    await run(
      join(directory, "controller/pipetune"),
      [
        "set-param",
        String(before.id),
        "Route",
        "{ index=3 device=1 save=true props={ mute=true } }",
      ],
      {},
    );
    await audio.verify(0, 0.42);
    await run(
      join(directory, "controller/pipetune"),
      [
        "set-param",
        String(before.id),
        "Route",
        "{ index=3 device=1 save=true props={ mute=false channelVolumes=[0.5 0.5] } }",
      ],
      {},
    );
    await audio.verify(0.5, 0.42);
    await select(3, 1, 0.63);
    await audio.verify(0.63, 0.42);
  }
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
  if (checkDesktop)
    assert.deepEqual(
      await desktopOutputs(),
      ["fixture.output", "fixture.virtual"],
      "Single must restore normal outputs without internal endpoints",
    );
  assert.deepEqual(
    restored.info.params.EnumRoute,
    before.info.params.EnumRoute,
    "Single restores output routes",
  );
  assert.deepEqual(restored.info.params.Route, selected.info.params.Route);
  if (audio) await audio.verify(0.63, 0.42);
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
