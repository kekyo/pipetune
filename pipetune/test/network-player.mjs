import assert from "node:assert/strict";
import { spawn, execFileSync } from "node:child_process";
import { once } from "node:events";
import { createServer } from "node:http";
import { networkInterfaces, tmpdir } from "node:os";
import {
  mkdtemp,
  mkdir,
  readFile,
  writeFile,
  rm,
  stat,
  open,
} from "node:fs/promises";
import { resolve, join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { createRequire } from "node:module";

const [build, iface, upstream] = process.argv.slice(2);
assert.ok(
  build && iface && upstream,
  "usage: network-player.mjs BUILD PRIVATE_INTERFACE EFFETUNE_SOURCE",
);
const local = networkInterfaces()[iface]?.find(
  (address) => address.family === "IPv4",
);
assert.ok(local && !local.internal, "a private test interface is required");
const { Discovery, service, soap } = createRequire(import.meta.url)(
  resolve(upstream, "electron/network-music-upnp.cjs"),
);
const mpdConfig = await readFile(
  new URL("../packaging/player/mpd.conf", import.meta.url),
  "utf8",
);
const receiverConfig = await readFile(
  new URL("../packaging/player/upmpdcli.conf", import.meta.url),
  "utf8",
);
const directory = await mkdtemp(join(tmpdir(), "pipetune-player-"));
const config = join(directory, "config");
const runtime = join(directory, "run");
const state = join(directory, "state");
const bin = process.env.PIPETUNE_TEST_DAEMON || resolve(build, "pipetune");
const policy = resolve(
  build,
  "pipetune-build/pipetune_wireplumber_policy_fixture",
);
const env = {
  ...process.env,
  XDG_RUNTIME_DIR: runtime,
  PIPEWIRE_RUNTIME_DIR: runtime,
  XDG_CONFIG_HOME: config,
  XDG_STATE_HOME: state,
  XDG_DATA_HOME: join(directory, "data"),
  PIPEWIRE_CORE: "player-test",
  PIPEWIRE_REMOTE: "player-test",
  UPMPD_CACHEDIR: join(state, "receiver"),
  GIO_USE_VFS: "local",
};
const children = [];
const start = (program, args) => {
  const child = spawn(program, args, {
    env,
    stdio: ["ignore", "pipe", "pipe"],
  });
  child.diagnostic = "";
  child.stderr.on("data", (chunk) => {
    child.diagnostic = (child.diagnostic + chunk).slice(-12000);
  });
  child.done = new Promise((r) => {
    child.once("exit", r);
    child.once("error", r);
  });
  children.push(child);
  return child;
};
const stop = async (child) => {
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  child.kill("SIGTERM");
  await child.done;
};
const waitFor = async (label, check) => {
  const deadline = Date.now() + 25000;
  let diagnostic;
  do {
    try {
      const result = await check();
      if (result) return result;
    } catch (error) {
      diagnostic = error;
    }
    await delay(100);
  } while (Date.now() < deadline);
  throw new Error(
    label +
      ": " +
      (diagnostic ?? "") +
      "\n" +
      children.map((c) => c.spawnfile + " " + c.diagnostic).join("\n"),
  );
};
const put = async (relative, text) => {
  const path = join(config, relative);
  await mkdir(resolve(path, ".."), { recursive: true });
  await writeFile(path, text);
};
const text = (program, args) =>
  execFileSync(program, args, {
    env,
    encoding: "utf8",
    stdio: ["ignore", "pipe", "pipe"],
  });
const graph = () =>
  JSON.parse(text("pw-dump", ["--no-colors"]).replace(/\]\s*\[/g, ", "));
const writePolicy = async (path, part) => put(path, text(policy, [part]));
const wave = (seconds, level) => {
  const body = Buffer.alloc(44 + 48000 * 4 * seconds);
  body.write("RIFF");
  body.writeUInt32LE(body.length - 8, 4);
  body.write("WAVEfmt ", 8);
  body.writeUInt32LE(16, 16);
  body.writeUInt16LE(1, 20);
  body.writeUInt16LE(2, 22);
  body.writeUInt32LE(48000, 24);
  body.writeUInt32LE(192000, 28);
  body.writeUInt16LE(4, 32);
  body.writeUInt16LE(16, 34);
  body.write("data", 36);
  body.writeUInt32LE(body.length - 44, 40);
  for (let i = 44; i < body.length; i += 4) {
    const amplitude = (i - 44) % 192 < 96 ? level : -level;
    body.writeInt16LE(amplitude, i);
    body.writeInt16LE(amplitude, i + 2);
  }
  return body;
};
const firstDuration = process.env.PIPETUNE_TEST_EDITOR ? 120 : 16;
const songs = [wave(firstDuration, 8192), wave(16, 4096)];
const server = createServer((req, res) => {
  const data = songs[Number(req.url.slice(1, 2)) - 1];
  if (!data) {
    res.writeHead(404).end();
    return;
  }
  const range = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range ?? "");
  const start = range ? Number(range[1]) : 0;
  const end = range?.[2] ? Number(range[2]) : data.length - 1;
  res.writeHead(range ? 206 : 200, {
    "Content-Type": "audio/wav",
    "Accept-Ranges": "bytes",
    "Content-Length": end - start + 1,
    ...(range
      ? { "Content-Range": "bytes " + start + "-" + end + "/" + data.length }
      : {}),
  });
  res.end(req.method === "HEAD" ? undefined : data.subarray(start, end + 1));
});
let discovery = new Discovery();
try {
  await mkdir(runtime, { mode: 0o700 });
  await mkdir(join(state, "receiver"), { recursive: true });

  await put(
    "mpd.conf",
    mpdConfig.replace(
      "~/.local/state/pipetune/player/mpd.state",
      join(state, "mpd.state"),
    ),
  );
  await put("upmpdcli.conf", receiverConfig);
  const name = "pipetune-test-device";
  await put(
    "pipewire/pipewire.conf.d/99-test.conf",
    'context.properties = { core.name = "player-test" default.clock.rate = 48000 }\ncontext.objects = [ { factory = adapter args = { factory.name = support.null-audio-sink node.name = ' +
      name +
      " media.class = Audio/Sink audio.position = [ FL FR ] adapter.auto-port-config = { mode = dsp monitor = true position = preserve } } } ]",
  );
  if (text("wireplumber", ["--version"]).includes("libwireplumber 0.4.")) {
    await put(
      "wireplumber/main.lua.d/60-test.lua",
      "alsa_monitor.enabled = false\nv4l2_monitor.enabled = false\nlibcamera_monitor.enabled = false",
    );
    await put(
      "wireplumber/bluetooth.lua.d/60-test.lua",
      "bluez_monitor.enabled = false\nbluez_midi_monitor.enabled = false",
    );
    for (const [file, part] of [
      ["main.lua.d/60-pipetune-streams.lua", "stream-configuration"],
      ["scripts/pipetune-node-visibility.lua", "visibility"],
      ["policy.lua.d/60-pipetune-policy.lua", "configuration"],
      ["scripts/pipetune-endpoint-client.lua", "endpoint-client"],
      ["scripts/pipetune-endpoint-device.lua", "endpoint-device"],
    ])
      await writePolicy("wireplumber/" + file, part);
  } else {
    await put(
      "wireplumber/wireplumber.conf.d/60-test.conf",
      "wireplumber.profiles = { main = { monitor.alsa = disabled monitor.bluez = disabled monitor.v4l2 = disabled monitor.libcamera = disabled } }",
    );
    await writePolicy(
      "wireplumber/wireplumber.conf.d/60-pipetune-visibility.conf",
      "visibility-configuration",
    );
    await mkdir(join(env.XDG_DATA_HOME, "wireplumber/scripts"), {
      recursive: true,
    });
    await writeFile(
      join(
        env.XDG_DATA_HOME,
        "wireplumber/scripts/pipetune-node-visibility.lua",
      ),
      text(policy, ["visibility"]),
    );
  }
  start("pipewire", []);
  await waitFor("PipeWire socket", () => stat(join(runtime, "player-test")));
  start("wireplumber", []);
  const preset = join(directory, "gain.effetune_preset");
  await writeFile(preset, JSON.stringify([{ nm: "Volume", en: true, vl: -6 }]));
  await put("pipetune/environment", "PIPETUNE_RATE=48000\n");
  start(bin, ["daemon"]);
  await waitFor("daemon status", () => text(bin, ["--status"]));
  const recording = join(directory, "output.wav");
  const monitor = start("pw-cat", [
    "--record",
    "--format",
    "s16",
    "--channels",
    "2",
    "--rate",
    "48000",
    "--target",
    "0",
    "--properties",
    "node.name=player.capture node.autoconnect=false node.dont-move=true node.dont-fallback=true",
    recording,
  ]);
  const capture = await waitFor("recorder", () =>
    graph().find(
      (node) => node.info?.props?.["node.name"] === "player.capture",
    ),
  );
  text("pw-cli", [
    "set-param",
    String(capture.id),
    "PortConfig",
    "{ direction=Input mode=dsp monitor=false format={ mediaType=audio mediaSubtype=raw format=F32P rate=48000 channels=2 position=[FL FR] } }",
  ]);
  for (const channel of ["FL", "FR"]) {
    await waitFor("physical monitor " + channel, () => {
      text("pw-link", [
        name + ":monitor_" + channel,
        "player.capture:input_" + channel,
      ]);
      return true;
    });
  }
  const lastLevel = async (resampled = false) => {
    assert.equal(monitor.exitCode, null, monitor.diagnostic);
    const file = await open(recording, "r");
    const bytes = Buffer.alloc(16384);
    try {
      const { size } = await file.stat();
      if (size < bytes.length + 44) return -1;
      await file.read(bytes, 0, bytes.length, size - bytes.length);
    } finally {
      await file.close();
    }
    // Sample-rate conversion rounds the square wave's edges. Compare the
    // energy in both channels when the DSP and capture rates differ.
    if (resampled) {
      const squares = [0, 0];
      for (let offset = 0; offset + 3 < bytes.length; offset += 4) {
        squares[0] += bytes.readInt16LE(offset) ** 2;
        squares[1] += bytes.readInt16LE(offset + 2) ** 2;
      }
      const [left, right] = squares.map((sum) =>
        Math.sqrt(sum / (bytes.length / 4)),
      );
      return Math.abs(left - right) < 2 ? left : -1;
    }
    const histogram = new Map();
    for (let offset = 0; offset + 3 < bytes.length; offset += 4) {
      const value = Math.abs(bytes.readInt16LE(offset));
      if (Math.abs(value - Math.abs(bytes.readInt16LE(offset + 2))) > 2)
        continue;
      histogram.set(value, (histogram.get(value) ?? 0) + 1);
    }
    return [...histogram].sort((a, b) => b[1] - a[1])[0]?.[0] ?? -1;
  };
  server.listen(0, local.address);
  await once(server, "listening");
  const source = "http://" + local.address + ":" + server.address().port;
  let expectedPlayerId;
  for (const managed of process.env.PIPETUNE_TEST_UNIT_WRAPPER
    ? [false, true]
    : [false]) {
    const units = [];
    const managerEnv = {
      ...process.env,
      XDG_RUNTIME_DIR: process.env.PIPETUNE_TEST_MANAGER_RUNTIME,
      DBUS_SESSION_BUS_ADDRESS:
        "unix:path=" + process.env.PIPETUNE_TEST_MANAGER_RUNTIME + "/bus",
    };
    const receiverProcess = (program, args) => {
      if (!managed) {
        const child = start(program, args);
        return () => stop(child);
      }
      const unit =
        "pipetune-player-test-" + process.pid + "-" + program + ".service";
      const variables = Object.keys(env)
        .filter(
          (key) =>
            [
              "PATH",
              "LD_LIBRARY_PATH",
              "GIO_USE_VFS",
              "DBUS_SESSION_BUS_ADDRESS",
            ].includes(key) || /^(XDG_|PIPEWIRE_|UPMPD_)/u.test(key),
        )
        .map((key) => key + "=" + env[key]);
      execFileSync(
        "systemd-run",
        [
          "--user",
          "--quiet",
          "--collect",
          "--unit",
          unit,
          "--property=Type=exec",
          process.env.PIPETUNE_TEST_UNIT_WRAPPER,
          "--enter",
          String(process.pid),
          "/usr/bin/env",
          ...variables,
          program,
          ...args,
        ],
        { env: managerEnv },
      );
      units.push(unit);
      return () => {
        if (!units.includes(unit)) return;
        execFileSync("systemctl", ["--user", "stop", unit], {
          env: managerEnv,
        });
        units.splice(units.indexOf(unit), 1);
      };
    };
    try {
      text(bin, ["output", "mode", "single"]);
      text(bin, ["bypass"]);
      const stopMpd = receiverProcess("mpd", [
        "--no-daemon",
        join(config, "mpd.conf"),
      ]);
      const rendererLock = join(env.UPMPD_CACHEDIR, "renderer.lock");
      const stopReceiver = receiverProcess("flock", [
        "--nonblock",
        "--no-fork",
        "--verbose",
        rendererLock,
        "upmpdcli",
        "-m",
        "1",
        "-i",
        iface,
        "-c",
        join(config, "upmpdcli.conf"),
      ]);
      await discovery.start();
      const findDevice = async (type) =>
        waitFor("discover " + type, () => {
          const found = [...discovery.devices.values()].find(
            (d) =>
              d.available && d.name.startsWith("PipeTune") && service(d, type),
          );
          return found && service(found, type);
        });
      const playlist = await findDevice("Playlist");
      const found = [...discovery.devices.values()].find(
        (d) => d.available && service(d, "Playlist") === playlist,
      );
      if (expectedPlayerId)
        assert.equal(
          found.id,
          expectedPlayerId,
          "the service must publish the same receiver",
        );
      expectedPlayerId = found.id;
      if (managed) {
        const ids = Buffer.from(
          (await soap(playlist, "IdArray")).Array,
          "base64",
        );
        assert.equal(
          ids.length,
          4,
          "the receiver must restore the queued track across the handoff",
        );
        assert.equal(
          (await soap(playlist, "Read", { Id: ids.readUInt32BE() })).Uri,
          source + "/2.wav",
        );
      }
      await soap(playlist, "DeleteAll");
      const duplicate = start("mpd", ["--no-daemon", join(config, "mpd.conf")]);
      await waitFor(
        "dedicated MPD rejects duplicate listener",
        () => duplicate.exitCode !== null,
      );
      assert.notEqual(duplicate.exitCode, 0);
      assert.match(duplicate.diagnostic, /bind|already in use/iu);
      const duplicateReceiver = start("flock", [
        "--nonblock",
        rendererLock,
        "true",
      ]);
      assert.equal(
        await duplicateReceiver.done,
        1,
        "a second renderer must not acquire its instance lock",
      );
      let id = 0;
      for (const n of [1, 2]) {
        const added = await soap(playlist, "Insert", {
          AfterId: id,
          Uri: source + "/" + n + ".wav",
          Metadata: "",
        });
        id = Number(added.NewId);
        assert.ok(id);
      }
      await soap(playlist, "Play");
      const pcm = async (value, resampled = false) => {
        try {
          await waitFor(
            "output PCM amplitude " + value,
            async () =>
              Math.abs((await lastLevel(resampled)) - value) <=
              (resampled ? 0.02 * value : 2),
          );
        } catch (error) {
          console.error(
            "Last PCM level",
            await lastLevel(resampled),
            "state",
            text(bin, ["--status"]).slice(0, 2300),
          );
          console.error(
            graph()
              .map((n) => [
                n.id,
                n.info?.props?.["node.name"],
                n.info?.props?.["port.alias"],
                n.info?.["output-node-id"],
                n.info?.["input-node-id"],
              ])
              .filter((a) => a.slice(1).some(Boolean)),
          );
          throw error;
        }
      };
      await pcm(8192);
      text(bin, ["--load-preset", preset]);
      await pcm(4106);
      const pair = text(bin, ["remote", "enable", local.address, "0"]).trim();
      const editor = new WebSocket(pair.replace(/^http/u, "ws"));
      await once(editor, "open");
      let seq = 0;
      const edit = async (message) => {
        const id = ++seq;
        const acknowledged = new Promise((resolve, reject) => {
          const handler = (event) => {
            const reply = JSON.parse(event.data);
            if (reply.op !== "ack" || reply.seq !== id) return;
            editor.removeEventListener("message", handler);
            if (reply.ok) resolve();
            else reject(Error(reply.error));
          };
          editor.addEventListener("message", handler);
        });
        editor.send(JSON.stringify({ ...message, seq: id }));
        await acknowledged;
      };
      await edit({
        op: "chain",
        pipeline: [{ nm: "Volume", en: true, vl: -12 }],
      });
      await pcm(2058);
      await edit({ op: "bypass", on: true });
      await pcm(8192);
      await edit({ op: "params", index: 0, params: { vl: -6 } });
      await edit({ op: "bypass", on: false });
      await pcm(4106);
      await soap(playlist, "Pause");
      await pcm(0);
      await soap(playlist, "Play");
      await pcm(4106);
      await soap(playlist, "SeekSecondAbsolute", { Value: firstDuration - 2 });
      discovery.close();
      await pcm(2053);
      await soap(playlist, "Stop");
      await pcm(0);
      await discovery.start();
      const av = await findDevice("AVTransport");
      await soap(av, "SetAVTransportURI", {
        InstanceID: 0,
        CurrentURI: source + "/1.wav",
        CurrentURIMetaData: "",
      });
      await soap(av, "Play", { InstanceID: 0, Speed: "1" });
      await pcm(4106);
      await soap(av, "Stop", { InstanceID: 0 });
      await pcm(0);

      text(bin, ["output", "select", name]);
      text(bin, ["output", "mode", "multiple"]);
      const direct = await waitFor("Direct sink", () =>
        graph().find(
          (node) =>
            node.info?.props?.["node.description"] ===
              "PipeTune Direct Output" &&
            node.info.props["media.class"] === "Audio/Sink",
        ),
      );
      text("wpctl", ["set-default", String(direct.id)]);
      await waitFor("Direct selected", () =>
        text("wpctl", ["inspect", "@DEFAULT_AUDIO_SINK@"]).includes(
          "PipeTune Direct Output",
        ),
      );
      await soap(av, "SetAVTransportURI", {
        InstanceID: 0,
        CurrentURI: source + "/1.wav",
        CurrentURIMetaData: "",
      });
      await soap(av, "Play", { InstanceID: 0, Speed: "1" });
      await pcm(4106);
      const volume = service(found, "Volume");
      const originalVolume = Number((await soap(volume, "Volume")).Value);
      await soap(volume, "SetVolume", {
        Value: Math.round(originalVolume / 2),
      });
      await waitFor("the player's software mixer attenuates MPD", async () => {
        const level = await lastLevel();
        return level > 0 && level < 3800;
      });
      await soap(volume, "SetVolume", { Value: originalVolume });
      await pcm(4106);
      await soap(volume, "SetMute", { Value: 1 });
      await pcm(0);
      const desktopWave = join(directory, "desktop.wav");
      await writeFile(desktopWave, wave(3, 6144));
      const desktopStream = () =>
        start("pw-cat", [
          "--playback",
          "--target",
          "pipetune_sink",
          desktopWave,
        ]);
      const alongsideMutedPlayer = desktopStream();
      await pcm(3079);
      await stop(alongsideMutedPlayer);
      await pcm(0);
      await soap(volume, "SetMute", { Value: 0 });
      await pcm(4106);
      text(bin, ["rate", "set", "96000", "suggest"]);
      await edit({ op: "params", index: 0, params: { vl: -9 } });
      await pcm(2907, true);
      text(bin, ["rate", "set", "48000", "suggest"]);
      await edit({ op: "params", index: 0, params: { vl: -6 } });
      await pcm(4106);
      if (process.env.PIPETUNE_TEST_EDITOR) {
        // Only a new selection from the desktop application can restart playback.
        await soap(av, "Stop", { InstanceID: 0 });
        await pcm(0);
        const info = join(directory, "editor.json");
        await writeFile(
          info,
          JSON.stringify({
            pair,
            source,
            directory,
            openHomeId: found.id,
            playerId: [...discovery.devices.values()].find(
              (d) =>
                d.name.startsWith("PipeTune") &&
                service(d, "AVTransport") &&
                !service(d, "Playlist"),
            ).id,
            upstream: resolve(upstream),
          }),
          { mode: 0o600 },
        );
        const ui = start(process.execPath, [
          resolve(process.env.PIPETUNE_TEST_EDITOR),
          info,
        ]);
        ui.stdout.on("data", (bytes) => process.stdout.write(bytes));
        assert.equal(await ui.done, 0, ui.diagnostic);
        await pcm(2058);
        await edit({ op: "params", index: 0, params: { vl: -6 } });
        await pcm(4106);
      }
      editor.close();
      await soap(av, "Stop", { InstanceID: 0 });
      await pcm(0);
      console.log(
        "OpenHome and UPnP AV: stereo PCM 8192 to 4106 (-6 dB), pause/resume/seek/stop, unattended next track and processed playback with Direct as default.",
      );
      await soap(playlist, "DeleteAll");
      await soap(playlist, "Insert", {
        AfterId: 0,
        Uri: source + "/2.wav",
        Metadata: "",
      });
      await stopReceiver();
      await stopMpd();
      // The receiver has stopped. A desktop stream still reaches the same DSP.
      const desktop = desktopStream();
      await pcm(3079);
      await stop(desktop);
      text(bin, ["remote", "disable"]);
      console.log(
        (managed ? "Transient user units" : "Manual processes") +
          ": same renderer " +
          found.id +
          ", duplicate MPD and renderer rejected, desktop PCM survives receiver shutdown.",
      );
    } finally {
      discovery.close();
      discovery = new Discovery();
      for (const unit of units.reverse())
        execFileSync("systemctl", ["--user", "stop", unit], {
          env: managerEnv,
        });
    }
  }
} finally {
  discovery.close();
  server.closeAllConnections();
  server.close();
  for (const child of children.reverse()) await stop(child);
  await rm(directory, { recursive: true, force: true });
}
