import assert from "node:assert/strict";
import { spawn, spawnSync, execFileSync } from "node:child_process";
import { once } from "node:events";
import { join } from "node:path";
import { writeFile } from "node:fs/promises";
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
  assert.equal(cli("remote", "link"), pair);
  assert.match(await (await fetch(pair)).text(), /<html/iu);
  assert.equal((await fetch(new URL("/electron/main.cjs", pair))).status, 404);
  assert.equal(
    (await fetch(pair, { headers: { Origin: "http://other.invalid" } })).status,
    403,
  );
  const editor = await connect(pair);
  const initial = await editor.request({ op: "hello", v: 1 }, "state");
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
  const disabled = once(next.socket, "close");
  cli("remote", "disable");
  assert.equal((await disabled)[0].code, 1001);
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
