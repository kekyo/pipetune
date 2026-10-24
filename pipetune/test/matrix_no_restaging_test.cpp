/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pipetune/dsp_pipeline.h>
#include <effetune/abi.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static decltype(&et_instance_set_param_bytes) originalSetParameterBytes = nullptr;
static bool rejectParameterWrites = false;

static et_status setParameterBytes(et_engine engine, et_instance instance,
    const std::uint8_t *packed, std::uint32_t bytes, std::uint32_t hash,
    std::uint32_t offset) {
  return rejectParameterWrites ? ET_ERR_STATE :
      originalSetParameterBytes(engine, instance, packed, bytes, hash, offset);
}

// Interpose only the loader's symbol lookup; all processing uses the real backend.
extern "C" void *__real_dlsym(void *handle, const char *name);
extern "C" void *__wrap_dlsym(void *handle, const char *name) {
  auto *address = __real_dlsym(handle, name);
  if (address != nullptr && std::strcmp(name, "et_instance_set_param_bytes") == 0) {
    static_assert(sizeof(address) == sizeof(originalSetParameterBytes));
    std::memcpy(&originalSetParameterBytes, &address, sizeof(address));
    const auto wrapper = &setParameterBytes;
    std::memcpy(&address, &wrapper, sizeof(address));
  }
  return address;
}

static void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

int main() {
  const auto directory = std::filesystem::temp_directory_path() /
      ("pipetune-matrix-reset-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  auto result = 0;
  try {
    const auto path = directory / "matrix.effetune_preset";
    std::ofstream(path) << R"({"pipeline":[{"name":"Matrix","channel":"All","parameters":{"mx":"00021112"}}]})";
    auto loaded = pipetune::loadDspPipeline(path, {48000, 4, 64});
    require(loaded.pipeline != nullptr, loaded.error);
    require(originalSetParameterBytes != nullptr, "the test must use the parameter-write wrapper");
    rejectParameterWrites = true;
    for (auto iteration = 0; iteration < 3; ++iteration) {
      require(loaded.pipeline->reset() == pipetune::ProcessStatus::ok,
          "reset must succeed when the backend accepts no further parameter writes");
      auto audio = std::array{0.25F, 0.125F, 0.75F, 0.5F};
      require(loaded.pipeline->process(audio, 4, 1, iteration / 48000.0) == pipetune::ProcessStatus::ok,
          "Matrix must process without parameter restaging");
      require(audio == std::array{0.25F, 0.125F, 0.375F, 0.0F},
          "reset must preserve the configured routing without parameter restaging");
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    result = 1;
  }
  rejectParameterWrites = false;
  std::filesystem::remove_all(directory);
  return result;
}
