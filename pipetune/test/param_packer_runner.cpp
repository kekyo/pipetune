#include "dsp_catalog.h"

#include <yyjson.h>

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

struct JsonDocumentDeleter {
  void operator()(yyjson_doc *document) const {
    yyjson_doc_free(document);
  }
};

static void printString(std::string_view value) {
  std::cout << '"';
  for (const auto character : value) {
    if (character == '"' || character == '\\') {
      std::cout << '\\';
    }
    std::cout << character;
  }
  std::cout << '"';
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: pipetune_param_packer_runner REQUEST_JSON\n";
    return 2;
  }
  auto document =
      std::unique_ptr<yyjson_doc, JsonDocumentDeleter>(yyjson_read_file(argv[1], 0, nullptr, nullptr));
  auto *root = yyjson_doc_get_root(document.get());
  auto *cases = yyjson_obj_get(root, "cases");
  if (!yyjson_is_arr(cases)) {
    std::cerr << "request must contain a cases array\n";
    return 2;
  }

  std::cout << std::setprecision(std::numeric_limits<double>::max_digits10) << "{\"cases\":[";
  const auto count = yyjson_arr_size(cases);
  for (auto index = std::size_t{0}; index < count; ++index) {
    auto *testCase = yyjson_arr_get(cases, index);
    auto *typeValue = yyjson_obj_get(testCase, "type");
    auto *parameters = yyjson_obj_get(testCase, "parameters");
    if (!yyjson_is_str(typeValue) || !yyjson_is_obj(parameters)) {
      std::cerr << "invalid parameter packing case\n";
      return 2;
    }
    const auto type = std::string_view(yyjson_get_str(typeValue), yyjson_get_len(typeValue));
    const auto *definition = pipetune::findDspByTypeName(type);
    if (definition == nullptr) {
      std::cerr << "unknown DSP type\n";
      return 2;
    }
    const auto packed = pipetune::packDspParameters(*definition, parameters);
    if (index != 0) {
      std::cout << ',';
    }
    std::cout << "{\"type\":";
    printString(type);
    std::cout << ",\"error\":";
    printString(packed.error);
    std::cout << ",\"hash\":" << definition->hash << ",\"floatCount\":"
              << definition->floatCount << ",\"floats\":[";
    for (auto valueIndex = std::size_t{0}; valueIndex < packed.floats.size(); ++valueIndex) {
      if (valueIndex != 0) {
        std::cout << ',';
      }
      std::cout << static_cast<double>(packed.floats[valueIndex]);
    }
    std::cout << "],\"bytes\":[";
    for (auto byteIndex = std::size_t{0}; byteIndex < packed.bytes.size(); ++byteIndex) {
      if (byteIndex != 0) {
        std::cout << ',';
      }
      std::cout << static_cast<unsigned int>(packed.bytes[byteIndex]);
    }
    std::cout << "]}";
  }
  std::cout << "]}\n";
  return 0;
}
