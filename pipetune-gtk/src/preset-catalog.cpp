#include "preset-catalog.h"

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pipetune_gtk {

constexpr auto kMaximumManifestBytes = std::size_t{1024 * 1024};
constexpr auto kMaximumUserPresetBytes = std::size_t{16 * 1024 * 1024};

struct JsonDocumentDeleter {
  void operator()(yyjson_doc *document) const noexcept {
    yyjson_doc_free(document);
  }
};

using JsonDocument = std::unique_ptr<yyjson_doc, JsonDocumentDeleter>;

static std::string systemError(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

static std::string_view trim(std::string_view value) {
  constexpr auto whitespace = std::string_view{" \t\r\n"};
  const auto start = value.find_first_not_of(whitespace);
  if (start == std::string_view::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(whitespace);
  return value.substr(start, end - start + 1);
}

static bool readLimitedFile(const std::filesystem::path &path,
                            std::size_t maximumBytes,
                            std::string &contents,
                            std::string &error) {
  auto stream = std::ifstream(path, std::ios::binary);
  if (!stream) {
    error = "cannot read " + path.string();
    return false;
  }
  contents.clear();
  auto buffer = std::array<char, 4096>{};
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = stream.gcount();
    if (count > 0) {
      contents.append(buffer.data(), static_cast<std::size_t>(count));
      if (contents.size() > maximumBytes) {
        error = path.string() + " exceeds the supported size";
        return false;
      }
    }
  }
  if (stream.bad()) {
    error = "cannot read " + path.string();
    return false;
  }
  error.clear();
  return true;
}

static bool isSafeRelativePresetPath(
    const std::filesystem::path &relativePath) {
  if (relativePath.empty() || relativePath.is_absolute()) {
    return false;
  }
  for (const auto &component : relativePath) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

static void loadStandardPresets(
    const std::filesystem::path &directory,
    EffeTunePresetCatalogResult &result) {
  const auto manifestPath = directory / "presets.txt";
  auto contents = std::string{};
  auto error = std::string{};
  if (!readLimitedFile(manifestPath, kMaximumManifestBytes, contents,
                       error)) {
    result.diagnostics.push_back(
        "EffeTune standard presets are unavailable: " + error);
    return;
  }

  auto inPresetSection = false;
  auto offset = std::size_t{0};
  auto displayNames = std::set<std::string>{};
  while (offset <= contents.size()) {
    const auto end = contents.find('\n', offset);
    const auto length =
        end == std::string::npos ? contents.size() - offset : end - offset;
    const auto line = trim(std::string_view(contents).substr(offset, length));
    if (!line.empty() && !line.starts_with('#')) {
      if (line.front() == '[' && line.back() == ']') {
        inPresetSection = line == "[presets]";
      } else if (inPresetSection) {
        const auto colon = line.find(':');
        const auto firstPipe =
            colon == std::string_view::npos
                ? std::string_view::npos
                : line.find('|', colon + 1);
        const auto secondPipe =
            firstPipe == std::string_view::npos
                ? std::string_view::npos
                : line.find('|', firstPipe + 1);
        if (colon == std::string_view::npos ||
            firstPipe == std::string_view::npos ||
            secondPipe == std::string_view::npos) {
          result.diagnostics.push_back(
              "EffeTune standard preset manifest contains an invalid entry");
        } else {
          const auto relativeText = trim(line.substr(0, colon));
          const auto displayName =
              std::string(trim(line.substr(colon + 1,
                                           firstPipe - colon - 1)));
          const auto category =
              std::string(trim(line.substr(firstPipe + 1,
                                           secondPipe - firstPipe - 1)));
          auto relativePath =
              std::filesystem::path(std::string(relativeText));
          if (!isSafeRelativePresetPath(relativePath) ||
              displayName.empty() || category.empty() ||
              !displayNames.insert(displayName).second) {
            result.diagnostics.push_back(
                "EffeTune standard preset manifest contains an invalid entry");
          } else {
            relativePath += ".effetune_preset";
            const auto presetPath =
                (directory / relativePath).lexically_normal();
            auto filesystemError = std::error_code{};
            const auto regular = std::filesystem::is_regular_file(
                presetPath, filesystemError);
            if (!regular || filesystemError) {
              result.diagnostics.push_back(
                  "EffeTune standard preset is unavailable: " +
                  presetPath.string());
            } else {
              result.choices.push_back(
                  {.source = PresetSource::standard,
                   .name = displayName,
                   .category = category,
                   .path = presetPath,
                   .serializedPreset = {}});
            }
          }
        }
      }
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1;
  }
}

static bool hasPresetPipeline(yyjson_val *value) {
  if (!yyjson_is_obj(value)) {
    return false;
  }
  auto *pipeline = yyjson_obj_get(value, "pipeline");
  auto *plugins = yyjson_obj_get(value, "plugins");
  return yyjson_is_arr(pipeline) || yyjson_is_arr(plugins);
}

static std::string serializePreset(yyjson_val *value) {
  auto length = std::size_t{0};
  auto *encoded = yyjson_val_write(
      value, YYJSON_WRITE_PRETTY | YYJSON_WRITE_NEWLINE_AT_END, &length);
  if (encoded == nullptr) {
    return {};
  }
  auto serialized = std::string(encoded, length);
  std::free(encoded);
  return serialized;
}

EffeTuneSavedPresetLoadResult loadEffeTuneSavedPresets(
    const std::filesystem::path &path) {
  auto result = EffeTuneSavedPresetLoadResult{
      .choices = {},
      .parsed = false,
      .diagnostics = {},
  };
  if (path.empty()) {
    return result;
  }
  auto filesystemError = std::error_code{};
  const auto exists = std::filesystem::exists(path, filesystemError);
  if (!exists && !filesystemError) {
    return result;
  }
  if (filesystemError) {
    result.diagnostics.push_back(
        "Cannot inspect EffeTune saved presets: " +
        filesystemError.message());
    return result;
  }

  auto contents = std::string{};
  auto error = std::string{};
  if (!readLimitedFile(path, kMaximumUserPresetBytes, contents, error)) {
    result.diagnostics.push_back(
        "Cannot read EffeTune saved presets: " + error);
    return result;
  }
  auto document =
      JsonDocument(yyjson_read(contents.data(), contents.size(),
                               YYJSON_READ_NOFLAG));
  auto *root =
      document == nullptr ? nullptr : yyjson_doc_get_root(document.get());
  if (!yyjson_is_obj(root)) {
    result.diagnostics.push_back(
        "EffeTune saved presets contain invalid JSON");
    return result;
  }
  result.parsed = true;

  auto names = std::set<std::string>{};
  auto iterator = yyjson_obj_iter_with(root);
  while (auto *key = yyjson_obj_iter_next(&iterator)) {
    auto *value = yyjson_obj_iter_get_val(key);
    const auto name =
        std::string(yyjson_get_str(key), yyjson_get_len(key));
    const auto serialized =
        hasPresetPipeline(value) ? serializePreset(value) : std::string{};
    if (name.empty() ||
        name.find('\0') != std::string::npos ||
        !names.insert(name).second || serialized.empty()) {
      result.diagnostics.push_back(
          "EffeTune saved preset \"" + name +
          "\" has an invalid pipeline and was omitted");
      continue;
    }
    result.choices.push_back(
        {.source = PresetSource::saved,
         .name = name,
         .category = {},
         .path = {},
         .serializedPreset = serialized});
  }
  std::ranges::sort(
      result.choices, {},
      [](const PresetChoice &choice) { return choice.name; });
  return result;
}

static std::uint64_t presetNameHash(std::string_view name) {
  auto hash = std::uint64_t{14695981039346656037ULL};
  for (const auto character : name) {
    hash ^= static_cast<unsigned char>(character);
    hash *= std::uint64_t{1099511628211ULL};
  }
  return hash;
}

static std::string presetFileStem(std::string_view name) {
  auto stem = std::string{};
  stem.reserve(std::min<std::size_t>(name.size(), 48));
  auto pendingSeparator = false;
  for (const auto character : name) {
    const auto asciiLetter =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z');
    const auto asciiDigit = character >= '0' && character <= '9';
    if (asciiLetter || asciiDigit || character == '_' || character == '-') {
      if (pendingSeparator && !stem.empty() && stem.back() != '-' &&
          stem.size() < 48) {
        stem.push_back('-');
      }
      pendingSeparator = false;
      if (stem.size() < 48) {
        stem.push_back(character);
      }
    } else {
      pendingSeparator = true;
    }
  }
  while (!stem.empty() && stem.back() == '-') {
    stem.pop_back();
  }
  return stem.empty() ? std::string("preset") : stem;
}

static std::filesystem::path savedPresetPath(
    const PresetChoice &choice,
    const std::filesystem::path &directory) {
  auto hashText = std::ostringstream{};
  hashText << std::hex << std::setfill('0') << std::setw(16)
           << presetNameHash(choice.name);
  return directory /
         (presetFileStem(choice.name) + "-" + hashText.str() +
          ".effetune_preset");
}

static bool validateSerializedPreset(std::string_view serialized) {
  if (serialized.empty() ||
      serialized.size() > kMaximumUserPresetBytes) {
    return false;
  }
  auto document =
      JsonDocument(yyjson_read(serialized.data(), serialized.size(),
                               YYJSON_READ_NOFLAG));
  return document != nullptr &&
         hasPresetPipeline(yyjson_doc_get_root(document.get()));
}

static std::string writeAll(int descriptor, std::string_view contents) {
  auto offset = std::size_t{0};
  while (offset < contents.size()) {
    const auto count =
        write(descriptor, contents.data() + offset,
              contents.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return systemError("cannot write saved preset snapshot");
  }
  return {};
}

static PresetChoicePathResult materializeSavedPreset(
    const PresetChoice &choice,
    const std::filesystem::path &directory) {
  if (choice.name.empty() ||
      !validateSerializedPreset(choice.serializedPreset)) {
    return {.path = {},
            .error = "saved EffeTune preset is invalid"};
  }
  if (directory.empty()) {
    return {.path = {},
            .error = "saved preset snapshot directory is unavailable"};
  }

  auto filesystemError = std::error_code{};
  std::filesystem::create_directories(directory, filesystemError);
  if (filesystemError) {
    return {.path = {},
            .error = "cannot create saved preset snapshot directory: " +
                     filesystemError.message()};
  }
  if (chmod(directory.c_str(), 0700) != 0) {
    return {.path = {},
            .error =
                systemError("cannot secure saved preset snapshot directory")};
  }

  auto templatePath = (directory / ".preset.XXXXXX").string();
  auto templateBuffer =
      std::vector<char>(templatePath.begin(), templatePath.end());
  templateBuffer.push_back('\0');
  const auto descriptor = mkstemp(templateBuffer.data());
  if (descriptor < 0) {
    return {.path = {},
            .error = systemError(
                "cannot create temporary saved preset snapshot")};
  }
  const auto temporaryPath = std::filesystem::path(templateBuffer.data());
  auto error = std::string{};
  if (fchmod(descriptor, 0600) != 0) {
    error = systemError("cannot secure saved preset snapshot");
  }
  if (error.empty()) {
    error = writeAll(descriptor, choice.serializedPreset);
  }
  if (error.empty() && fsync(descriptor) != 0) {
    error = systemError("cannot synchronize saved preset snapshot");
  }
  if (close(descriptor) != 0 && error.empty()) {
    error = systemError("cannot close saved preset snapshot");
  }

  const auto destination = savedPresetPath(choice, directory);
  if (error.empty() &&
      rename(temporaryPath.c_str(), destination.c_str()) != 0) {
    error = systemError("cannot replace saved preset snapshot");
  }
  if (!error.empty()) {
    unlink(temporaryPath.c_str());
    return {.path = {}, .error = std::move(error)};
  }
  return {.path = destination, .error = {}};
}

EffeTuneUserPresetPathResult resolveEffeTuneUserPresetPath(
    std::string_view xdgConfigHome,
    const std::filesystem::path &homeDirectory) {
  if (!xdgConfigHome.empty()) {
    return {
        .path = std::filesystem::path(std::string(xdgConfigHome)) /
                "effetune" / "effetune_presets.json",
        .error = {}};
  }
  if (homeDirectory.empty()) {
    return {
        .path = {},
        .error = "HOME is required when XDG_CONFIG_HOME is unset"};
  }
  return {.path = homeDirectory / ".config" / "effetune" /
                  "effetune_presets.json",
          .error = {}};
}

EffeTunePresetCatalogResult loadEffeTunePresetCatalog(
    const std::filesystem::path &standardPresetDirectory,
    const std::filesystem::path &userPresetFile) {
  auto result = EffeTunePresetCatalogResult{};
  loadStandardPresets(standardPresetDirectory, result);
  auto saved = loadEffeTuneSavedPresets(userPresetFile);
  result.choices.insert(
      result.choices.end(),
      std::make_move_iterator(saved.choices.begin()),
      std::make_move_iterator(saved.choices.end()));
  result.diagnostics.insert(
      result.diagnostics.end(),
      std::make_move_iterator(saved.diagnostics.begin()),
      std::make_move_iterator(saved.diagnostics.end()));
  return result;
}

std::vector<PresetChoice> applyEffeTuneSavedPresetRefresh(
    const std::vector<PresetChoice> &currentChoices,
    const EffeTuneSavedPresetLoadResult &refresh) {
  auto updated = std::vector<PresetChoice>{};
  updated.reserve(currentChoices.size() + refresh.choices.size());
  std::ranges::copy_if(
      currentChoices, std::back_inserter(updated),
      [](const PresetChoice &choice) {
        return choice.source == PresetSource::standard;
      });
  const auto &savedChoices =
      refresh.parsed ? refresh.choices : currentChoices;
  std::ranges::copy_if(
      savedChoices, std::back_inserter(updated),
      [](const PresetChoice &choice) {
        return choice.source == PresetSource::saved;
      });
  return updated;
}

PresetChoicePathResult resolvePresetChoicePath(
    const PresetChoice &choice,
    const std::filesystem::path &savedPresetDirectory) {
  if (choice.source == PresetSource::standard) {
    if (choice.path.empty()) {
      return {.path = {},
              .error = "standard EffeTune preset path is unavailable"};
    }
    return {.path = choice.path, .error = {}};
  }
  return materializeSavedPreset(choice, savedPresetDirectory);
}

ActiveSavedPresetRefreshResult refreshActiveSavedPresetSnapshot(
    const std::vector<PresetChoice> &choices,
    const std::filesystem::path &activePresetPath,
    const std::filesystem::path &savedPresetDirectory) {
  if (activePresetPath.empty() || savedPresetDirectory.empty()) {
    return {.matched = false, .changed = false, .error = {}};
  }

  auto filesystemError = std::error_code{};
  const auto active = std::filesystem::absolute(
                          activePresetPath, filesystemError)
                          .lexically_normal();
  if (filesystemError) {
    return {
        .matched = false,
        .changed = false,
        .error = "cannot resolve active saved preset path: " +
                 filesystemError.message(),
    };
  }

  for (const auto &choice : choices) {
    if (choice.source != PresetSource::saved) {
      continue;
    }
    filesystemError.clear();
    const auto expected = std::filesystem::absolute(
                              savedPresetPath(choice, savedPresetDirectory),
                              filesystemError)
                              .lexically_normal();
    if (filesystemError) {
      return {
          .matched = false,
          .changed = false,
          .error = "cannot resolve saved preset snapshot path: " +
                   filesystemError.message(),
      };
    }
    if (expected != active) {
      continue;
    }
    if (!validateSerializedPreset(choice.serializedPreset)) {
      return {
          .matched = true,
          .changed = false,
          .error = "saved EffeTune preset is invalid",
      };
    }

    auto current = std::string{};
    auto readError = std::string{};
    if (readLimitedFile(expected, kMaximumUserPresetBytes, current,
                        readError) &&
        current == choice.serializedPreset) {
      return {.matched = true, .changed = false, .error = {}};
    }
    const auto materialized =
        materializeSavedPreset(choice, savedPresetDirectory);
    if (!materialized.error.empty()) {
      return {
          .matched = true,
          .changed = false,
          .error = materialized.error,
      };
    }
    return {.matched = true, .changed = true, .error = {}};
  }
  return {.matched = false, .changed = false, .error = {}};
}

} // namespace pipetune_gtk
