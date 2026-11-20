import assert from "node:assert/strict";
import { spawn, spawnSync, execFileSync } from "node:child_process";
import { once } from "node:events";
import { join } from "node:path";
import { mkdir, writeFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import { setTimeout as delay } from "node:timers/promises";

const [bin] = process.argv.slice(2);
const cli = (...args) =>
  execFileSync(bin, args, {
    encoding: "utf8",
    stdio: ["ignore", "pipe", "pipe"],
  }).trim();
const preset = join(process.env.XDG_RUNTIME_DIR, "local.effetune_preset");
const local = (value) =>
  JSON.stringify({
    pipeline: [{ name: "Volume", enabled: true, parameters: { vl: value } }],
  });
await writeFile(preset, local(-6));
const daemon = spawn(bin, ["daemon"], { stdio: ["ignore", "ignore", "pipe"] });
const stopped = once(daemon, "exit");
let diagnostic = "";
daemon.stderr.on("data", (chunk) => (diagnostic += chunk));
const connect = async (link) => {
  const url = new URL(link);
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(url);
  const messages = [];
  socket.addEventListener("message", (e) => messages.push(JSON.parse(e.data)));
  await once(socket, "open", { signal: AbortSignal.timeout(5000) });
  let seq = 0;
  const receive = async (match) => {
    for (let i = 0; i < 500; i++) {
      const index = messages.findIndex(match);
      if (index >= 0) return messages.splice(index, 1)[0];
      await delay(10);
    }
    throw Error("Missing remote reply, received " + JSON.stringify(messages));
  };
  return {
    socket,
    receive,
    request: async (message, op, success = true) => {
      const number = ++seq;
      socket.send(JSON.stringify({ ...message, seq: number }));
      const ack = await receive((m) => m.op === "ack" && m.seq === number);
      assert.equal(ack.ok, success, JSON.stringify(ack));
      return op && receive((m) => m.op === op && m.seq === number);
    },
  };
};
try {
  for (let i = 0; i < 200 && spawnSync(bin, ["--status"]).status !== 0; i++)
    await delay(25);
  assert.equal(spawnSync(bin, ["remote", "link"]).status, 1);
  cli("--load-preset", preset);
  const pair = cli("remote", "enable", "127.0.0.1", "0");
  const publicStatus = () => JSON.parse(cli("--status")).remoteListener;
  assert.equal(publicStatus().port, Number(new URL(pair).port));
  assert.equal(publicStatus().address, "127.0.0.1");
  assert.equal(publicStatus().clients, 0);
  const firstRevision = publicStatus().revision;
  const token = new URL(pair).searchParams.get("t");
  assert.ok(!cli("--status").includes(token), "status subscribers must not receive pairing credentials");
  assert.equal(cli("remote", "link"), pair);
  assert.match(await (await fetch(pair)).text(), /<html/iu);
  assert.equal((await fetch(new URL("/electron/main.cjs", pair))).status, 404);
  assert.equal(
    (await fetch(pair, { headers: { Origin: "http://other.invalid" } })).status,
    403,
  );
  const editor = await connect(pair);
  const initial = await editor.request({ op: "hello", v: 1 }, "state");
  assert.equal(publicStatus().clients, 1);
  assert.equal(initial.appName, "PipeTune");
  assert.ok(initial.effects.includes("Volume"));
  assert.deepEqual(initial.features, []);
  assert.deepEqual(initial.pipeline, [{ nm: "Volume", en: true, vl: -6 }]);
  await editor.request({
    op: "chain",
    pipeline: [{ nm: "Volume", en: true, vl: -12 }],
  });
  await editor.request(
    { op: "chain", pipeline: [{ nm: "unknown" }] },
    null,
    false,
  );
  await editor.request({ op: "bypass", on: true });
  await editor.request({ op: "params", index: 0, params: { vl: -4 } });
  await editor.request({ op: "bypass", on: false });
  const state = await editor.request({ op: "get" }, "state");
  assert.equal(state.pipeline[0].vl, -4);
  assert.equal(state.masterBypass, false);
  assert.ok(state.rev > initial.rev);
  const presets = await editor.request({ op: "listPresets" }, "presets");
  assert.ok(presets.names.length > 0);
  assert.ok(
    Array.isArray(
      (
        await editor.request(
          { op: "getPreset", name: presets.names[0] },
          "preset",
        )
      ).pipeline,
    ),
  );
  const closed = once(editor.socket, "close");
  const rotated = cli("remote", "rotate");
  assert.ok(publicStatus().revision > firstRevision);
  assert.equal(publicStatus().clients, 0);
  assert.equal((await closed)[0].code, 4401);
  assert.notEqual(rotated, pair);
  const invalid = new WebSocket(pair.replace(/^http/u, "ws"));
  assert.equal(
    (await once(invalid, "close", { signal: AbortSignal.timeout(5000) }))[0]
      .code,
    4401,
  );
  const next = await connect(rotated);
  assert.deepEqual(
    (await next.request({ op: "get" }, "state")).pipeline,
    state.pipeline,
  );
  const observer = await connect(rotated);
  const current = await observer.request({ op: "get" }, "state");
  await observer.request(
    { op: "params", index: 0, params: { vl: 900 } },
    null,
    false,
  );
  const missing = [
    {
      nm: "IR Reverb",
      en: true,
      ir: "aaaaaaaaaaaaaaaaaaaaaaaa",
      cr: "full",
      lt: 128,
    },
  ];
  await next.request({ op: "chain", pipeline: missing }, null, false);
  assert.equal((await next.request({ op: "get" }, "state")).rev, current.rev);
  // The pending candidate must lose to a later accepted edit on the same stream.
  const canceled = next.request(
    { op: "chain", pipeline: missing },
    null,
    false,
  );
  await next.request({ op: "params", index: 0, params: { vl: -8 } });
  await canceled;
  await observer.receive((m) => m.op === "state" && m.pipeline[0]?.vl === -8);
  await observer.request({
    op: "chain",
    pipeline: [{ nm: "Volume", en: true, vl: -10 }],
  });
  await next.receive((m) => m.op === "state" && m.pipeline[0]?.vl === -10);
  assert.equal(JSON.parse(cli("--status")).activePluginCount, 1);
  cli("rate", "set", "96000", "suggest");
  assert.equal(
    (await next.request({ op: "get" }, "state")).pipeline[0].vl,
    -10,
  );
  if (
    JSON.parse(cli("--status")).availableDspBackends.some(
      (backend) => backend.name === "simd" && backend.available,
    )
  ) {
    cli("dsp", "set", "simd");
    assert.equal(
      (await next.request({ op: "get" }, "state")).pipeline[0].vl,
      -10,
    );
  }
  cli("dsp", "set", "scalar");
  assert.equal(
    (await next.request({ op: "get" }, "state")).pipeline[0].vl,
    -10,
  );
  cli("--load-preset", preset);
  await observer.receive((m) => m.op === "state" && m.pipeline[0]?.vl === -6);
  await writeFile(preset, local(-3));
  await observer.receive((m) => m.op === "state" && m.pipeline[0]?.vl === -3);
  const widePreset = join(process.env.XDG_RUNTIME_DIR, "wide.effetune_preset");
  const matrix = [{ nm: "Matrix", en: true, mx: "0213", ch: "A" }];
  await writeFile(widePreset, JSON.stringify(matrix));
  await next.request({ op: "bypass", on: true });
  cli("--load-preset", widePreset);
  assert.equal(
    (await next.request({ op: "get" }, "state")).masterBypass,
    false,
    "selecting a local preset must resume DSP when it changes the channel count",
  );
  await next.request({ op: "bypass", on: true });
  cli("bypass");
  assert.equal(
    (await next.request({ op: "get" }, "state")).masterBypass,
    false,
  );
  await next.request({ op: "chain", pipeline: matrix });
  assert.equal(JSON.parse(cli("--status")).inMemoryPreset, true);
  await next.request({ op: "bypass", on: true });
  await next.request({
    op: "chain",
    pipeline: [{ nm: "Volume", en: true, vl: -6 }],
  });
  assert.equal(
    (await next.request({ op: "get" }, "state")).masterBypass,
    true,
    "changing channel count from an editor must preserve its master bypass",
  );
  await next.request({ op: "bypass", on: false });
  const library = join(process.env.XDG_CONFIG_HOME, "effetune");
  const irDir = join(library, "ir-library"),
    measurements = join(library, "measurement-backups"),
    bank = join(library, "instruments");
  for (const directory of [irDir, measurements, bank])
    await mkdir(directory, { recursive: true });
  const wave = Buffer.alloc(44 + 512 * 4);
  wave.write("RIFF");
  wave.writeUInt32LE(wave.length - 8, 4);
  wave.write("WAVEfmt ", 8);
  wave.writeUInt32LE(16, 16);
  wave.writeUInt16LE(3, 20);
  wave.writeUInt16LE(1, 22);
  wave.writeUInt32LE(48000, 24);
  wave.writeUInt32LE(192000, 28);
  wave.writeUInt16LE(4, 32);
  wave.writeUInt16LE(32, 34);
  wave.write("data", 36);
  wave.writeUInt32LE(wave.length - 44, 40);
  wave.writeFloatLE(1, 44);
  const sha256 = createHash("sha256").update(wave).digest("hex"),
    id = sha256.slice(0, 24);
  await writeFile(join(irDir, id + ".wav"), wave);
  await writeFile(
    join(irDir, "index.json"),
    JSON.stringify({
      version: 1,
      entries: {
        [id]: {
          irId: id,
          composition: "single",
          bytes: wave.length,
          originals: [
            {
              role: "single",
              fileName: "impulse.wav",
              storageName: id + ".wav",
              sha256,
              byteLength: wave.length,
            },
          ],
        },
      },
    }),
  );
  await writeFile(join(bank, "a.wav"), wave);
  await writeFile(join(bank, "note.sfz"), "<region> sample=a.wav key=83");
  await writeFile(
    join(library, "sfz-references.json"),
    JSON.stringify([
      { id, name: "test", root: bank, path: join(bank, "note.sfz") },
    ]),
  );
  for (const side of ["left", "right"])
    await writeFile(
      join(measurements, side + ".json"),
      JSON.stringify({
        id: side,
        averageFrequencyResponse: [
          [20, 0],
          [1000, 4],
          [20000, 0],
        ],
      }),
    );
  for (const stage of [
    { nm: "IR Reverb", ir: id, cr: "full", lt: 128 },
    { nm: "Room EQ", ms0: "left", ms1: "right", pm: "lin", tp: 8192, lt: 128 },
    { nm: "SoundFont Note Player", sf: id, mn: 83, mx: 83 },
  ]) {
    await next.request({ op: "chain", pipeline: [stage] });
    const status = JSON.parse(cli("--status"));
    assert.equal(status.activePluginCount, 1);
    assert.ok(status.dspLatencyFrames > 0, stage.nm);
  }
  observer.socket.close();
  const disabled = once(next.socket, "close");
  cli("remote", "disable");
  assert.equal((await disabled)[0].code, 1001);
  assert.equal(publicStatus().port, 0);
  const certificate = join(process.env.XDG_RUNTIME_DIR, "certificate.pem"),
    key = join(process.env.XDG_RUNTIME_DIR, "key.pem");
  execFileSync(
    "openssl",
    [
      "req",
      "-x509",
      "-newkey",
      "rsa:2048",
      "-nodes",
      "-days",
      "1",
      "-subj",
      "/CN=127.0.0.1",
      "-addext",
      "subjectAltName=IP:127.0.0.1",
      "-keyout",
      key,
      "-out",
      certificate,
    ],
    { stdio: "ignore" },
  );
  assert.equal(
    spawnSync(bin, ["remote", "enable", "127.0.0.1", "0", key, certificate])
      .status,
    1,
  );
  const secure = cli("remote", "enable", "127.0.0.1", "0", certificate, key);
  assert.match(secure, /^https:/u);
  assert.equal(publicStatus().certificate, certificate);
  assert.equal(publicStatus().key, key);
  execFileSync(
    process.execPath,
    [
      "--input-type=module",
      "-e",
      `
    import assert from "node:assert/strict";
    import {once} from "node:events";
    const link = process.env.PAIRED_LINK;
    assert.match(await (await fetch(link)).text(),/<html/i);
    const ws = new WebSocket(link.replace(/^https/,"wss"));
    await once(ws,"open");
    ws.send(JSON.stringify({op:"get",seq:1}));
    const [reply] = await once(ws,"message");
    assert.ok(JSON.parse(reply.data).ok);
    ws.close();`,
    ],
    {
      env: {
        ...process.env,
        NODE_EXTRA_CA_CERTS: certificate,
        PAIRED_LINK: secure,
      },
      timeout: 10000,
    },
  );
  cli("remote", "disable");
  assert.equal(spawnSync(bin, ["remote", "link"]).status, 1);
} finally {
  daemon.kill("SIGTERM");
  await stopped;
  if (diagnostic) process.stderr.write(diagnostic);
}
