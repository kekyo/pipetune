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

const catalogHeader = (template) =>
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
        '"Language-Team: Japanese\\n"',
        '"Language: ja\\n"',
        '"MIME-Version: 1.0\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        '"Plural-Forms: nplurals=1; plural=0;\\n"',
        '',
      ];

/**
 * Serializes a deterministic PipeTune GTK POT or Japanese PO catalog.
 *
 * @param {Map<string, {singular: string, plural: string | undefined}>} messages Extracted messages.
 * @param {Map<string, {translations: Map<number, string>}>} translations Existing translations.
 * @param {boolean} template True for POT output, false for Japanese PO.
 * @returns {string} Complete deterministic catalog source.
 */
export const serializeCatalog = (messages, translations, template) => {
  const lines = catalogHeader(template);
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
      lines.push(
        `msgstr[0] ${quotePo(
          template ? '' : (retained?.translations.get(0) ?? '')
        )}`
      );
      if (template) {
        lines.push('msgstr[1] ""');
      }
    }
    lines.push('');
  }
  return lines.join('\n');
};

const placeholders = (value) =>
  [...new Set(value.match(/\{\d+\}/gu) ?? [])].sort();

/**
 * Validates catalog coverage, Japanese translations, and numbered placeholders.
 *
 * @param {Map<string, {singular: string, plural: string | undefined}>} messages Extracted messages.
 * @param {Map<string, {singular: string, plural: string | undefined, translations: Map<number, string>}>} template Parsed POT.
 * @param {Map<string, {singular: string, plural: string | undefined, translations: Map<number, string>}>} japanese Parsed Japanese PO.
 * @returns {string[]} Human-readable validation failures.
 */
export const catalogProblems = (messages, template, japanese) => {
  const problems = [];
  const expectedIds = [...messages.keys()];
  for (const [name, catalog] of [
    ['POT', template],
    ['Japanese PO', japanese],
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
  for (const message of messages.values()) {
    const translation = japanese.get(message.singular)?.translations.get(0);
    if (translation === undefined || translation === '') {
      problems.push(`Japanese translation is empty: ${message.singular}`);
      continue;
    }
    const expectedPlaceholders = placeholders(message.singular);
    const translatedPlaceholders = placeholders(translation);
    if (
      expectedPlaceholders.join('\u0000') !==
      translatedPlaceholders.join('\u0000')
    ) {
      problems.push(`Japanese placeholders differ: ${message.singular}`);
    }
    const templatePlural = template.get(message.singular)?.plural;
    const japanesePlural = japanese.get(message.singular)?.plural;
    if (
      templatePlural !== message.plural ||
      japanesePlural !== message.plural
    ) {
      problems.push(`plural metadata differs: ${message.singular}`);
    }
  }
  return problems;
};
