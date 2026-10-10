/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "asset_preparation_resources.h"
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace pipetune {

static std::atomic<std::uint64_t> reservedAssetBytes{0};

AssetMemoryReservation::AssetMemoryReservation(std::uint64_t bytes, std::uint64_t maximumBytes) {
  const auto limit = std::min(maximumBytes, kDefaultAssetMemoryBytes);
  auto used = reservedAssetBytes.load();
  do {
    if (bytes > limit || used > limit - bytes)
      throw std::length_error("combined asset memory budget is occupied by live or preparing assets");
  } while (!reservedAssetBytes.compare_exchange_weak(used, used + bytes));
  bytes_ = bytes;
}

AssetMemoryReservation::~AssetMemoryReservation() { reservedAssetBytes.fetch_sub(bytes_); }

AssetMemoryReservation::AssetMemoryReservation(AssetMemoryReservation &&source) noexcept
    : bytes_(std::exchange(source.bytes_, 0)) {}

AssetMemoryReservation &AssetMemoryReservation::operator=(AssetMemoryReservation &&source) noexcept {
  if (this != &source) {
    reservedAssetBytes.fetch_sub(bytes_);
    bytes_ = std::exchange(source.bytes_, 0);
  }
  return *this;
}

void AssetMemoryReservation::shrink(std::uint64_t bytes) {
  if (bytes > bytes_) throw std::length_error("prepared asset exceeds its reserved working memory budget");
  reservedAssetBytes.fetch_sub(bytes_ - bytes);
  bytes_ = bytes;
}

AssetPreparationPool &assetPreparationPool() {
  static auto pool = AssetPreparationPool{};
  return pool;
}

} // namespace pipetune
