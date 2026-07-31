import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  catalogProblems,
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
const potSource = readFileSync(potPath, 'utf8');
const japaneseSource = readFileSync(japanesePath, 'utf8');
const problems = catalogProblems(
  messages,
  parseCatalog(potSource),
  parseCatalog(japaneseSource)
);
if (potSource !== serializeCatalog(messages, new Map(), true)) {
  problems.push(
    'POT is stale; run node scripts/update-translations.mjs'
  );
}
if (problems.length !== 0) {
  throw new Error(problems.join('\n'));
}
