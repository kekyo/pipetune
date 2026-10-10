/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PREPARATION_GENERATION_H
#define PIPETUNE_PREPARATION_GENERATION_H
#include <cardio.h>
#include <atomic>
#include <cstdint>
#include <memory>

namespace pipetune {

/** Captured identity and cancellation of one candidate configuration. */
struct PreparationTicket {
  std::uint64_t generation; /**< Monotonic configuration intent. */
  cardio::cancellation_source source; /**< Canceled by superseding work or shutdown. */
};

/** Serializes configuration intent; callers protect access with the mutation mutex. */
struct PreparationGeneration {
  /** Cancels older preparation and captures a fresh, independently owned request. */
  PreparationTicket begin() {
    invalidate();
    const auto next = std::make_shared<cardio::cancellation_source>();
    source.store(next);
    if (stopped.load()) next->cancel();
    active.store(generation, std::memory_order_release);
    return {generation, *next};
  }

  /** Invalidates any outstanding candidate without changing the active pipeline. */
  void invalidate() {
    source.load()->cancel();
    ++generation;
    active.store(0, std::memory_order_release);
  }

  /** Permanently rejects preparation and cancels its current owner; callable from any non-audio thread. */
  void shutdown() {
    stopped.store(true);
    source.load()->cancel();
    active.store(0, std::memory_order_release);
  }

  /** Returns whether the candidate still describes the current configuration intent. */
  bool current(const PreparationTicket &ticket) const {
    return !stopped.load() && ticket.generation == generation && !ticket.source.get_cancellation().is_cancellation_requested();
  }

  /** Marks a completed candidate without clearing newer preparation; callable without the mutex. */
  void finish(const PreparationTicket &ticket) {
    auto expected = ticket.generation;
    active.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
  }

  /** Reports whether preparation is still pending. */
  bool pending() const { return !stopped.load() && active.load(std::memory_order_acquire) != 0; }

private:
  std::uint64_t generation = 0;
  std::atomic<std::shared_ptr<cardio::cancellation_source>> source{std::make_shared<cardio::cancellation_source>()};
  std::atomic<bool> stopped{false};
  std::atomic<std::uint64_t> active{0};
};

/** A preparation ticket whose pending marker is released on every completion path. */
struct PreparationScope {
  PreparationGeneration &generations; /**< Owner, retained until this scope ends. */
  PreparationTicket ticket; /**< Captured configuration intent. */
  cardio::cancellation_registration registration; /**< Parent cancellation subscription. */
  /** Starts a candidate while the caller holds the owner's mutation mutex. */
  PreparationScope(PreparationGeneration &owner, cardio::cancellation parent)
      : generations(owner), ticket(owner.begin()),
        registration(parent.on_cancellation_requested([source = ticket.source]() mutable { source.cancel(); })) {
    if (parent.is_cancellation_requested()) ticket.source.cancel();
  }
  /** Releases only this pending marker; no mutation mutex is acquired. */
  ~PreparationScope() { generations.finish(ticket); }
};

} // namespace pipetune
#endif
