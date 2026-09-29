/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "tray-backend.h"

#include <gio/gio.h>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static constexpr char kWatcherName[] = "org.kde.StatusNotifierWatcher";
static constexpr char kWatcherPath[] = "/StatusNotifierWatcher";
static constexpr char kWatcherXml[] = R"XML(
<node><interface name="org.kde.StatusNotifierWatcher">
  <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  <signal name="StatusNotifierHostRegistered"/>
  <signal name="StatusNotifierHostUnregistered"/>
</interface></node>
)XML";

struct Watcher {
  GDBusConnection *connection = nullptr;
  guint objectId = 0;
  bool hostRegistered = true;
  bool holdRegistration = false;
  unsigned int hostQueries = 0;
  GDBusMethodInvocation *pendingRegistration = nullptr;
  std::vector<std::string> registeredItems;
};

static void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void waitUntil(const std::function<bool()> &condition,
                      const char *message) {
  auto expired = false;
  auto *timeout = g_timeout_source_new(15000);
  g_source_set_callback(
      timeout,
      [](gpointer data) -> gboolean {
        *static_cast<bool *>(data) = true;
        return G_SOURCE_REMOVE;
      },
      &expired, nullptr);
  g_source_attach(timeout, nullptr);
  while (!condition() && !expired) {
    g_main_context_iteration(nullptr, TRUE);
  }
  g_source_destroy(timeout);
  g_source_unref(timeout);
  require(condition(), message);
}

static void onRegisterItem(GDBusConnection *, const gchar *sender,
                           const gchar *, const gchar *, const gchar *,
                           GVariant *, GDBusMethodInvocation *invocation,
                           gpointer userData) {
  auto *watcher = static_cast<Watcher *>(userData);
  watcher->registeredItems.emplace_back(sender);
  if (watcher->holdRegistration) {
    watcher->pendingRegistration =
        G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
  } else {
    g_dbus_method_invocation_return_value(invocation, nullptr);
  }
}

static GVariant *onGetHost(GDBusConnection *, const gchar *, const gchar *,
                           const gchar *, const gchar *property,
                           GError **, gpointer userData) {
  auto *watcher = static_cast<Watcher *>(userData);
  require(std::string(property) == "IsStatusNotifierHostRegistered",
          "Unexpected watcher property");
  ++watcher->hostQueries;
  return g_variant_new_boolean(watcher->hostRegistered);
}

static void waitForBusRoundTrip(GApplication *application) {
  auto complete = false;
  g_dbus_connection_call(
      g_application_get_dbus_connection(application),
      "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetId", nullptr, G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
      [](GObject *source, GAsyncResult *result, gpointer userData) {
        auto *error = static_cast<GError *>(nullptr);
        auto *reply = g_dbus_connection_call_finish(
            G_DBUS_CONNECTION(source), result, &error);
        if (reply != nullptr) {
          g_variant_unref(reply);
        }
        if (error != nullptr) {
          g_error_free(error);
        }
        *static_cast<bool *>(userData) = true;
      },
      &complete);
  waitUntil([&complete]() { return complete; },
            "D-Bus round trip did not complete");
}

static std::unique_ptr<Watcher> createWatcher(const char *address,
                                               bool hostRegistered) {
  auto watcher = std::make_unique<Watcher>();
  watcher->hostRegistered = hostRegistered;
  auto *error = static_cast<GError *>(nullptr);
  watcher->connection = g_dbus_connection_new_for_address_sync(
      address,
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  if (error != nullptr) {
    const auto message = std::string(error->message);
    g_error_free(error);
    throw std::runtime_error(message);
  }
  auto *info = g_dbus_node_info_new_for_xml(kWatcherXml, nullptr);
  const auto vtable = GDBusInterfaceVTable{
      onRegisterItem, onGetHost, nullptr, {nullptr}};
  watcher->objectId = g_dbus_connection_register_object(
      watcher->connection, kWatcherPath, info->interfaces[0], &vtable,
      watcher.get(), nullptr, &error);
  g_dbus_node_info_unref(info);
  if (error != nullptr) {
    const auto message = std::string(error->message);
    g_error_free(error);
    throw std::runtime_error(message);
  }
  require(watcher->objectId != 0, "Cannot export test watcher");
  return watcher;
}

static void destroyWatcher(std::unique_ptr<Watcher> &watcher) {
  if (watcher == nullptr) {
    return;
  }
  if (watcher->pendingRegistration != nullptr) {
    g_dbus_method_invocation_return_value(watcher->pendingRegistration,
                                           nullptr);
    g_clear_object(&watcher->pendingRegistration);
  }
  if (watcher->objectId != 0) {
    g_dbus_connection_unregister_object(watcher->connection,
                                        watcher->objectId);
  }
  g_clear_object(&watcher->connection);
  watcher.reset();
}

static void changeName(Watcher &watcher, bool claim) {
  auto *error = static_cast<GError *>(nullptr);
  auto *reply = g_dbus_connection_call_sync(
      watcher.connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", claim ? "RequestName" : "ReleaseName",
      claim ? g_variant_new("(su)", kWatcherName, 0U)
            : g_variant_new("(s)", kWatcherName),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  if (error != nullptr) {
    const auto message = std::string(error->message);
    g_error_free(error);
    throw std::runtime_error(message);
  }
  auto result = guint{0};
  g_variant_get(reply, "(u)", &result);
  g_variant_unref(reply);
  require(result == 1U, "Cannot change test watcher ownership");
}

static void setHost(Watcher &watcher, bool registered,
                    bool propertiesChanged) {
  watcher.hostRegistered = registered;
  auto *error = static_cast<GError *>(nullptr);
  if (propertiesChanged) {
    auto changed = GVariantBuilder{};
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed, "{sv}",
                          "IsStatusNotifierHostRegistered",
                          g_variant_new_boolean(registered));
    g_dbus_connection_emit_signal(
        watcher.connection, nullptr, kWatcherPath,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", kWatcherName, &changed, nullptr),
        &error);
  } else {
    g_dbus_connection_emit_signal(
        watcher.connection, nullptr, kWatcherPath, kWatcherName,
        registered ? "StatusNotifierHostRegistered"
                   : "StatusNotifierHostUnregistered",
        nullptr, &error);
  }
  if (error != nullptr) {
    const auto message = std::string(error->message);
    g_error_free(error);
    throw std::runtime_error(message);
  }
}

int main() {
  auto *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  auto *application = g_application_new(
      "net.kekyo.pipetune_gtk.TrayTest", G_APPLICATION_DEFAULT_FLAGS);
  auto *error = static_cast<GError *>(nullptr);
  if (!g_application_register(application, nullptr, &error)) {
    std::cerr << error->message << '\n';
    g_error_free(error);
    g_object_unref(application);
    g_test_dbus_down(bus);
    g_object_unref(bus);
    return 1;
  }

  using State = pipetune_gtk::TrayBackendAvailabilityState;
  auto availability = State::pending;
  auto *backend = static_cast<pipetune_gtk::TrayBackendState *>(nullptr);
  auto first = std::unique_ptr<Watcher>{};
  auto second = std::unique_ptr<Watcher>{};
  auto failure = std::string{};
  try {
    backend = pipetune_gtk::createTrayBackend({
        .application = application,
        .identifier = "pipetune",
        .title = "PipeTune",
        .iconState = pipetune_gtk::TrayIconState::active,
        .colorMode = pipetune_gtk::TrayIconColorMode::grayscale,
        .tooltip = "PipeTune",
        .callbacks = {.activate = {},
                      .quit = {},
                      .availabilityChanged = [&availability](State state) {
                        availability = state;
                      }},
    });
    waitUntil([&availability]() { return availability == State::unavailable; },
              "Missing watcher did not become unavailable");

    first = createWatcher(g_test_dbus_get_bus_address(bus), false);
    changeName(*first, true);
    waitUntil([&first]() { return first->hostQueries > 0; },
              "Late watcher host was not queried");
    require(first->registeredItems.empty(),
            "Unready host must not receive the tray item");
    setHost(*first, true, false);
    waitUntil([&availability]() { return availability == State::available; },
              "Late watcher host did not recover the tray");
    require(first->registeredItems.size() == 1,
            "Late watcher must register one tray item");

    changeName(*first, false);
    waitUntil([&availability]() { return availability == State::unavailable; },
              "Watcher loss did not invalidate the tray");
    second = createWatcher(g_test_dbus_get_bus_address(bus), true);
    changeName(*second, true);
    waitUntil([&availability]() { return availability == State::available; },
              "Restarted watcher did not recover the tray");
    require(second->registeredItems.size() == 1,
            "Restarted watcher must register one tray item");

    for (const auto propertiesChanged : {false, true}) {
      setHost(*second, false, propertiesChanged);
      waitUntil(
          [&availability]() { return availability == State::unavailable; },
          "Host loss did not invalidate the tray");
      setHost(*second, true, propertiesChanged);
      waitUntil([&availability]() { return availability == State::available; },
                "Returning host did not recover the tray");
    }
    require(second->registeredItems.size() == 3,
            "Returning host must re-register one tray item each time");

    changeName(*second, false);
    waitUntil([&availability]() { return availability == State::unavailable; },
              "Watcher loss before a held registration was ignored");
    second->holdRegistration = true;
    changeName(*second, true);
    waitUntil([&second]() { return second->pendingRegistration != nullptr; },
              "Held registration was not requested");
    require(availability != State::available,
            "Pending registration must not mark the tray available");
    changeName(*second, false);
    first->hostRegistered = true;
    changeName(*first, true);
    waitUntil([&availability]() { return availability == State::available; },
              "Replacement watcher did not recover the tray");
    g_dbus_method_invocation_return_value(second->pendingRegistration,
                                           nullptr);
    g_clear_object(&second->pendingRegistration);
    waitForBusRoundTrip(application);
    require(availability == State::available,
            "Old registration reply overwrote the current tray");

    changeName(*first, false);
    waitUntil([&availability]() { return availability == State::unavailable; },
              "Watcher loss before destruction was ignored");
    changeName(*second, true);
    waitUntil([&second]() { return second->pendingRegistration != nullptr; },
              "Registration was not held before destruction");
    pipetune_gtk::destroyTrayBackend(backend);
    backend = nullptr;
    g_dbus_method_invocation_return_value(second->pendingRegistration,
                                           nullptr);
    g_clear_object(&second->pendingRegistration);
    waitForBusRoundTrip(application);
    changeName(*second, false);
  } catch (const std::exception &exception) {
    failure = exception.what();
  }
  pipetune_gtk::destroyTrayBackend(backend);
  destroyWatcher(first);
  destroyWatcher(second);
  g_object_unref(application);
  g_test_dbus_down(bus);
  g_object_unref(bus);
  if (!failure.empty()) {
    std::cerr << failure << '\n';
    return 1;
  }
  std::cout << "tray watcher lifecycle PASS\n";
  return 0;
}
