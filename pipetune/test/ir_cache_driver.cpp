/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "ir_asset_loader.h"
#include <iostream>
#include <iterator>

int main(int argc, char **argv) {
  if (argc != 5) return 2;
  const auto json = std::string(std::istreambuf_iterator<char>(std::cin), {});
  const auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
  if (!document) return 2;
  const auto result = pipetune::loadIrAsset(argv[1], yyjson_doc_get_root(document.get()),
      std::stof(argv[3]), static_cast<std::uint32_t>(std::stoul(argv[4])), {argv[2]});
  if (!result.error.empty()) { std::cerr << result.error << '\n'; return 1; }
  for (const auto &diagnostic : result.prepared->diagnostics) std::cerr << diagnostic << '\n';
  const auto &asset = result.prepared->asset;
  std::cout << result.cacheHit << ' ' << asset.info.head_block << ' ' << asset.info.frames << '\n';
  std::cout.write(reinterpret_cast<const char *>(asset.payload.data()), asset.payload.size());
  return 0;
}
