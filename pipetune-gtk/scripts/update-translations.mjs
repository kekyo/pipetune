import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  catalogLocales,
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
const messages = extractMessages(projectDirectory);

writeFileSync(potPath, serializeCatalog(messages, new Map(), true));
for (const locale of catalogLocales) {
  const catalogPath = resolve(projectDirectory, 'po', `${locale.id}.po`);
  const existing = existsSync(catalogPath)
    ? parseCatalog(readFileSync(catalogPath, 'utf8'))
    : new Map();
  writeFileSync(
    catalogPath,
    serializeCatalog(messages, existing, false, locale)
  );
}
