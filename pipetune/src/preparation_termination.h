/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PREPARATION_TERMINATION_H
#define PIPETUNE_PREPARATION_TERMINATION_H
#include <cardio.h>
#include <cerrno>
#include <csignal>
#include <optional>
#include <stdexcept>
#include <sys/signalfd.h>
#include <unistd.h>

namespace pipetune {

struct PreparationSignalDescriptor {
  int value;
  ~PreparationSignalDescriptor() { if (value >= 0) close(value); }
};

static cardio::promise<void> observePreparationTermination(int descriptor,
    cardio::cancellation_source source, cardio::cancellation finished, bool &interrupted) {
  try {
    for (;;) {
      co_await cardio::from_fd(descriptor, cardio::fd_event::read, finished);
      auto signal = signalfd_siginfo{};
      const auto count = read(descriptor, &signal, sizeof(signal));
      if (count == sizeof(signal)) {
        interrupted = true;
        source.cancel();
        co_return;
      }
      if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      throw std::runtime_error("cannot read preparation termination signal");
    }
  } catch (const cardio::canceled_exception &) {
    if (!finished.is_cancellation_requested()) throw;
  } catch (...) { source.cancel(); throw; }
}

/**
 * Cancels and drains preparation when termination arrives while another main loop is suspended.
 * @tparam T Owned preparation result.
 * @tparam Factory Callable accepting a cancellation token and returning promise<T>.
 * @param factory Preparation to start on the current dispatcher.
 * @param source Cancellation source shared with the configuration generation.
 * @param interrupted Set when SIGINT or SIGTERM is consumed.
 * @return The owned result, or the preparation/cancellation exception after draining the observer.
 * @remarks The application must block SIGINT/SIGTERM before creating any threads.
 * The observer adds no thread, polling, or fixed delay. Its descriptor is closed only after the wait ends.
 */
template <typename T, typename Factory>
cardio::promise<T> withPreparationTermination(Factory factory,
    cardio::cancellation_source source, bool &interrupted) {
  auto signals = sigset_t{};
  sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
  const auto descriptor = PreparationSignalDescriptor{signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC)};
  if (descriptor.value < 0) throw std::runtime_error("cannot observe preparation termination signals");
  auto finished = cardio::cancellation_source{};
  auto observer = observePreparationTermination(descriptor.value, source, finished.get_cancellation(), interrupted);
  auto result = std::optional<T>{};
  auto failure = std::exception_ptr{};
  try { result.emplace(std::move(co_await factory(source.get_cancellation()))); }
  catch (...) { failure = std::current_exception(); }
  finished.cancel();
  co_await observer;
  if (failure) std::rethrow_exception(failure);
  co_return std::move(*result);
}

} // namespace pipetune
#endif
