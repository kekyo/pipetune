// Optional end-to-end test for a real EffeTune desktop and a second browser on the private test LAN.
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { readFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { setTimeout as delay } from "node:timers/promises";

const executable = process.env.PIPETUNE_TEST_EFFETUNE_DESKTOP;
const chromium = process.env.PIPETUNE_TEST_CHROMIUM;
assert.ok(
  executable && chromium,
  "Set PIPETUNE_TEST_EFFETUNE_DESKTOP and PIPETUNE_TEST_CHROMIUM",
);
const openMcp = async (args) => {
  const child = spawn(
    process.env.PIPETUNE_TEST_PLAYWRIGHT_MCP || "playwright-mcp",
    args,
    { stdio: ["pipe", "pipe", "pipe"] },
  );
  let seq = 0;
  const pending = new Map();
  const onDisconnect = (reason) => {
    for (const { reject } of pending.values()) reject(Error(String(reason)));
    pending.clear();
  };
  child.once("error", onDisconnect);
  child.once("exit", (code) =>
    onDisconnect("Playwright MCP exited with code " + code),
  );
  child.stderr.on("data", (chunk) => process.stderr.write(chunk));
  createInterface({ input: child.stdout }).on("line", (line) => {
    const msg = JSON.parse(line);
    pending.get(msg.id)?.resolve(msg);
    pending.delete(msg.id);
  });
  const rpc = async (method, params) => {
    const id = ++seq;
    const response = new Promise((resolve, reject) =>
      pending.set(id, { resolve, reject }),
    );
    const timeout = setTimeout(
      () => pending.get(id)?.reject(Error("Timed out waiting for " + method)),
      45000,
    );
    child.stdin.write(
      JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n",
    );
    try {
      const msg = await response;
      if (msg.error) throw Error(JSON.stringify(msg.error));
      return msg.result;
    } finally {
      clearTimeout(timeout);
      pending.delete(id);
    }
  };
  await rpc("initialize", {
    protocolVersion: "2025-03-26",
    capabilities: {},
    clientInfo: { name: "PipeTune interoperability test", version: "1" },
  });
  child.stdin.write(
    JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) +
      "\n",
  );
  const tool = async (name, args) => {
    const v = await rpc("tools/call", { name, arguments: args });
    if (v.isError || v.content.some((b) => b.text?.includes("### Error")))
      throw Error(v.content.map((c) => c.text).join("\n"));
    return v.content
      .filter((b) => b.type === "text")
      .map((b) => b.text)
      .join("\n");
  };
  return { tool, close: () => child.stdin.end() };
};

const { pair, directory, playerId, openHomeId, source, upstream } = JSON.parse(
  await readFile(process.argv[2], "utf8"),
);
const req = createRequire(import.meta.url);
const { Discovery } = req(join(upstream, "electron/network-music-upnp.cjs"));
const { MusicServer } = req(
  join(upstream, "electron/network-music-server.cjs"),
);
const ds = new Discovery();
await ds.start();
const music = new MusicServer({
  id: "uuid:d4f59bdc-7eab-4a37-87f2-c5b334802102",
  name: "Test WAV library",
  discovery: ds,
  browse: async () => ({
    items: [
      {
        id: "1",
        title: "Test WAV",
        uri: source + "/1.wav",
        protocolInfo: "http-get:*:audio/wav:*",
      },
    ],
    total: 1,
  }),
});
await music.start();
const check = async (m, fn) => {
  const args = { function: fn };
  for (let i = 0; i < 80; i++) {
    const value = await m.tool("browser_evaluate", args);
    if (/### Result\n(?:true|"ok")/u.test(value)) return;
    if (i === 79) throw Error(value.slice(0, 600));
    await delay(250);
  }
};
const change = async (m, db) => {
  await check(
    m,
    `() => {
      const slider = document.querySelector('#pipeline input[type=range]');
      if (!slider) return false;
      slider.value = String(${db});
      slider.dispatchEvent(new Event('input', { bubbles: true }));
      slider.dispatchEvent(new Event('change', { bubbles: true }));
      return true;
    }`,
  );
};
const web = await openMcp([
  "--headless",
  "--isolated",
  "--no-sandbox",
  "--executable-path",
  chromium,
  "--output-dir",
  directory,
]);
let desktop, m;
try {
  await web.tool("browser_navigate", { url: pair });
  await change(web, -9);
  console.log("Browser Volume edited using Playwright MCP");
  desktop = spawn(
    executable,
    [
      "--no-sandbox",
      "--disable-gpu",
      "--remote-debugging-address=127.0.0.1",
      "--remote-debugging-port=28426",
      "--user-data-dir=" + join(directory, "electron"),
    ],
    { stdio: ["ignore", "ignore", "pipe"] },
  );
  let stderr = "";
  desktop.stderr.on(
    "data",
    (chunk) => (stderr = (stderr + chunk).slice(-1800)),
  );
  let ok = false;
  for (let i = 0; i < 150; i++) {
    try {
      const j = await (await fetch("http://127.0.0.1:28426/json/list")).json();
      if (j.some((x) => /effetune.html/.test(x.url))) {
        ok = true;
        break;
      }
    } catch {}
    await delay(120);
  }
  if (!ok) throw Error("Desktop startup: " + stderr);
  m = await openMcp([
    "--cdp-endpoint",
    "http://127.0.0.1:28426",
    "--output-dir",
    directory,
  ]);
  await check(m, "() => !!window.electronAPI?.networkMusicDiscover");
  await m.tool("browser_evaluate", {
    function:
      "async () => {await window.electronAPI.networkMusicDiscover(); return true;}",
  });
  const selectTrack = (id) => `async () => {
    const api = window.electronAPI;
    const state = await api.networkMusicGetState();
    const player = state.players.find(p => p.id === ${JSON.stringify(id)});
    const library = state.libraries.find(l => l.name === 'Test WAV library');
    if (!player || !library) return false;
    const page = await api.networkMusicBrowse({ serverId: library.id });
    if (!page.items.length) return false;
    await api.networkMusicPlayerCommand({
      playerId: player.id, action: 'replace', play: true,
      items: [{ serverId: library.id, trackId: page.items[0].id }]
    });
    return true;
  }`;
  try {
    await check(m, selectTrack(openHomeId));
    console.log("EffeTune desktop selected the OpenHome renderer");
  } catch (error) {
    // upmpdcli 1.9.17 reports an unchanged queue as changed to EffeTune 2.16.
    if (!String(error).includes("The player queue changed.")) throw error;
    console.log(
      "OpenHome desktop selection unavailable: the renderer reports a changed queue",
    );
  }
  await check(m, selectTrack(playerId));
  console.log(
    "EffeTune desktop selected a WAV from the ContentDirectory library for the UPnP AV renderer",
  );
  const joined = await m.tool("browser_evaluate", {
    function: `async () => (await window.electronAPI.remoteV1.settings.join(${JSON.stringify(pair)})).ok`,
  });
  if (!joined.includes("true")) throw Error("Join failed");
  let index;
  for (let i = 0; i < 60; i++) {
    const tabs = await m.tool("browser_tabs", { action: "list" });
    const row = tabs.split("\n").find((s) => s.includes(new URL(pair).origin));
    if (row) {
      const match = row.match(/(?:^|\s)(\d+):/u);
      if (match) {
        index = Number(match[1]);
        break;
      }
    }
    await delay(100);
  }
  if (index === undefined) throw Error("Remote editor window missing");
  await m.tool("browser_tabs", { action: "select", index });
  await change(m, -12);
  await check(
    web,
    "() => Number(document.querySelector('#pipeline input[type=range]')?.value)===-12",
  );
  console.log("EffeTune 2.16 desktop and browser both show -12 dB");
} finally {
  await music.close();
  ds.close();
  if (m) m.close();
  web.close();
  if (desktop && desktop.exitCode === null && desktop.signalCode === null) {
    const ended = once(desktop, "exit");
    desktop.kill("SIGTERM");
    await ended;
  }
}
