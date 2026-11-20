// Ship exactly the files published by EffeTune's same-origin editor server.
import { createRequire } from "node:module";
import { copyFileSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";

const [source, destination] = process.argv.slice(2);
const { createStaticHandler } = createRequire(import.meta.url)(
  resolve(source, "electron/remote-static-server.cjs"),
);
const { allowed } = createStaticHandler({ root: source });
if (!allowed.has("remote.html")) throw Error("Missing EffeTune remote editor");
rmSync(destination, { recursive: true, force: true });
for (const relative of allowed) {
  const target = join(destination, relative);
  mkdirSync(dirname(target), { recursive: true });
  copyFileSync(join(source, relative), target);
}
writeFileSync(join(destination, "remote-files.txt"), [...allowed].join("\n") + "\n");
