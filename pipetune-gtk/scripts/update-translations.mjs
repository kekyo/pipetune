import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  extractMessages,
  parseCatalog,
  serializeCatalog,
} from './translation-tools.mjs';

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const projectDirectory =
  process.argv[2] === undefined
    ? resolve(scriptDirectory, '..')
    : resolve(process.argv[2]);
const potPath = resolve(projectDirectory, 'po', 'pipetune-gtk.pot');
const japanesePath = resolve(projectDirectory, 'po', 'ja.po');
const messages = extractMessages(projectDirectory);
const existingJapanese = existsSync(japanesePath)
  ? parseCatalog(readFileSync(japanesePath, 'utf8'))
  : new Map();

writeFileSync(potPath, serializeCatalog(messages, new Map(), true));
writeFileSync(
  japanesePath,
  serializeCatalog(messages, existingJapanese, false)
);
