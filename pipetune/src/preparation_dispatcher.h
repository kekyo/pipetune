/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PREPARATION_DISPATCHER_H
#define PIPETUNE_PREPARATION_DISPATCHER_H
#include <cardio.h>
#include <memory>
#include <optional>

namespace pipetune {

/**
 * Yields preparation to the current GLib dispatcher without sleeping or polling.
 * @param cancellation Superseded request or shutdown notification.
 * @return Completion after other ready sources have had an opportunity to run.
 * @throws cardio::canceled_exception When the request is no longer needed.
 */
inline cardio::promise<void> preparationCheckpoint(cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  auto completion = cardio::promise_source<void>{};
  const auto destroy = [](GSource *source) { g_source_destroy(source); g_source_unref(source); };
  const auto source = std::unique_ptr<GSource, decltype(destroy)>(g_idle_source_new(), destroy);
  g_source_set_priority(source.get(), G_PRIORITY_DEFAULT);
  g_source_set_callback(source.get(), [](void *data) {
    static_cast<cardio::promise_source<void> *>(data)->try_resolve();
    return G_SOURCE_REMOVE;
  }, &completion, nullptr);
  g_source_attach(source.get(), g_main_context_get_thread_default());
  co_await completion.get_promise();
  cancellation.throw_if_cancellation_requested();
}

template <typename T, typename Factory>
static cardio::promise<void> collectPreparation(Factory &factory,
    cardio::dispatcher_group_glib &group, std::optional<T> &result, std::exception_ptr &failure) {
  try { result.emplace(std::move(co_await factory())); }
  catch (...) { failure = std::current_exception(); }
  group.shutdown();
}

/**
 * Drives an asynchronous preparation for synchronous startup and command-line callers.
 * @tparam T Owned preparation result.
 * @tparam Factory Callable returning a cardio promise on the installed dispatcher.
 * @param factory Operation to start after installing its private GLib context.
 * @return The completed result; exceptions are rethrown after context cleanup.
 * @remarks Live control handlers must await directly instead of nesting this pump.
 */
template <typename T, typename Factory>
T runPreparation(Factory factory) {
  auto nested = false;
  try { static_cast<void>(cardio::get_current_dispatcher()); nested = true; }
  catch (const std::runtime_error &) {}
  if (nested) throw std::logic_error("await asynchronous preparation on the current dispatcher");
  const auto release = [](GMainContext *context) {
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
  };
  auto *created = g_main_context_new();
  g_main_context_push_thread_default(created);
  const auto context = std::unique_ptr<GMainContext, decltype(release)>(created, release);
  auto result = std::optional<T>{};
  auto failure = std::exception_ptr{};
  {
    cardio::dispatcher_group_glib group(context.get());
    cardio::dispatcher_host_glib host(group);
    const auto operation = collectPreparation<T>(factory, group, result, failure);
    host.park();
  }
  if (failure) std::rethrow_exception(failure);
  return std::move(*result);
}

} // namespace pipetune
#endif
