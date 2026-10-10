/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "sfz/parser.h"
#include "sfz/bank.h"
#include "sfz/selection.h"
#include "sfz/preparation.h"
#include "sfz/audio_header.h"
#include <bit>
#include <array>
#include <iomanip>
#include <iostream>
#include <stdexcept>

static std::uint32_t readWord() {
  auto result = std::uint32_t{0};
  for (auto byte = 0u; byte < 4; ++byte) {
    const auto value = std::cin.get();
    if (value == EOF) throw std::runtime_error("truncated SFZ test input");
    result |= static_cast<std::uint32_t>(value) << (byte * 8);
  }
  return result;
}

static std::string readText() {
  const auto bytes = readWord();
  if (bytes > 16 * 1024 * 1024) throw std::runtime_error("oversized SFZ test input");
  auto value = std::string(bytes, '\0');
  if (!std::cin.read(value.data(), bytes)) throw std::runtime_error("truncated SFZ test text");
  return value;
}

static std::uint64_t readWide() {
  const auto low = readWord();
  return low | (static_cast<std::uint64_t>(readWord()) << 32);
}

static void printText(std::string_view text) {
  constexpr auto digits = std::string_view{"0123456789abcdef"};
  std::cout << '"';
  for (const auto ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    if (ch == '"' || ch == '\\') std::cout << '\\' << ch;
    else if (byte < 32) std::cout << "\\u00" << digits[byte >> 4] << digits[byte & 15];
    else std::cout << ch;
  }
  std::cout << '"';
}

static void printStrings(const std::vector<std::string> &values) {
  std::cout << '[';
  for (auto index = std::size_t{0}; index < values.size(); ++index) {
    if (index) std::cout << ',';
    printText(values[index]);
  }
  std::cout << ']';
}

static void printParsed(const pipetune::assets::ParsedSfz &parsed) {
  std::cout << std::setprecision(17) << "{\"regions\":[";
  for (auto index = std::size_t{0}; index < parsed.regions.size(); ++index) {
    if (index) std::cout << ',';
    const auto &r = parsed.regions[index];
    std::cout << "{\"sample\":";
    printText(r.sample);
    const auto values = std::array{
        std::pair{"seqGroup", static_cast<double>(r.seqGroup)}, std::pair{"lokey", r.lokey}, std::pair{"hikey", r.hikey},
        std::pair{"lovel", r.lovel}, std::pair{"hivel", r.hivel}, std::pair{"lorand", r.lorand}, std::pair{"hirand", r.hirand},
        std::pair{"seq_length", r.seq_length}, std::pair{"seq_position", r.seq_position},
        std::pair{"pitch_keycenter", r.pitch_keycenter}, std::pair{"pitch_keytrack", r.pitch_keytrack},
        std::pair{"transpose", r.transpose}, std::pair{"tune", r.tune}, std::pair{"volume", r.volume}, std::pair{"pan", r.pan},
        std::pair{"amp_veltrack", r.amp_veltrack}, std::pair{"offset", r.offset}, std::pair{"loop_mode", r.loop_mode},
        std::pair{"loop_start", r.loop_start}, std::pair{"ampeg_attack", r.ampeg_attack}, std::pair{"ampeg_hold", r.ampeg_hold},
        std::pair{"ampeg_decay", r.ampeg_decay}, std::pair{"ampeg_sustain", r.ampeg_sustain}, std::pair{"ampeg_release", r.ampeg_release}};
    for (const auto &[name, value] : values) std::cout << ",\"" << name << "\":" << value;
    if (r.end) std::cout << ",\"end\":" << *r.end;
    if (r.loop_end) std::cout << ",\"loop_end\":" << *r.loop_end;
    std::cout << '}';
  }
  std::cout << "],\"dependencies\":";
  printStrings(parsed.dependencies);
  std::cout << ",\"warnings\":[";
  for (auto index = std::size_t{0}; index < parsed.warnings.size(); ++index) {
    if (index) std::cout << ',';
    std::cout << "{\"code\":";
    printText(parsed.warnings[index].code);
    std::cout << ",\"count\":" << parsed.warnings[index].count << '}';
  }
  std::cout << "],\"diagnostics\":{\"ignoredOpcodes\":";
  printStrings(parsed.diagnostics.ignoredOpcodes);
  std::cout << ",\"excludedOpcodes\":";
  printStrings(parsed.diagnostics.excludedOpcodes);
  std::cout << ",\"missingSamples\":";
  printStrings(parsed.diagnostics.missingSamples);
  std::cout << ",\"invalidRegions\":";
  printStrings(parsed.diagnostics.invalidRegions);
  std::cout << "}}\n";
}

static int prepareInput(std::uint64_t maximumBytes, bool parseOnly, bool details) {
  using namespace pipetune::assets;
  const auto selected = readText();
  const auto count = readWord();
  if (count > 10000) return 2;
  auto documents = std::vector<SfzDocument>{};
  for (auto index = 0u; index < count; ++index) {
    auto path = readText();
    documents.push_back({std::move(path), readText()});
  }
  const auto sampleCount = readWord();
  if (sampleCount > 10000) return 2;
  auto samples = std::vector<SfzSample>{};
  auto paths = std::vector<std::string>{};
  for (auto index = 0u; index < sampleCount; ++index) {
    auto path = readText();
    const auto rate = readWord(), channels = readWord(), frames = readWord();
    if (channels == 0 || channels > 16 || frames > 16 * 1024 * 1024 / channels) return 2;
    auto audio = Audio{rate, std::vector<std::vector<float>>(channels, std::vector<float>(frames))};
    for (auto &channel : audio.channels)
      for (auto &sample : channel) sample = std::bit_cast<float>(readWord());
    paths.push_back(path);
    samples.push_back({std::move(path), std::move(audio)});
  }
  const auto parsed = parseSfz(selected, documents, std::span<const std::string>(paths), maximumBytes);
  if (parseOnly) { printParsed(parsed); return 0; }
  const auto bank = prepareSfzBank(parsed, samples, maximumBytes);
  std::cout << "{\"regions\":" << bank.regionCount << ",\"footprintBytes\":" << bank.footprintBytes
            << ",\"warmupFrames\":" << bank.warmupFrames << ",\"samples\":" << bank.samples;
  if (details) {
    std::cout << ",\"warnings\":[";
    for (auto index = std::size_t{0}; index < bank.warnings.size(); ++index) {
      if (index) std::cout << ',';
      std::cout << "{\"code\":";
      printText(bank.warnings[index].code);
      std::cout << ",\"count\":" << bank.warnings[index].count << '}';
    }
    std::cout << "],\"invalidRegions\":";
    printStrings(bank.invalidRegions);
  }
  std::cout << "}\n";
  std::cout.write(reinterpret_cast<const char *>(bank.payload.data()), bank.payload.size());
  return 0;
}

static int selectInput(std::uint64_t maximumBytes, std::uint64_t definitionBytes) {
  using namespace pipetune::assets;
  const auto documents = std::vector<SfzDocument>{{"main.sfz", readText()}};
  const auto count = readWord();
  if (count > 10000) return 2;
  auto metadata = std::vector<SfzSampleMetadata>{};
  for (auto index = 0u; index < count; ++index) {
    auto path = readText();
    const auto size = readWide(), frames = readWide();
    metadata.push_back({std::move(path), size, frames, readWord()});
  }
  auto parsed = parseSfz("main.sfz", documents, std::nullopt, kMaximumSfzBytes);
  auto selection = selectSfzRegionsForBudget(parsed.regions, metadata, maximumBytes, definitionBytes);
  parsed.regions = std::move(selection.regions);
  std::cout << "{\"parsed\":";
  printParsed(parsed);
  std::cout << ",\"reduced\":" << (selection.reduced ? "true" : "false");
  if (selection.reduced) std::cout << ",\"velocity\":" << selection.velocity << ",\"keyCount\":" << selection.keyCount;
  std::cout << "}\n";
  return 0;
}

int main(int argc, char **argv) {
  using namespace pipetune::assets;
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--audio-header") {
      const auto bytes = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(std::cin), {});
      const auto header = inspectSfzAudioHeader(bytes, bytes.size());
      if (header.channels) std::cout << "{\"channels\":" << header.channels << ",\"sampleRate\":"
          << std::setprecision(17) << header.sampleRate << ",\"frames\":" << header.frames << "}\n";
      else std::cout << "{}\n";
      return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--select") return selectInput(std::stoull(argv[2]), std::stoull(argv[3]));
    if (argc == 3 && (std::string_view(argv[1]) == "--prepare" || std::string_view(argv[1]) == "--parse" ||
        std::string_view(argv[1]) == "--prepare-details"))
      return prepareInput(std::stoull(argv[2]), std::string_view(argv[1]) == "--parse", std::string_view(argv[1]) == "--prepare-details");
    if (argc != 1) return 2;
    const auto documents = std::vector<SfzDocument>{{"instrument.sfz", "<region> sample=impulse.wav key=60"}};
    const auto samples = std::vector<SfzSample>{{"impulse.wav", {48000, {{1, 0, 0, 0}}}}};
    const auto parsed = parseSfz("instrument.sfz", documents, std::nullopt, 256 * 1024 * 1024);
    auto bounded = false;
    try { static_cast<void>(parseSfz("instrument.sfz", documents, std::nullopt, 256 * 1024 * 1024, 1)); }
    catch (const SfzError &error) { bounded = error.code == "too-large"; }
    if (!bounded) return 1;
    const auto bank = packSfzBank(parsed.regions, samples, 256 * 1024 * 1024);
    if (parsed.regions.size() != 1 || bank.payload.size() != 200 || bank.footprintBytes != 728) return 1;
    std::cout << "isolated SFZ parser and bank: one region, four PCM frames\n";
    return 0;
  } catch (const SfzError &error) { std::cerr << error.code << ": " << error.what() << '\n'; return 1; }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
