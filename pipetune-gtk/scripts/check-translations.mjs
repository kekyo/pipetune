import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  catalogLocales,
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
const messages = extractMessages(projectDirectory);
const potSource = readFileSync(potPath, 'utf8');
const catalogs = catalogLocales.map((locale) => {
  const source = readFileSync(
    resolve(projectDirectory, 'po', `${locale.id}.po`),
    'utf8'
  );
  return { locale, source, catalog: parseCatalog(source) };
});
const problems = catalogProblems(
  messages,
  parseCatalog(potSource),
  catalogs
);
if (potSource !== serializeCatalog(messages, new Map(), true)) {
  problems.push(
    'POT is stale; run node scripts/update-translations.mjs'
  );
}
for (const { locale, source, catalog } of catalogs) {
  if (source !== serializeCatalog(messages, catalog, false, locale)) {
    problems.push(
      `${locale.languageTeam} PO is stale; run node scripts/update-translations.mjs`
    );
  }
}
if (problems.length !== 0) {
  throw new Error(problems.join('\n'));
}
