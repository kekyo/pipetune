import {
  mkdirSync,
  readFileSync,
  renameSync,
  writeFileSync,
} from 'node:fs';
import { dirname } from 'node:path';

const [inputPath, outputPath] = process.argv.slice(2);
if (!inputPath || !outputPath) {
  throw new Error('usage: compile-po.mjs INPUT.po OUTPUT.mo');
}

const decodeQuoted = (text) => JSON.parse(text);

const entries = [];
let entry = undefined;
let activeField = undefined;

const finishEntry = () => {
  if (!entry || entry.fuzzy || entry.msgid === undefined) {
    entry = undefined;
    activeField = undefined;
    return;
  }
  const originalPrefix =
    entry.msgctxt === undefined ? '' : `${entry.msgctxt}\u0004`;
  const original =
    entry.msgidPlural === undefined
      ? `${originalPrefix}${entry.msgid}`
      : `${originalPrefix}${entry.msgid}\u0000${entry.msgidPlural}`;
  const translated =
    entry.msgidPlural === undefined
      ? (entry.msgstr.get(0) ?? '')
      : [...entry.msgstr.entries()]
          .sort(([left], [right]) => left - right)
          .map(([, value]) => value)
          .join('\u0000');
  entries.push({ original, translated });
  entry = undefined;
  activeField = undefined;
};

for (const line of readFileSync(inputPath, 'utf8').split(/\r?\n/u)) {
  if (line.length === 0) {
    finishEntry();
    continue;
  }
  if (line.startsWith('#,')) {
    entry ??= { msgstr: new Map(), fuzzy: false };
    entry.fuzzy ||= line
      .slice(2)
      .split(',')
      .some((flag) => flag.trim() === 'fuzzy');
    continue;
  }
  if (line.startsWith('#')) {
    continue;
  }
  entry ??= { msgstr: new Map(), fuzzy: false };
  const field = line.match(
    /^(msgctxt|msgid_plural|msgid|msgstr(?:\[(\d+)\])?)\s+(".*")$/u
  );
  if (field) {
    const [, name, pluralIndex, value] = field;
    const decoded = decodeQuoted(value);
    if (name === 'msgctxt') {
      entry.msgctxt = decoded;
      activeField = ['msgctxt'];
    } else if (name === 'msgid') {
      entry.msgid = decoded;
      activeField = ['msgid'];
    } else if (name === 'msgid_plural') {
      entry.msgidPlural = decoded;
      activeField = ['msgidPlural'];
    } else {
      const index = pluralIndex === undefined ? 0 : Number(pluralIndex);
      entry.msgstr.set(index, decoded);
      activeField = ['msgstr', index];
    }
    continue;
  }
  if (line.startsWith('"') && activeField) {
    const decoded = decodeQuoted(line);
    if (activeField[0] === 'msgstr') {
      const index = activeField[1];
      entry.msgstr.set(index, `${entry.msgstr.get(index)}${decoded}`);
    } else {
      entry[activeField[0]] = `${entry[activeField[0]]}${decoded}`;
    }
    continue;
  }
  throw new Error(`unsupported PO syntax: ${line}`);
}
finishEntry();

entries.sort((left, right) =>
  Buffer.from(left.original).compare(Buffer.from(right.original))
);

const count = entries.length;
const headerSize = 7 * 4;
const originalTableOffset = headerSize;
const translatedTableOffset = originalTableOffset + count * 8;
const stringsOffset = translatedTableOffset + count * 8;
const originals = entries.map(({ original }) =>
  Buffer.from(`${original}\u0000`, 'utf8')
);
const translations = entries.map(({ translated }) =>
  Buffer.from(`${translated}\u0000`, 'utf8')
);
const originalBytes = originals.reduce((total, value) => total + value.length, 0);
const output = Buffer.alloc(
  stringsOffset +
    originalBytes +
    translations.reduce((total, value) => total + value.length, 0)
);
output.writeUInt32LE(0x950412de, 0);
output.writeUInt32LE(0, 4);
output.writeUInt32LE(count, 8);
output.writeUInt32LE(originalTableOffset, 12);
output.writeUInt32LE(translatedTableOffset, 16);
output.writeUInt32LE(0, 20);
output.writeUInt32LE(0, 24);

let originalOffset = stringsOffset;
let translatedOffset = stringsOffset + originalBytes;
for (let index = 0; index < count; index += 1) {
  const original = originals[index];
  const translated = translations[index];
  output.writeUInt32LE(original.length - 1, originalTableOffset + index * 8);
  output.writeUInt32LE(originalOffset, originalTableOffset + index * 8 + 4);
  output.writeUInt32LE(
    translated.length - 1,
    translatedTableOffset + index * 8
  );
  output.writeUInt32LE(
    translatedOffset,
    translatedTableOffset + index * 8 + 4
  );
  original.copy(output, originalOffset);
  translated.copy(output, translatedOffset);
  originalOffset += original.length;
  translatedOffset += translated.length;
}

mkdirSync(dirname(outputPath), { recursive: true });
const temporaryPath = `${outputPath}.tmp-${process.pid}`;
writeFileSync(temporaryPath, output);
renameSync(temporaryPath, outputPath);
