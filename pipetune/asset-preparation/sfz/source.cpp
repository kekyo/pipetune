/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */

/*
 * Derived from EffeTune v2.14.0 js/sfz/source.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. See LICENSE.effetune.
 */
#include "source.h"
#include "text.h"
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unicode/ucol.h>
#include <unicode/ustring.h>

namespace pipetune::assets {

static void appendUtf8(std::string &text, std::uint32_t point) {
  if (point < 0x80) text.push_back(static_cast<char>(point));
  else if (point < 0x800) {
    text.push_back(static_cast<char>(0xc0 | (point >> 6)));
    text.push_back(static_cast<char>(0x80 | (point & 0x3f)));
  } else {
    text.push_back(static_cast<char>(0xe0 | (point >> 12)));
    text.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
    text.push_back(static_cast<char>(0x80 | (point & 0x3f)));
  }
}

std::string decodeSfzText(std::span<const std::uint8_t> bytes) {
  auto utf8 = bytes.empty() ? std::string_view{} :
      std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  try {
    static_cast<void>(sfzUtf16(utf8));
    if (utf8.starts_with("\xef\xbb\xbf")) utf8.remove_prefix(3);
    return std::string(utf8);
  } catch (const SfzError &error) {
    if (error.code != "prepare") throw;
  }
  constexpr auto extended = std::array<std::uint16_t, 32>{
      0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
      0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
      0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
      0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178};
  auto decoded = std::string{};
  decoded.reserve(bytes.size() * 3);
  for (const auto byte : bytes)
    appendUtf8(decoded, byte >= 0x80 && byte < 0xa0 ? extended[byte - 0x80] : byte);
  return decoded;
}

static std::string directoryOf(std::string_view path) {
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? std::string{} : std::string(path.substr(0, slash));
}

static std::string lowercase(std::string_view text) {
  static_assert(std::is_same_v<UChar, char16_t>);
  const auto source = sfzUtf16(text);
  if (source.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
    throw SfzError("prepare", "SFZ bank path is too long.");
  auto error = U_ZERO_ERROR;
  const auto length = u_strToLower(nullptr, 0, reinterpret_cast<const UChar *>(source.data()),
      static_cast<int32_t>(source.size()), "", &error);
  if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error))
    throw SfzError("prepare", "SFZ bank path case conversion failed.");
  auto lowered = std::u16string(static_cast<std::size_t>(length), u'\0');
  error = U_ZERO_ERROR;
  u_strToLower(reinterpret_cast<UChar *>(lowered.data()), length,
      reinterpret_cast<const UChar *>(source.data()), static_cast<int32_t>(source.size()), "", &error);
  if (U_FAILURE(error)) throw SfzError("prepare", "SFZ bank path case conversion failed.");
  auto result = std::string{};
  for (auto index = std::size_t{0}; index < lowered.size(); ++index) {
    auto point = static_cast<std::uint32_t>(lowered[index]);
    if (point >= 0xd800 && point <= 0xdbff) {
      const auto low = static_cast<std::uint32_t>(lowered[++index]);
      point = 0x10000 + ((point - 0xd800) << 10) + (low - 0xdc00);
    }
    if (point < 0x10000) appendUtf8(result, point);
    else {
      result.push_back(static_cast<char>(0xf0 | (point >> 18)));
      result.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    }
  }
  return result;
}

static std::string asciiLower(std::string text) {
  for (auto &ch : text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return text;
}

static bool asciiName(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static bool asciiWord(char ch) {
  return asciiName(ch) || (ch >= '0' && ch <= '9');
}

static bool asciiMatch(std::string_view text, std::size_t at, std::string_view expected) {
  if (expected.size() > text.size() - at) return false;
  for (auto index = std::size_t{0}; index < expected.size(); ++index) {
    const auto ch = text[at + index];
    if ((ch >= 'A' && ch <= 'Z' ? ch + 'a' - 'A' : ch) != expected[index]) return false;
  }
  return true;
}

// ECMAScript's \s includes these Unicode whitespace and line terminators.
static std::size_t javascriptWhitespace(std::string_view text) {
  if (text.empty()) return 0;
  if (std::string_view(" \t\r\n\v\f").find(text.front()) != std::string_view::npos) return 1;
  constexpr auto sequences = std::array<std::string_view, 19>{
      "\xc2\xa0", "\xe1\x9a\x80", "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82",
      "\xe2\x80\x83", "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x87",
      "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8", "\xe2\x80\xa9",
      "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80", "\xef\xbb\xbf"};
  for (const auto sequence : sequences) if (text.starts_with(sequence)) return sequence.size();
  return 0;
}

static std::string decodeEntities(std::string_view source) {
  constexpr auto entities = std::array<std::pair<std::string_view, char>, 5>{
      std::pair{"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}};
  auto result = std::string{};
  result.reserve(source.size());
  for (auto index = std::size_t{0}; index < source.size(); ++index) {
    auto replaced = false;
    if (source[index] == '&') for (const auto &[entity, value] : entities) {
      if (!source.substr(index).starts_with(entity)) continue;
      result.push_back(value);
      index += entity.size() - 1;
      replaced = true;
      break;
    }
    if (!replaced) result.push_back(source[index]);
  }
  return result;
}

static std::vector<std::pair<std::string, std::string>> attributes(std::string_view source) {
  auto values = std::vector<std::pair<std::string, std::string>>{};
  for (auto start = std::size_t{0}; start < source.size(); ++start) {
    if (!asciiName(source[start]) || (start && asciiWord(source[start - 1]))) continue;
    auto end = start;
    while (end < source.size() && asciiName(source[end])) ++end;
    auto equals = end;
    while (equals < source.size()) {
      const auto width = javascriptWhitespace(source.substr(equals));
      if (!width) break;
      equals += width;
    }
    if (equals == source.size() || source[equals++] != '=') continue;
    while (equals < source.size()) {
      const auto width = javascriptWhitespace(source.substr(equals));
      if (!width) break;
      equals += width;
    }
    if (equals == source.size() || (source[equals] != '"' && source[equals] != '\'')) continue;
    const auto quote = source[equals++];
    const auto close = source.find(quote, equals);
    if (close == std::string_view::npos) continue;
    const auto key = asciiLower(std::string(source.substr(start, end - start)));
    const auto value = decodeEntities(source.substr(equals, close - equals));
    const auto existing = std::ranges::find(values, key, &std::pair<std::string, std::string>::first);
    if (existing == values.end()) values.emplace_back(key, value);
    else existing->second = value;
    start = close;
  }
  return values;
}

struct SourceTag {
  std::string name;
  std::vector<std::pair<std::string, std::string>> values;
};

static bool hasAriaBank(std::string_view text) {
  for (auto at = text.find('<'); at != std::string_view::npos; at = text.find('<', at + 1)) {
    const auto end = at + 1 + std::string_view("ariabank").size();
    if (asciiMatch(text, at + 1, "ariabank") && (end == text.size() || !asciiWord(text[end]))) return true;
  }
  return false;
}

static std::vector<SourceTag> sourceTags(std::string_view text) {
  auto tags = std::vector<SourceTag>{};
  for (auto at = text.find('<'); at != std::string_view::npos;) {
    const auto name = asciiMatch(text, at + 1, "ariaelement") ? std::string_view("ariaelement") :
        asciiMatch(text, at + 1, "define") ? std::string_view("define") : std::string_view{};
    if (name.empty()) { at = text.find('<', at + 1); continue; }
    const auto start = at + 1 + name.size();
    if (start < text.size() && asciiWord(text[start])) { at = text.find('<', at + 1); continue; }
    const auto end = text.find_first_of("<>", start);
    if (end == std::string_view::npos) break;
    if (text[end] == '>') {
      tags.push_back({std::string(name), attributes(text.substr(start, end - start))});
      at = text.find('<', end + 1);
    } else at = end;
  }
  return tags;
}

static bool validDefineName(std::string_view key) {
  if (key.size() < 2 || key.front() != '$') return false;
  return std::ranges::all_of(key.substr(1), asciiWord);
}

static const std::string *attribute(const std::vector<std::pair<std::string, std::string>> &values,
    std::string_view key) {
  const auto found = std::ranges::find(values, key, &std::pair<std::string, std::string>::first);
  return found == values.end() ? nullptr : &found->second;
}

static std::vector<std::string> splitPath(std::string_view path) {
  auto parts = std::vector<std::string>{};
  for (auto start = std::size_t{0}; start <= path.size();) {
    const auto end = path.find('/', start);
    parts.emplace_back(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return parts;
}

static std::string sampleDirectory(std::string_view selectedPath, std::string_view bankDirectory) {
  auto source = splitPath(selectedPath);
  source.pop_back();
  const auto samples = splitPath(normalizeSfzPath("Samples", bankDirectory));
  auto common = std::size_t{0};
  while (common < source.size() && common < samples.size() && source[common] == samples[common]) ++common;
  auto value = std::string{};
  for (auto index = common; index < source.size(); ++index) value += "../";
  for (auto index = common; index < samples.size(); ++index) {
    if (index != common) value += '/';
    value += samples[index];
  }
  return value;
}

SfzBankDefines readSfzBankDefines(std::string_view selectedPath,
    std::span<const std::string> bankPaths, std::span<const SfzDocument> documents) {
  struct Candidate { std::string path; std::string directory; std::size_t depth; };
  auto candidates = std::vector<Candidate>{};
  for (const auto &path : bankPaths) {
    static_cast<void>(sfzUtf16(path));
    const auto directory = directoryOf(path);
    if (directory.empty() || selectedPath.starts_with(directory + '/'))
      candidates.push_back({path, directory, sfzUtf16(directory).size()});
  }
  auto error = U_ZERO_ERROR;
  auto *opened = ucol_open(nullptr, &error);
  if (U_FAILURE(error) || opened == nullptr) throw SfzError("prepare", "SFZ bank collation is unavailable.");
  auto collator = std::unique_ptr<UCollator, decltype(&ucol_close)>(opened, ucol_close);
  error = U_ZERO_ERROR;
  const auto *valid = ucol_getLocaleByType(collator.get(), ULOC_VALID_LOCALE, &error);
  if (U_FAILURE(error)) throw SfzError("prepare", "SFZ bank locale is unavailable.");
  if (valid != nullptr && std::string_view(valid) == "en_US_POSIX") {
    error = U_ZERO_ERROR;
    collator.reset(ucol_open("en_US", &error));
    if (U_FAILURE(error) || !collator) throw SfzError("prepare", "SFZ bank collation is unavailable.");
  }
  error = U_ZERO_ERROR;
  // V8's default sort uses variant sensitivity and normalized comparisons.
  ucol_setAttribute(collator.get(), UCOL_STRENGTH, UCOL_TERTIARY, &error);
  ucol_setAttribute(collator.get(), UCOL_NORMALIZATION_MODE, UCOL_ON, &error);
  if (U_FAILURE(error)) throw SfzError("prepare", "SFZ bank collation is unavailable.");
  std::stable_sort(candidates.begin(), candidates.end(), [&](const Candidate &left, const Candidate &right) {
    if (left.depth != right.depth) return left.depth > right.depth;
    auto status = U_ZERO_ERROR;
    if (left.path.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()) ||
        right.path.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
      throw SfzError("prepare", "SFZ bank path is too long.");
    const auto comparison = ucol_strcollUTF8(collator.get(), left.path.data(), static_cast<int32_t>(left.path.size()),
        right.path.data(), static_cast<int32_t>(right.path.size()), &status);
    if (U_FAILURE(status)) throw SfzError("prepare", "SFZ bank collation failed.");
    return comparison == UCOL_LESS;
  });
  auto result = SfzBankDefines{};
  for (const auto &candidate : candidates) {
    const auto found = std::ranges::find(documents, candidate.path, &SfzDocument::path);
    if (found == documents.end())
      throw SfzError("prepare", "An SFZ bank definition has not been supplied.", candidate.path);
    result.dependencies.push_back(candidate.path);
    if (found->text.empty() || !hasAriaBank(found->text)) continue;
    const auto tags = sourceTags(found->text);
    auto registered = false;
    for (const auto &item : tags) {
      const auto *path = attribute(item.values, "path");
      if (item.name == "ariaelement" && path != nullptr &&
          lowercase(normalizeSfzPath(*path, candidate.directory)) == lowercase(selectedPath)) {
        registered = true;
        break;
      }
    }
    if (!registered && std::ranges::count(candidates, candidate.directory, &Candidate::directory) != 1) continue;
    for (const auto &item : tags) {
      const auto *key = attribute(item.values, "name");
      const auto *sourceValue = attribute(item.values, "value");
      if (item.name != "define" || key == nullptr || sourceValue == nullptr ||
          !validDefineName(*key)) continue;
      auto value = *sourceValue;
      if (*key == "$sample_dir" && value.starts_with("$Banks/"))
        value = sampleDirectory(selectedPath, candidate.directory);
      const auto existing = std::ranges::find(result.defines, *key,
          &std::pair<std::string, std::string>::first);
      if (existing == result.defines.end()) result.defines.emplace_back(*key, std::move(value));
      else existing->second = std::move(value);
    }
    result.bankPath = candidate.path;
    return result;
  }
  return result;
}

} // namespace pipetune::assets
