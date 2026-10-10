/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */

/*
 * Derived from EffeTune v2.13.0 js/sfz/parser.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. See LICENSE.effetune.
 */
#include "parser.h"
#include "text.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <regex>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace pipetune::assets {

std::string normalizeSfzPath(std::string_view value, std::string_view directory) {
  auto path = std::string(value);
  std::ranges::replace(path, '\\', '/');
  if (path.starts_with('/') || path.find('\0') != std::string::npos ||
      (path.size() >= 2 && ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) && path[1] == ':'))
    throw SfzError("prepare", "SFZ references must stay inside the selected folder.");
  auto result = std::string(directory);
  for (auto start = std::size_t{0}; start <= path.size();) {
    const auto end = path.find('/', start);
    const auto part = std::string_view(path).substr(start, end == std::string::npos ? path.size() - start : end - start);
    if (part == "..") {
      if (result.empty()) throw SfzError("prepare", "SFZ references must stay inside the selected folder.");
      const auto separator = result.rfind('/');
      result.resize(separator == std::string::npos ? 0 : separator);
    } else if (!part.empty() && part != ".") {
      if (!result.empty()) result += '/';
      result += part;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  static_cast<void>(sfzUtf16(result));
  return result;
}

// ECMAScript whitespace is wider than the C locale used by std::isspace.
static std::size_t whitespace(std::string_view text) {
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

static std::string_view trimmed(std::string_view value) {
  while (const auto count = whitespace(value)) value.remove_prefix(count);
  auto last = std::size_t{0};
  for (auto index = std::size_t{0}; index < value.size();) {
    const auto count = whitespace(value.substr(index));
    if (count) index += count;
    else last = ++index;
  }
  return value.substr(0, last);
}

static std::string unquoted(std::string_view value) {
  return !value.empty() && value.front() == '"' && value.back() == '"' ?
      std::string(value.substr(1, value.size() > 1 ? value.size() - 2 : 0)) : std::string(value);
}

static double number(std::string_view text) {
  text = trimmed(text);
  if (text.empty()) return 0;
  if (text.size() > 2 && text[0] == '0') {
    const auto prefix = text[1];
    const auto radix = prefix == 'x' || prefix == 'X' ? 16 : prefix == 'b' || prefix == 'B' ? 2 :
        prefix == 'o' || prefix == 'O' ? 8 : 0;
    if (radix) {
      auto result = 0.0;
      for (const auto ch : text.substr(2)) {
        const auto digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ?
            ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 16;
        if (digit >= radix) return NAN;
        result = result * radix + digit;
      }
      return result;
    }
  }
  if (text.front() == '+') {
    text.remove_prefix(1);
    if (text.empty() || text.front() == '-' || text.front() == '+') return NAN;
  }
  auto value = 0.0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec == std::errc::result_out_of_range && result.ptr == text.data() + text.size()) {
    // from_chars rejects underflow, while Number rounds it to signed zero.
    auto stream = std::istringstream(std::string(text));
    stream.imbue(std::locale::classic());
    stream >> value;
    return stream.fail() ? NAN : value;
  }
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : NAN;
}

static double midi(std::string_view text) {
  static const auto numeric = std::regex("^-?[0-9]+$");
  if (std::regex_match(text.begin(), text.end(), numeric)) return number(text);
  static const auto note = std::regex("^([a-g])([#b]?)(-?[0-9]+)$", std::regex::icase);
  auto match = std::match_results<std::string_view::const_iterator>{};
  if (!std::regex_match(text.begin(), text.end(), match, note)) return NAN;
  const auto name = static_cast<char>(std::tolower(static_cast<unsigned char>(match[1].str()[0])));
  constexpr auto pitches = std::array{9, 11, 0, 2, 4, 5, 7};
  return (number(match[3].str()) + 1) * 12 + pitches[name - 'a'] +
      (match[2].str() == "#" ? 1 : match[2].str() == "b" ? -1 : 0);
}

static bool identifier(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

static std::string lowercase(std::string_view text) {
  auto result = std::string(text);
  for (auto &ch : result) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return result;
}

static std::string directoryOf(std::string_view path) {
  const auto separator = path.rfind('/');
  return separator == std::string_view::npos ? std::string{} : std::string(path.substr(0, separator));
}

static void definitionTooLarge() {
  throw SfzError("too-large", "The SFZ definition is too large to load.");
}

static std::string stripComments(std::string_view text) {
  auto result = std::string{};
  result.reserve(text.size());
  auto quoted = false;
  for (auto index = std::size_t{0}; index < text.size(); ++index) {
    const auto ch = text[index];
    if (ch == '"') quoted = !quoted;
    if (!quoted && text.substr(index, 2) == "//") {
      while (index < text.size() && text[index] != '\n') ++index;
      result += '\n';
    } else if (!quoted && text.substr(index, 2) == "/*") {
      index += 2;
      while (index < text.size() && text.substr(index, 2) != "*/") ++index;
      ++index;
      result += ' ';
    } else result += ch;
  }
  return result;
}

struct Expansion {
  std::unordered_map<std::string, const SfzDocument *> documents;
  std::unordered_map<std::string, std::string> defines;
  std::vector<std::string> dependencies;
  std::vector<std::string> ancestors;
  std::uint64_t maximumBytes;
  std::uint64_t workingBytes;
  std::uint64_t initialWorkingBytes;
  std::uint64_t sourceChars = 0;
  std::uint64_t expandedChars = 0;
  std::uint32_t visits = 0;
};

// Charge conservative allocation work before expansion and inherited-region copies.
// This intentionally overestimates simultaneous storage, including string/container
// capacity growth and UTF-16/diagnostic temporaries. The host owns the actual lease.
static void chargeWorking(Expansion &state, std::uint64_t bytes, std::uint64_t multiplier) {
  if (bytes > state.workingBytes / multiplier) {
    auto error = SfzError("too-large", "The SFZ preparation exceeds its memory budget.");
    const auto used = state.initialWorkingBytes - state.workingBytes;
    error.minimumWorkingBytes = bytes > (UINT64_MAX - used) / multiplier ? UINT64_MAX : used + bytes * multiplier;
    throw error;
  }
  state.workingBytes -= bytes * multiplier;
}

static std::string expandDefines(Expansion &state, std::string_view text) {
  auto length = static_cast<std::uint64_t>(sfzUtf16(text).size());
  if (length > state.maximumBytes - state.expandedChars) definitionTooLarge();
  auto result = std::string{};
  auto last = std::size_t{0};
  for (auto index = std::size_t{0}; index < text.size(); ++index) {
    if (text[index] != '$' || index + 1 == text.size() || !identifier(text[index + 1])) continue;
    auto end = index + 1;
    while (end < text.size() && identifier(text[end])) ++end;
    const auto key = std::string(text.substr(index, end - index));
    const auto found = state.defines.find(key);
    const auto &value = found == state.defines.end() ? key : found->second;
    chargeWorking(state, value.size(), 16);
    length = length - key.size() + sfzUtf16(value).size();
    if (length > state.maximumBytes - state.expandedChars) definitionTooLarge();
    result.append(text.substr(last, index - last));
    result += value;
    last = end;
    index = end - 1;
  }
  result.append(text.substr(last));
  state.expandedChars += length + 1;
  if (state.expandedChars > state.maximumBytes) definitionTooLarge();
  return result;
}

static void expandDocument(Expansion &state, const std::string &path, std::string &output) {
  if (state.ancestors.size() >= 32 || std::ranges::find(state.ancestors, path) != state.ancestors.end())
    throw SfzError("prepare", "SFZ includes are recursive or too deep.");
  if (++state.visits > 10000) definitionTooLarge();
  const auto found = state.documents.find(path);
  if (found == state.documents.end()) throw SfzError("prepare", "An SFZ include could not be found.", path);
  const auto &text = found->second->text;
  chargeWorking(state, text.size(), 16);
  state.sourceChars += sfzUtf16(text).size();
  if (state.sourceChars > state.maximumBytes) definitionTooLarge();
  state.dependencies.push_back(path);
  state.ancestors.push_back(path);
  const auto source = stripComments(text);
  for (auto start = std::size_t{0}; start <= source.size();) {
    const auto end = source.find('\n', start);
    auto line = std::string_view(source).substr(start, end == std::string::npos ? source.size() - start : end - start);
    if (end != std::string::npos && line.ends_with('\r')) line.remove_suffix(1);
    auto directive = line;
    while (const auto count = whitespace(directive)) directive.remove_prefix(count);
    auto keywordEnd = std::size_t{0};
    while (keywordEnd < directive.size() && !whitespace(directive.substr(keywordEnd))) ++keywordEnd;
    const auto keyword = lowercase(directive.substr(0, keywordEnd));
    auto arguments = directive.substr(keywordEnd);
    while (const auto count = whitespace(arguments)) arguments.remove_prefix(count);
    auto handled = false;
    if (keyword == "#define" && arguments.starts_with('$')) {
      auto nameEnd = std::size_t{1};
      while (nameEnd < arguments.size() && identifier(arguments[nameEnd])) ++nameEnd;
      auto value = arguments.substr(nameEnd);
      const auto separated = whitespace(value) != 0;
      while (const auto count = whitespace(value)) value.remove_prefix(count);
      if (nameEnd > 1 && separated && !value.empty() && value.find_first_of("\r\n") == std::string_view::npos &&
          value.find("\xe2\x80\xa8") == std::string_view::npos && value.find("\xe2\x80\xa9") == std::string_view::npos) {
        const auto expanded = expandDefines(state, unquoted(trimmed(value)));
        state.defines[std::string(arguments.substr(0, nameEnd))] = expanded;
        handled = true;
      }
    } else if (keyword == "#include" && keywordEnd < directive.size() && !arguments.empty() &&
        arguments.find_first_of("\r\n") == std::string_view::npos &&
        arguments.find("\xe2\x80\xa8") == std::string_view::npos && arguments.find("\xe2\x80\xa9") == std::string_view::npos) {
      const auto included = normalizeSfzPath(unquoted(expandDefines(state, trimmed(arguments))), directoryOf(path));
      expandDocument(state, included, output);
      handled = true;
    }
    if (!handled) {
      output += expandDefines(state, line);
      output += '\n';
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  state.ancestors.pop_back();
}

// Preserve JS object insertion order so that the first failing opcode and key aliases agree.
using RawRegion = std::vector<std::pair<std::string, std::string>>;

static void assign(RawRegion &raw, std::string_view key, const std::string &value) {
  const auto found = std::ranges::find_if(raw, [&](const auto &item) { return item.first == key; });
  if (found == raw.end()) raw.emplace_back(key, value);
  else found->second = value;
}

static std::string rawSample(const RawRegion &raw) {
  const auto found = std::ranges::find_if(raw, [](const auto &item) { return item.first == "sample"; });
  return found == raw.end() ? std::string{} : found->second;
}

struct NumericOpcode {
  const char *name;
  double SfzRegion::*member;
  double minimum;
  double maximum;
  bool integer;
  bool note;
};

static constexpr auto numericOpcodes = std::array{
    NumericOpcode{"lokey", &SfzRegion::lokey, 0, 127, true, true},
    NumericOpcode{"hikey", &SfzRegion::hikey, 0, 127, true, true},
    NumericOpcode{"lovel", &SfzRegion::lovel, 0, 127, true, false},
    NumericOpcode{"hivel", &SfzRegion::hivel, 1, 127, true, false},
    NumericOpcode{"lorand", &SfzRegion::lorand, 0, 1, false, false},
    NumericOpcode{"hirand", &SfzRegion::hirand, 0, 1, false, false},
    NumericOpcode{"seq_length", &SfzRegion::seq_length, 1, 16777216, true, false},
    NumericOpcode{"seq_position", &SfzRegion::seq_position, 1, 16777216, true, false},
    NumericOpcode{"pitch_keycenter", &SfzRegion::pitch_keycenter, 0, 127, true, true},
    NumericOpcode{"pitch_keytrack", &SfzRegion::pitch_keytrack, -1200, 1200, false, false},
    NumericOpcode{"transpose", &SfzRegion::transpose, -127, 127, true, false},
    NumericOpcode{"tune", &SfzRegion::tune, -1200, 1200, false, false},
    NumericOpcode{"volume", &SfzRegion::volume, -144, 144, false, false},
    NumericOpcode{"pan", &SfzRegion::pan, -100, 100, false, false},
    NumericOpcode{"amp_veltrack", &SfzRegion::amp_veltrack, -100, 100, false, false},
    NumericOpcode{"offset", &SfzRegion::offset, 0, INFINITY, true, false},
    NumericOpcode{"loop_start", &SfzRegion::loop_start, -INFINITY, INFINITY, true, false},
    NumericOpcode{"ampeg_attack", &SfzRegion::ampeg_attack, 0, 100, false, false},
    NumericOpcode{"ampeg_hold", &SfzRegion::ampeg_hold, 0, 100, false, false},
    NumericOpcode{"ampeg_decay", &SfzRegion::ampeg_decay, 0, 100, false, false},
    NumericOpcode{"ampeg_sustain", &SfzRegion::ampeg_sustain, 0, 100, false, false},
    NumericOpcode{"ampeg_release", &SfzRegion::ampeg_release, 0, 100, false, false}};

static bool supported(std::string_view key) {
  return key == "sample" || key == "key" || key == "end" || key == "loop_end" || key == "loop_mode" ||
      std::ranges::find(numericOpcodes, key, &NumericOpcode::name) != numericOpcodes.end();
}

static SfzRegion normalizeRegion(const RawRegion &raw, std::string sample, std::uint32_t seqGroup) {
  auto region = SfzRegion{};
  region.sample = std::move(sample);
  region.seqGroup = seqGroup;
  for (const auto &[key, text] : raw) {
    if (!supported(key) || key == "sample") continue;
    const auto found = std::ranges::find(numericOpcodes, key, &NumericOpcode::name);
    auto value = found != numericOpcodes.end() && found->note ? midi(text) : number(text);
    if (key == "loop_mode") {
      constexpr auto modes = std::array{"no_loop", "one_shot", "loop_continuous", "loop_sustain"};
      const auto mode = std::ranges::find(modes, text);
      value = mode == modes.end() ? NAN : static_cast<double>(mode - modes.begin());
    }
    if (!std::isfinite(value)) throw SfzError("prepare", "Invalid SFZ opcode " + key + '.');
    if ((key == "end" || key == "loop_end" || (found != numericOpcodes.end() && found->integer)) &&
        std::trunc(value) != value) throw SfzError("prepare", "SFZ opcode " + key + " must be an integer.");
    if (key == "end") region.end = value;
    else if (key == "loop_end") region.loop_end = value;
    else if (key == "loop_mode") region.loop_mode = value;
    else if (found != numericOpcodes.end()) region.*(found->member) = value;
  }
  if (region.lovel == 0) region.lovel = 1;
  for (const auto &opcode : numericOpcodes) {
    const auto value = region.*(opcode.member);
    if (value < opcode.minimum || value > opcode.maximum)
      throw SfzError("prepare", "SFZ region contains an invalid parameter range.");
  }
  if (region.lokey > region.hikey || region.lovel > region.hivel || region.lorand > region.hirand ||
      region.seq_position > region.seq_length)
    throw SfzError("prepare", "SFZ region contains an invalid parameter range.");
  return region;
}

static bool conditionalOpcode(std::string_view key, std::string_view value) {
  if (key == "trigger") return value != "attack";
  static const auto condition = std::regex("^(on_)?(lo|hi)(cc|hdcc|realcc|oncc|bend|chanaft|polyaft|prog|chan|timer|bpm)");
  return std::regex_search(key.begin(), key.end(), condition) || key.starts_with("sw_") ||
      key == "sustain_sw" || key == "sostenuto_sw";
}

static std::optional<std::uint32_t> ccIndex(std::string_view key, std::string_view prefix) {
  if (!key.starts_with(prefix)) return std::nullopt;
  key.remove_prefix(prefix.size());
  if (key.empty() || key.size() > 3) return std::nullopt;
  auto index = std::uint32_t{0};
  for (const auto ch : key) {
    if (ch < '0' || ch > '9') return std::nullopt;
    index = index * 10 + ch - '0';
  }
  return index < 128 ? std::optional(index) : std::nullopt;
}

struct Marker {
  std::size_t start;
  std::size_t end;
  std::string_view name;
  bool header;
};

// Stream markers instead of materializing a match object for every opcode.
static std::optional<Marker> nextMarker(std::string_view text, std::size_t position) {
  while (position < text.size()) {
    const auto header = text[position] == '<';
    const auto begin = position + (header ? 1 : 0);
    auto end = begin;
    while (end < text.size() && identifier(text[end])) ++end;
    if (end == begin) { ++position; continue; }
    if (header && end < text.size() && text[end] == '>') return Marker{position, end + 1, text.substr(begin, end - begin), true};
    if (header) { ++position; continue; }
    auto equal = end;
    while (const auto count = whitespace(text.substr(equal))) equal += count;
    if (equal < text.size() && text[equal] == '=') return Marker{position, equal + 1, text.substr(begin, end - begin), false};
    position = end;
  }
  return std::nullopt;
}

static void sortUnique(std::vector<std::string> &values) {
  std::ranges::sort(values, [](const auto &a, const auto &b) { return sfzUtf16(a) < sfzUtf16(b); });
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

ParsedSfz parseSfz(std::string_view selectedPath, std::span<const SfzDocument> documents,
    std::optional<std::span<const std::string>> availableSamples, std::uint64_t maximumBytes,
    std::uint64_t maximumWorkingBytes) {
  if (maximumBytes == 0 || maximumBytes > kMaximumSfzBytes) throw SfzError("too-large", "Invalid SFZ size limit.");
  const auto selected = normalizeSfzPath(selectedPath, {});
  auto expansion = Expansion{{}, {}, {}, {}, maximumBytes, maximumWorkingBytes, maximumWorkingBytes};
  for (const auto &document : documents) {
    chargeWorking(expansion, document.path.size() + 256, 4);
    expansion.documents.try_emplace(document.path, &document);
  }
  auto text = std::string{};
  expandDocument(expansion, selected, text);
  auto result = ParsedSfz{};
  result.dependencies = std::move(expansion.dependencies);
  sortUnique(result.dependencies);
  auto initialCC = std::array<std::uint8_t, 128>{};
  initialCC[7] = 100; initialCC[10] = 64; initialCC[11] = 127;
  auto initialSwitch = std::optional<double>{};
  auto scopes = std::array<RawRegion, 4>{};
  auto other = RawRegion{};
  auto *scope = &other;
  auto header = std::string{};
  auto defaultPath = std::string{};
  auto seqGroup = std::uint32_t{0};
  struct PendingRegion { RawRegion raw; std::string defaultPath; std::uint32_t seqGroup; };
  auto pending = std::vector<PendingRegion>{};
  const auto finish = [&] {
    if (header != "region") return;
    chargeWorking(expansion, sizeof(SfzRegion), 8);
    for (const auto &level : scopes) for (const auto &[key, value] : level)
      chargeWorking(expansion, sizeof(std::pair<std::string, std::string>) + key.size() + value.size(), 8);
    auto raw = RawRegion{};
    for (const auto &level : scopes) for (const auto &[key, value] : level) assign(raw, key, value);
    pending.push_back({std::move(raw), defaultPath, seqGroup});
  };
  for (auto marker = nextMarker(text, 0); marker;) {
    const auto next = nextMarker(text, marker->end);
    const auto key = lowercase(marker->name);
    if (marker->header) {
      finish();
      header = key;
      auto level = 4u;
      if (key == "global") { level = 0; scopes[1].clear(); scopes[2].clear(); ++seqGroup; }
      else if (key == "master") { level = 1; scopes[2].clear(); ++seqGroup; }
      else if (key == "group") { level = 2; ++seqGroup; }
      else if (key == "region") level = 3;
      scope = level < 4 ? &scopes[level] : &other;
      scope->clear();
    } else {
      const auto value = unquoted(trimmed(std::string_view(text).substr(marker->end,
          (next ? next->start : text.size()) - marker->end)));
      if (key == "sw_default") {
        const auto note = midi(value);
        if (std::isfinite(note) && std::trunc(note) == note && note >= 0 && note <= 127) initialSwitch = note;
      }
      const auto cc = header == "control" ? ccIndex(key, "set_cc") : std::nullopt;
      if (header == "control" && key == "default_path") {
        defaultPath = value;
        std::ranges::replace(defaultPath, '\\', '/');
      } else if (cc) {
        const auto bound = number(value);
        if (!std::isfinite(bound) || std::trunc(bound) != bound || bound < 0 || bound > 127)
          throw SfzError("prepare", "Invalid SFZ opcode " + key + '.');
        initialCC[*cc] = static_cast<std::uint8_t>(bound);
      } else if (key == "key") {
        assign(*scope, "pitch_keycenter", value);
        assign(*scope, "hikey", value);
        assign(*scope, "lokey", value);
      } else if (supported(key) || key == "trigger" || conditionalOpcode(key, value)) assign(*scope, key, value);
      else result.diagnostics.ignoredOpcodes.push_back(key);
    }
    marker = next;
  }
  finish();
  auto invalid = std::uint32_t{0}, unsupported = std::uint32_t{0};
  auto inventory = std::unordered_set<std::string_view>{};
  if (availableSamples) for (const auto &path : *availableSamples) inventory.insert(path);
  for (const auto &item : pending) {
    try {
      auto excluded = std::vector<std::string>{};
      auto unsupportedCondition = false;
      for (const auto &[key, value] : item.raw) {
        if (key == "sw_default" || key == "sw_lokey" || key == "sw_hikey" || key == "sw_label") continue;
        const auto cc = ccIndex(key, key.starts_with("lo") ? "locc" : "hicc");
        auto filtered = false;
        if (key == "sw_last") filtered = !initialSwitch || midi(value) != *initialSwitch;
        else if (cc) {
          const auto bound = number(value);
          if (!std::isfinite(bound) || std::trunc(bound) != bound || bound < 0 || bound > 127)
            throw SfzError("prepare", "Invalid SFZ opcode " + key + '.');
          filtered = key.starts_with("lo") ? initialCC[*cc] < bound : initialCC[*cc] > bound;
        } else filtered = conditionalOpcode(key, value);
        if (filtered) {
          excluded.push_back(key);
          unsupportedCondition |= key == "sw_last" ? !initialSwitch.has_value() : !cc.has_value();
        }
      }
      if (!excluded.empty()) {
        result.diagnostics.excludedOpcodes.insert(result.diagnostics.excludedOpcodes.end(), excluded.begin(), excluded.end());
        if (unsupportedCondition) ++unsupported;
        continue;
      }
      const auto source = rawSample(item.raw);
      if (source.empty()) continue;
      const auto sample = normalizeSfzPath(item.defaultPath + unquoted(source), directoryOf(selected));
      auto region = normalizeRegion(item.raw, sample, item.seqGroup);
      if (availableSamples && !inventory.contains(sample)) result.diagnostics.missingSamples.push_back(sample);
      else result.regions.push_back(std::move(region));
    } catch (const SfzError &error) {
      if (error.code != "prepare") throw;
      ++invalid;
      const auto source = rawSample(item.raw);
      result.diagnostics.invalidRegions.push_back((source.empty() ? "(no sample)" : source) + ": " + error.what());
    }
  }
  sortUnique(result.diagnostics.ignoredOpcodes);
  sortUnique(result.diagnostics.excludedOpcodes);
  sortUnique(result.diagnostics.missingSamples);
  sortUnique(result.diagnostics.invalidRegions);
  if (invalid) result.warnings.push_back({"invalid-regions", invalid});
  if (!result.diagnostics.missingSamples.empty()) result.warnings.push_back({"missing-samples",
      static_cast<std::uint32_t>(result.diagnostics.missingSamples.size())});
  if (unsupported) result.warnings.push_back({"unsupported-regions", unsupported});
  return result;
}

} // namespace pipetune::assets
