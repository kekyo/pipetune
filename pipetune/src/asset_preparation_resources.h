/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ASSET_PREPARATION_RESOURCES_H
#define PIPETUNE_ASSET_PREPARATION_RESOURCES_H
#include <pipetune/asset_memory.h>
#include "prepared_dsp_asset.h"
#include <cardio.h>
#include <memory>
#include <unordered_map>

namespace pipetune {

struct AssetCachePin;

/** Owns a reservation against all live and preparing assets in this process. */
class AssetMemoryReservation final {
  std::uint64_t bytes_ = 0;
public:
  /** Creates an empty reservation. */
  AssetMemoryReservation() = default;
  /** Reserves bytes before allocation; throws when the combined limit would be exceeded. */
  AssetMemoryReservation(std::uint64_t bytes, std::uint64_t maximumBytes);
  /** Releases the reservation after the associated data have been destroyed. */
  ~AssetMemoryReservation();
  /** Transfers a reservation without changing its process accounting. */
  AssetMemoryReservation(AssetMemoryReservation &&source) noexcept;
  /** Releases the previous reservation and transfers the new one. */
  AssetMemoryReservation &operator=(AssetMemoryReservation &&source) noexcept;
  AssetMemoryReservation(const AssetMemoryReservation &) = delete;
  AssetMemoryReservation &operator=(const AssetMemoryReservation &) = delete;
  /** Reduces a working reservation to the bytes retained by its immutable result. */
  void shrink(std::uint64_t bytes);
};

/** Immutable preparation shared only while pipeline construction needs its host payload. */
struct SharedPreparedAsset {
  AssetMemoryReservation memory; /**< Released after the payload. */
  PreparedDspAsset asset; /**< Encoded native input and dimensions. */
  std::vector<std::string> diagnostics; /**< Preparation warnings, including cache availability. */
  std::shared_ptr<const AssetCachePin> cachePin; /**< Optional persisted entry protection. */
};

/** Serializes host preparation across dispatchers and retains only weak completed results. */
struct AssetPreparationPool {
  cardio::primitives::mutex mutex; /**< Cancellable wait; never held by an audio callback. */
  std::unordered_map<std::string, std::weak_ptr<const SharedPreparedAsset>> entries; /**< Access requires mutex. */
};

/** Returns the process-wide preparation pool; its promises remain local to each waiter. */
AssetPreparationPool &assetPreparationPool();

/** Conservative IR working reservation including bounded source, decode, and analysis buffers. */
inline constexpr std::uint64_t kIrWorkingMemoryBytes = 768ull * 1024 * 1024;

} // namespace pipetune
#endif
