import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

const decodeCppString = (source, start) => {
  if (source[start] !== '"') {
    return undefined;
  }
  let index = start + 1;
  let escaped = false;
  while (index < source.length) {
    const character = source[index];
    if (!escaped && character === '"') {
      const literal = source.slice(start, index + 1);
      return { value: JSON.parse(literal), next: index + 1 };
    }
    escaped = !escaped && character === '\\';
    if (character !== '\\') {
      escaped = false;
    }
    index += 1;
  }
  throw new Error('unterminated C++ translation string');
};

const skipWhitespace = (source, start) => {
  let index = start;
  while (index < source.length && /\s/u.test(source[index])) {
    index += 1;
  }
  return index;
};

const parseCppStringSequence = (source, start) => {
  let index = skipWhitespace(source, start);
  let value = '';
  let found = false;
  while (source[index] === '"') {
    const parsed = decodeCppString(source, index);
    value += parsed.value;
    found = true;
    index = skipWhitespace(source, parsed.next);
  }
  return found ? { value, next: index } : undefined;
};

const addMessage = (messages, singular, plural = undefined) => {
  if (singular === '') {
    return;
  }
  const existing = messages.get(singular);
  if (existing !== undefined && existing.plural !== plural) {
    throw new Error(`conflicting plural forms for ${singular}`);
  }
  messages.set(singular, { singular, plural });
};

const extractCppMessages = (source, messages) => {
  const callPattern = /\b(translatePlural|translate|localizedMessage)\s*\(/gu;
  for (const match of source.matchAll(callPattern)) {
    const first = parseCppStringSequence(
      source,
      match.index + match[0].length
    );
    if (first === undefined) {
      continue;
    }
    if (match[1] !== 'translatePlural') {
      addMessage(messages, first.value);
      continue;
    }
    let index = skipWhitespace(source, first.next);
    if (source[index] !== ',') {
      throw new Error(`plural translation lacks a comma: ${first.value}`);
    }
    index = skipWhitespace(source, index + 1);
    const second = parseCppStringSequence(source, index);
    if (second === undefined) {
      throw new Error(`plural translation lacks a plural: ${first.value}`);
    }
    addMessage(messages, first.value, second.value);
  }
};

const decodeXml = (value) =>
  value
    .replaceAll('&quot;', '"')
    .replaceAll('&apos;', "'")
    .replaceAll('&lt;', '<')
    .replaceAll('&gt;', '>')
    .replaceAll('&amp;', '&');

const extractBuilderMessages = (source, messages) => {
  const elementPattern =
    /<(?:property|item)\b[^>]*\btranslatable="yes"[^>]*>([^<]*)<\/(?:property|item)>/gu;
  for (const match of source.matchAll(elementPattern)) {
    addMessage(messages, decodeXml(match[1]));
  }
};

/**
 * Extracts every gettext message used by PipeTune GTK C++ and GtkBuilder UI.
 *
 * @param {string} projectDirectory Absolute or relative pipetune-gtk root.
 * @returns {Map<string, {singular: string, plural: string | undefined}>}
 * Sorted message metadata keyed by singular msgid.
 */
export const extractMessages = (projectDirectory) => {
  const messages = new Map();
  const sourceDirectory = join(projectDirectory, 'src');
  for (const fileName of readdirSync(sourceDirectory).sort()) {
    if (!fileName.endsWith('.cpp')) {
      continue;
    }
    extractCppMessages(
      readFileSync(join(sourceDirectory, fileName), 'utf8'),
      messages
    );
  }
  extractBuilderMessages(
    readFileSync(join(projectDirectory, 'resources', 'main-window.ui'), 'utf8'),
    messages
  );
  return new Map(
    [...messages.entries()].sort(([left], [right]) =>
      left.localeCompare(right, 'en')
    )
  );
};

const decodePoString = (text) => JSON.parse(text);

/**
 * Parses the msgids and translations needed by the PipeTune GTK catalogs.
 *
 * @param {string} source PO or POT source text.
 * @returns {Map<string, {singular: string, plural: string | undefined, translations: Map<number, string>}>}
 * Parsed entries keyed by singular msgid, excluding the catalog header.
 */
export const parseCatalog = (source) => {
  const entries = new Map();
  let entry = undefined;
  let activeField = undefined;
  const finishEntry = () => {
    if (entry?.singular !== undefined && entry.singular !== '') {
      entries.set(entry.singular, entry);
    }
    entry = undefined;
    activeField = undefined;
  };

  for (const line of source.split(/\r?\n/u)) {
    if (line === '') {
      finishEntry();
      continue;
    }
    if (line.startsWith('#')) {
      continue;
    }
    entry ??= { plural: undefined, translations: new Map() };
    const field = line.match(
      /^(msgid_plural|msgid|msgstr(?:\[(\d+)\])?)\s+(".*")$/u
    );
    if (field !== null) {
      const [, name, pluralIndex, encoded] = field;
      const value = decodePoString(encoded);
      if (name === 'msgid') {
        entry.singular = value;
        activeField = ['singular'];
      } else if (name === 'msgid_plural') {
        entry.plural = value;
        activeField = ['plural'];
      } else {
        const index = pluralIndex === undefined ? 0 : Number(pluralIndex);
        entry.translations.set(index, value);
        activeField = ['translation', index];
      }
      continue;
    }
    if (line.startsWith('"') && activeField !== undefined) {
      const value = decodePoString(line);
      if (activeField[0] === 'translation') {
        const index = activeField[1];
        entry.translations.set(
          index,
          `${entry.translations.get(index) ?? ''}${value}`
        );
      } else {
        entry[activeField[0]] = `${entry[activeField[0]] ?? ''}${value}`;
      }
      continue;
    }
    throw new Error(`unsupported catalog syntax: ${line}`);
  }
  finishEntry();
  return entries;
};

const quotePo = (value) => JSON.stringify(value);

/**
 * Languages whose message catalogs are distributed with PipeTune GTK.
 */
export const catalogLocales = [
  {
    id: 'ar',
    languageTeam: 'Arabic',
    pluralCount: 6,
    pluralForms:
      'nplurals=6; plural=(n==0 ? 0 : n==1 ? 1 : n==2 ? 2 : n%100>=3 && n%100<=10 ? 3 : n%100>=11 && n%100<=99 ? 4 : 5);',
  },
  {
    id: 'es',
    languageTeam: 'Spanish',
    pluralCount: 2,
    pluralForms: 'nplurals=2; plural=(n != 1);',
  },
  {
    id: 'fr',
    languageTeam: 'French',
    pluralCount: 2,
    pluralForms: 'nplurals=2; plural=(n > 1);',
  },
  {
    id: 'hi',
    languageTeam: 'Hindi',
    pluralCount: 2,
    pluralForms: 'nplurals=2; plural=(n != 1);',
  },
  {
    id: 'ja',
    languageTeam: 'Japanese',
    pluralCount: 1,
    pluralForms: 'nplurals=1; plural=0;',
  },
  {
    id: 'ko',
    languageTeam: 'Korean',
    pluralCount: 1,
    pluralForms: 'nplurals=1; plural=0;',
  },
  {
    id: 'pt',
    languageTeam: 'Portuguese',
    pluralCount: 2,
    pluralForms: 'nplurals=2; plural=(n != 1);',
  },
  {
    id: 'ru',
    languageTeam: 'Russian',
    pluralCount: 3,
    pluralForms:
      'nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);',
  },
  {
    id: 'zh',
    languageTeam: 'Chinese',
    pluralCount: 1,
    pluralForms: 'nplurals=1; plural=0;',
  },
];

const catalogHeader = (template, locale) =>
  template
    ? [
        'msgid ""',
        'msgstr ""',
        '"Project-Id-Version: PipeTune GTK\\n"',
        '"Report-Msgid-Bugs-To: \\n"',
        '"POT-Creation-Date: 2026-07-31 00:00+0900\\n"',
        '"MIME-Version: 1.0\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        '',
      ]
    : [
        'msgid ""',
        'msgstr ""',
        '"Project-Id-Version: PipeTune GTK\\n"',
        '"Report-Msgid-Bugs-To: \\n"',
        '"POT-Creation-Date: 2026-07-31 00:00+0900\\n"',
        '"PO-Revision-Date: 2026-07-31 00:00+0900\\n"',
        '"Last-Translator: PipeTune contributors\\n"',
        `"Language-Team: ${locale.languageTeam}\\n"`,
        `"Language: ${locale.id}\\n"`,
        '"MIME-Version: 1.0\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        `"Plural-Forms: ${locale.pluralForms}\\n"`,
        '',
      ];

/**
 * Serializes a deterministic PipeTune GTK POT or translated PO catalog.
 *
 * @param {Map<string, {singular: string, plural: string | undefined}>} messages Extracted messages.
 * @param {Map<string, {translations: Map<number, string>}>} translations Existing translations.
 * @param {boolean} template True for POT output, false for translated PO.
 * @param {{id: string, languageTeam: string, pluralCount: number, pluralForms: string} | undefined} locale Catalog locale, omitted for POT output.
 * @returns {string} Complete deterministic catalog source.
 */
export const serializeCatalog = (
  messages,
  translations,
  template,
  locale = undefined
) => {
  if (!template && locale === undefined) {
    throw new Error('a translated catalog requires locale metadata');
  }
  const lines = catalogHeader(template, locale);
  for (const message of messages.values()) {
    const retained = translations.get(message.singular);
    if (/\{\d+\}/u.test(message.singular)) {
      lines.push('#, brace-format');
    }
    lines.push(`msgid ${quotePo(message.singular)}`);
    if (message.plural === undefined) {
      lines.push(
        `msgstr ${quotePo(
          template ? '' : (retained?.translations.get(0) ?? '')
        )}`
      );
    } else {
      lines.push(`msgid_plural ${quotePo(message.plural)}`);
      const pluralCount = template ? 2 : locale.pluralCount;
      for (let index = 0; index < pluralCount; index += 1) {
        lines.push(
          `msgstr[${index}] ${quotePo(
            template ? '' : (retained?.translations.get(index) ?? '')
          )}`
        );
      }
    }
    lines.push('');
  }
  return lines.join('\n');
};

const placeholders = (value) =>
  [...new Set(value.match(/\{\d+\}/gu) ?? [])].sort();

/**
 * Validates catalog coverage, translations, plurals, and placeholders.
 *
 * @param {Map<string, {singular: string, plural: string | undefined}>} messages Extracted messages.
 * @param {Map<string, {singular: string, plural: string | undefined, translations: Map<number, string>}>} template Parsed POT.
 * @param {Array<{locale: {id: string, languageTeam: string, pluralCount: number, pluralForms: string}, catalog: Map<string, {singular: string, plural: string | undefined, translations: Map<number, string>}>}>} catalogs Parsed translated catalogs.
 * @returns {string[]} Human-readable validation failures.
 */
export const catalogProblems = (messages, template, catalogs) => {
  const problems = [];
  const expectedIds = [...messages.keys()];
  for (const [name, catalog] of [
    ['POT', template],
    ...catalogs.map(({ locale, catalog }) => [
      `${locale.languageTeam} PO`,
      catalog,
    ]),
  ]) {
    for (const id of expectedIds) {
      if (!catalog.has(id)) {
        problems.push(`${name} is missing: ${id}`);
      }
    }
    for (const id of catalog.keys()) {
      if (!messages.has(id)) {
        problems.push(`${name} has an obsolete message: ${id}`);
      }
    }
  }
  for (const { locale, catalog } of catalogs) {
    for (const message of messages.values()) {
      const entry = catalog.get(message.singular);
      const expectedTranslationCount =
        message.plural === undefined ? 1 : locale.pluralCount;
      for (let index = 0; index < expectedTranslationCount; index += 1) {
        const translation = entry?.translations.get(index);
        if (translation === undefined || translation === '') {
          problems.push(
            `${locale.languageTeam} translation ${index} is empty: ${message.singular}`
          );
          continue;
        }
        const source =
          index === 0 || message.plural === undefined
            ? message.singular
            : message.plural;
        const expectedPlaceholders = placeholders(source);
        const translatedPlaceholders = placeholders(translation);
        if (
          expectedPlaceholders.join('\u0000') !==
          translatedPlaceholders.join('\u0000')
        ) {
          problems.push(
            `${locale.languageTeam} placeholders differ: ${message.singular}`
          );
        }
      }
      for (const index of entry?.translations.keys() ?? []) {
        if (index >= expectedTranslationCount) {
          problems.push(
            `${locale.languageTeam} has an extra plural form: ${message.singular}`
          );
        }
      }
      if (entry?.plural !== message.plural) {
        problems.push(
          `${locale.languageTeam} plural metadata differs: ${message.singular}`
        );
      }
    }
  }
  for (const message of messages.values()) {
    const templatePlural = template.get(message.singular)?.plural;
    if (templatePlural !== message.plural) {
      problems.push(`POT plural metadata differs: ${message.singular}`);
    }
  }
  return problems;
};
