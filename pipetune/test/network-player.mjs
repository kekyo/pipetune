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
const bin = resolve(build, "pipetune");
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
const songs = [wave(16, 8192), wave(16, 4096)];
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
const discovery = new Discovery();
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
  const lastLevel = async () => {
    assert.equal(monitor.exitCode, null, monitor.diagnostic);
    const bytes = await readFile(recording);
    if (bytes.length < 16428) return -1;
    const histogram = new Map();
    for (
      let offset = bytes.length - 16384;
      offset + 3 < bytes.length;
      offset += 4
    ) {
      const value = Math.abs(bytes.readInt16LE(offset));
      if (Math.abs(value - Math.abs(bytes.readInt16LE(offset + 2))) > 2)
        continue;
      histogram.set(value, (histogram.get(value) ?? 0) + 1);
    }
    return [...histogram].sort((a, b) => b[1] - a[1])[0]?.[0] ?? -1;
  };
  start("mpd", ["--no-daemon", join(config, "mpd.conf")]);
  const receiver = start("upmpdcli", [
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
        (d) => d.available && d.name.startsWith("PipeTune") && service(d, type),
      );
      return found && service(found, type);
    });
  const playlist = await findDevice("Playlist");
  server.listen(0, local.address);
  await once(server, "listening");
  const source = "http://" + local.address + ":" + server.address().port;
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
  const pcm = async (value) => {
    try {
      await waitFor(
        "output PCM amplitude " + value,
        async () => Math.abs((await lastLevel()) - value) <= 2,
      );
    } catch (error) {
      console.error(
        "Last PCM level",
        await lastLevel(),
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
  await soap(playlist, "Pause");
  await pcm(0);
  await soap(playlist, "Play");
  await pcm(4106);
  await soap(playlist, "SeekSecondAbsolute", { Value: 14 });
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
        node.info?.props?.["node.description"] === "PipeTune Direct Output" &&
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
  await soap(av, "Stop", { InstanceID: 0 });
  await pcm(0);
  console.log(
    "OpenHome and UPnP AV: stereo PCM 8192 to 4106 (-6 dB), pause/resume/seek/stop, unattended next track and processed playback with Direct as default.",
  );
  await stop(receiver);
} finally {
  discovery.close();
  server.closeAllConnections();
  server.close();
  for (const child of children.reverse()) await stop(child);
  await rm(directory, { recursive: true, force: true });
}
