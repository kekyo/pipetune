#include "tray-backend.h"

#include <gdk/gdkx.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pipetune_gtk {

constexpr char kApplicationId[] = "net.kekyo.pipetune-gtk";
constexpr char kStatusNotifierWatcherService[] =
    "org.kde.StatusNotifierWatcher";
constexpr char kStatusNotifierWatcherPath[] = "/StatusNotifierWatcher";
constexpr char kStatusNotifierWatcherInterface[] =
    "org.kde.StatusNotifierWatcher";
constexpr char kStatusNotifierItemPath[] = "/StatusNotifierItem";
constexpr char kStatusNotifierMenuPath[] = "/StatusNotifierMenu";
constexpr char kStatusNotifierItemInterface[] =
    "org.kde.StatusNotifierItem";
constexpr char kDbusMenuInterface[] = "com.canonical.dbusmenu";
constexpr auto kDbusMenuRevision = int{1};
constexpr auto kOpenMenuItemId = int{1};
constexpr auto kQuitMenuItemId = int{2};
constexpr auto kTrayIconSizes =
    std::array<int, 8>{16, 22, 24, 32, 48, 64, 128, 256};

constexpr char kStatusNotifierItemXml[] = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <method name="ContextMenu">
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
    </method>
    <method name="Activate">
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
    </method>
    <method name="Scroll">
      <arg type="i" name="delta" direction="in"/>
      <arg type="s" name="orientation" direction="in"/>
    </method>
    <signal name="NewAttentionIcon"/>
    <signal name="NewIcon"/>
    <signal name="NewStatus">
      <arg type="s" name="status"/>
    </signal>
    <signal name="NewTitle"/>
    <property type="s" name="Category" access="read"/>
    <property type="s" name="Id" access="read"/>
    <property type="s" name="Title" access="read"/>
    <property type="s" name="Status" access="read"/>
    <property type="u" name="WindowId" access="read"/>
    <property type="o" name="Menu" access="read"/>
    <property type="b" name="ItemIsMenu" access="read"/>
    <property type="s" name="IconName" access="read"/>
    <property type="a(iiay)" name="IconPixmap" access="read"/>
    <property type="s" name="OverlayIconName" access="read"/>
    <property type="a(iiay)" name="OverlayIconPixmap" access="read"/>
    <property type="s" name="IconThemePath" access="read"/>
    <property type="s" name="IconAccessibleDesc" access="read"/>
    <property type="s" name="AttentionIconName" access="read"/>
    <property type="a(iiay)" name="AttentionIconPixmap" access="read"/>
    <property type="s" name="AttentionAccessibleDesc" access="read"/>
  </interface>
</node>
)XML";

constexpr char kDbusMenuXml[] = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <method name="GetLayout">
      <arg type="i" name="parentId" direction="in"/>
      <arg type="i" name="recursionDepth" direction="in"/>
      <arg type="as" name="propertyNames" direction="in"/>
      <arg type="u" name="revision" direction="out"/>
      <arg type="(ia{sv}av)" name="layout" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg type="ai" name="ids" direction="in"/>
      <arg type="as" name="propertyNames" direction="in"/>
      <arg type="a(ia{sv})" name="properties" direction="out"/>
    </method>
    <method name="Event">
      <arg type="i" name="id" direction="in"/>
      <arg type="s" name="eventId" direction="in"/>
      <arg type="v" name="data" direction="in"/>
      <arg type="u" name="timestamp" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg type="a(isvu)" name="events" direction="in"/>
      <arg type="ai" name="idErrors" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg type="i" name="id" direction="in"/>
      <arg type="b" name="needUpdate" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg type="ai" name="ids" direction="in"/>
      <arg type="ai" name="updatesNeeded" direction="out"/>
      <arg type="ai" name="idErrors" direction="out"/>
    </method>
    <signal name="LayoutUpdated">
      <arg type="u" name="revision"/>
      <arg type="i" name="parent"/>
    </signal>
    <signal name="ItemActivationRequested">
      <arg type="i" name="id"/>
      <arg type="u" name="timestamp"/>
    </signal>
    <property type="s" name="Status" access="read"/>
    <property type="s" name="TextDirection" access="read"/>
    <property type="u" name="Version" access="read"/>
    <property type="s" name="IconThemePath" access="read"/>
  </interface>
</node>
)XML";

struct TrayBackendImplementation {
  TrayBackendOptions options;
  TrayBackendKind kind;
  TrayBackendAvailabilityState availability;
  TrayIconState iconState;
  std::string tooltip;
  bool destroyed;
  GDBusConnection *connection;
  GCancellable *cancellable;
  guint itemRegistrationId;
  guint menuRegistrationId;
  GtkStatusIcon *statusIcon;
  GtkWidget *statusMenu;
  gulong embeddedSignalId;

  explicit TrayBackendImplementation(TrayBackendOptions backendOptions)
      : options(std::move(backendOptions)), kind(TrayBackendKind::none),
        availability(TrayBackendAvailabilityState::pending),
        iconState(options.iconState), tooltip(options.tooltip),
        destroyed(false), connection(nullptr),
        cancellable(g_cancellable_new()), itemRegistrationId(0),
        menuRegistrationId(0), statusIcon(nullptr), statusMenu(nullptr),
        embeddedSignalId(0) {}

  ~TrayBackendImplementation() {
    g_object_unref(cancellable);
  }
};

struct TrayBackendState {
  std::shared_ptr<TrayBackendImplementation> implementation;
};

static GDBusNodeInfo *statusNotifierNodeInfo() {
  static auto *nodeInfo =
      g_dbus_node_info_new_for_xml(kStatusNotifierItemXml, nullptr);
  return nodeInfo;
}

static GDBusNodeInfo *dbusMenuNodeInfo() {
  static auto *nodeInfo =
      g_dbus_node_info_new_for_xml(kDbusMenuXml, nullptr);
  return nodeInfo;
}

static GDBusInterfaceInfo *statusNotifierInterfaceInfo() {
  return statusNotifierNodeInfo()->interfaces[0];
}

static GDBusInterfaceInfo *dbusMenuInterfaceInfo() {
  return dbusMenuNodeInfo()->interfaces[0];
}

const char *applicationId() {
  return kApplicationId;
}

TrayBackendKind
selectTrayBackendKind(const TrayBackendAvailability &availability) {
  if (availability.hasStatusNotifierItem) {
    return TrayBackendKind::statusNotifierItem;
  }
  if (availability.hasXEmbed) {
    return TrayBackendKind::xembed;
  }
  return TrayBackendKind::none;
}

std::string_view trayIconName(TrayIconState state) {
  if (state == TrayIconState::attention) {
    return "pipetune-attention";
  }
  if (state == TrayIconState::disconnected) {
    return "pipetune-disconnected";
  }
  return "pipetune";
}

std::vector<std::uint8_t>
convertTrayIconPixelsToArgb(const std::uint8_t *pixels, int width,
                            int height, int rowstride,
                            int channelCount) {
  if (pixels == nullptr || width <= 0 || height <= 0 || rowstride <= 0 ||
      (channelCount != 3 && channelCount != 4) ||
      rowstride < width * channelCount) {
    return {};
  }
  auto converted = std::vector<std::uint8_t>{};
  converted.reserve(static_cast<std::size_t>(width) *
                    static_cast<std::size_t>(height) * 4U);
  for (auto y = int{0}; y < height; ++y) {
    const auto *row =
        pixels + static_cast<std::size_t>(y) *
                     static_cast<std::size_t>(rowstride);
    for (auto x = int{0}; x < width; ++x) {
      const auto *pixel =
          row + static_cast<std::size_t>(x) *
                    static_cast<std::size_t>(channelCount);
      converted.push_back(channelCount == 4 ? pixel[3] : 0xffU);
      converted.push_back(pixel[0]);
      converted.push_back(pixel[1]);
      converted.push_back(pixel[2]);
    }
  }
  return converted;
}

GVariant *
buildTrayIconPixmapVariant(const std::vector<TrayIconPixmap> &pixmaps) {
  auto builder = GVariantBuilder{};
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
  for (const auto &pixmap : pixmaps) {
    if (pixmap.width <= 0 || pixmap.height <= 0) {
      continue;
    }
    const auto expected =
        static_cast<std::size_t>(pixmap.width) *
        static_cast<std::size_t>(pixmap.height) * 4U;
    if (pixmap.argbPixels.size() != expected) {
      continue;
    }
    auto *bytes = g_variant_new_fixed_array(
        G_VARIANT_TYPE_BYTE, pixmap.argbPixels.data(),
        pixmap.argbPixels.size(), sizeof(std::uint8_t));
    g_variant_builder_add(&builder, "(ii@ay)", pixmap.width,
                          pixmap.height, bytes);
  }
  return g_variant_builder_end(&builder);
}

static std::array<std::uint8_t, 3>
iconColor(TrayIconState state) {
  if (state == TrayIconState::attention) {
    return {222U, 146U, 28U};
  }
  if (state == TrayIconState::disconnected) {
    return {105U, 113U, 126U};
  }
  return {31U, 157U, 121U};
}

static GdkPixbuf *createTrayIconPixbuf(TrayIconState state, int size) {
  auto *pixbuf =
      gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, size, size);
  if (pixbuf == nullptr) {
    return nullptr;
  }
  gdk_pixbuf_fill(pixbuf, 0x00000000U);
  auto *pixels = gdk_pixbuf_get_pixels(pixbuf);
  const auto rowstride = gdk_pixbuf_get_rowstride(pixbuf);
  const auto channels = gdk_pixbuf_get_n_channels(pixbuf);
  const auto color = iconColor(state);
  const auto center = (static_cast<double>(size) - 1.0) / 2.0;
  const auto radius = static_cast<double>(size) * 0.46;
  const auto lineHalfWidth = std::max(1, size / 14);
  const auto nodeRadius = std::max(1, size / 9);
  for (auto y = int{0}; y < size; ++y) {
    auto *row = pixels + y * rowstride;
    for (auto x = int{0}; x < size; ++x) {
      auto *pixel = row + x * channels;
      const auto dx = static_cast<double>(x) - center;
      const auto dy = static_cast<double>(y) - center;
      if (dx * dx + dy * dy > radius * radius) {
        continue;
      }
      pixel[0] = color[0];
      pixel[1] = color[1];
      pixel[2] = color[2];
      pixel[3] = 0xffU;

      const auto horizontal =
          std::abs(y - size / 2) <= lineHalfWidth &&
          x >= size / 5 && x <= (size * 4) / 5;
      const auto leftNode =
          (x - size / 3) * (x - size / 3) +
                  (y - size / 2) * (y - size / 2) <=
              nodeRadius * nodeRadius;
      const auto rightNode =
          (x - (size * 2) / 3) * (x - (size * 2) / 3) +
                  (y - size / 2) * (y - size / 2) <=
              nodeRadius * nodeRadius;
      const auto disconnectedSlash =
          state == TrayIconState::disconnected &&
          std::abs((x + y) - (size - 1)) <= lineHalfWidth;
      if (horizontal || leftNode || rightNode || disconnectedSlash) {
        pixel[0] = 0xffU;
        pixel[1] = 0xffU;
        pixel[2] = 0xffU;
      }
    }
  }
  return pixbuf;
}

static std::vector<TrayIconPixmap>
createTrayIconPixmaps(TrayIconState state) {
  auto pixmaps = std::vector<TrayIconPixmap>{};
  pixmaps.reserve(kTrayIconSizes.size());
  for (const auto size : kTrayIconSizes) {
    auto *pixbuf = createTrayIconPixbuf(state, size);
    if (pixbuf == nullptr) {
      continue;
    }
    auto pixels = convertTrayIconPixelsToArgb(
        gdk_pixbuf_get_pixels(pixbuf), size, size,
        gdk_pixbuf_get_rowstride(pixbuf),
        gdk_pixbuf_get_n_channels(pixbuf));
    g_object_unref(pixbuf);
    if (!pixels.empty()) {
      pixmaps.push_back({.width = size,
                         .height = size,
                         .argbPixels = std::move(pixels)});
    }
  }
  return pixmaps;
}

static const char *statusNotifierStatus(TrayIconState state) {
  return state == TrayIconState::active ? "Active" : "NeedsAttention";
}

static GVariant *buildMenuItemProperties(const char *label) {
  auto properties = GVariantBuilder{};
  g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&properties, "{sv}", "label",
                        g_variant_new_string(label));
  g_variant_builder_add(&properties, "{sv}", "enabled",
                        g_variant_new_boolean(TRUE));
  g_variant_builder_add(&properties, "{sv}", "visible",
                        g_variant_new_boolean(TRUE));
  return g_variant_builder_end(&properties);
}

static GVariant *buildMenuItem(int id, const char *label) {
  auto children = GVariantBuilder{};
  g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
  return g_variant_new("(i@a{sv}@av)", id,
                       buildMenuItemProperties(label),
                       g_variant_builder_end(&children));
}

static GVariant *buildMenuLayout() {
  auto rootProperties = GVariantBuilder{};
  g_variant_builder_init(&rootProperties, G_VARIANT_TYPE("a{sv}"));
  auto children = GVariantBuilder{};
  g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
  g_variant_builder_add_value(
      &children,
      g_variant_new_variant(
          buildMenuItem(kOpenMenuItemId, "Open PipeTune")));
  g_variant_builder_add_value(
      &children,
      g_variant_new_variant(buildMenuItem(kQuitMenuItemId, "Quit")));
  auto *root =
      g_variant_new("(i@a{sv}@av)", 0,
                    g_variant_builder_end(&rootProperties),
                    g_variant_builder_end(&children));
  return g_variant_new("(u@(ia{sv}av))", kDbusMenuRevision, root);
}

static GVariant *
buildMenuGroupProperties(const std::vector<int> &identifiers) {
  auto builder = GVariantBuilder{};
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
  for (const auto identifier : identifiers) {
    if (identifier == kOpenMenuItemId) {
      g_variant_builder_add(
          &builder, "(i@a{sv})", identifier,
          buildMenuItemProperties("Open PipeTune"));
    } else if (identifier == kQuitMenuItemId) {
      g_variant_builder_add(&builder, "(i@a{sv})", identifier,
                            buildMenuItemProperties("Quit"));
    }
  }
  return g_variant_builder_end(&builder);
}

static void setAvailability(
    TrayBackendImplementation *implementation,
    TrayBackendAvailabilityState availability) {
  if (implementation == nullptr || implementation->destroyed ||
      implementation->availability == availability) {
    return;
  }
  implementation->availability = availability;
  if (implementation->options.callbacks.availabilityChanged) {
    implementation->options.callbacks.availabilityChanged(availability);
  }
}

static void activateBackend(TrayBackendImplementation *implementation) {
  if (implementation != nullptr && !implementation->destroyed &&
      implementation->options.callbacks.activate) {
    implementation->options.callbacks.activate();
  }
}

static void quitBackend(TrayBackendImplementation *implementation) {
  if (implementation != nullptr && !implementation->destroyed &&
      implementation->options.callbacks.quit) {
    implementation->options.callbacks.quit();
  }
}

static void handleMenuEvent(TrayBackendImplementation *implementation,
                            int itemId, const char *eventId) {
  if (eventId == nullptr || std::strcmp(eventId, "clicked") != 0) {
    return;
  }
  if (itemId == kOpenMenuItemId) {
    activateBackend(implementation);
  } else if (itemId == kQuitMenuItemId) {
    quitBackend(implementation);
  }
}

static void handleStatusNotifierMethod(
    TrayBackendImplementation *implementation, const gchar *methodName,
    GDBusMethodInvocation *invocation) {
  if (std::strcmp(methodName, "Activate") == 0 ||
      std::strcmp(methodName, "SecondaryActivate") == 0) {
    activateBackend(implementation);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (std::strcmp(methodName, "ContextMenu") == 0 ||
      std::strcmp(methodName, "Scroll") == 0) {
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  g_dbus_method_invocation_return_error_literal(
      invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
      "Unsupported StatusNotifierItem method");
}

static void handleDbusMenuMethod(
    TrayBackendImplementation *implementation, const gchar *methodName,
    GVariant *parameters, GDBusMethodInvocation *invocation) {
  if (std::strcmp(methodName, "GetLayout") == 0) {
    g_dbus_method_invocation_return_value(invocation, buildMenuLayout());
    return;
  }
  if (std::strcmp(methodName, "GetGroupProperties") == 0) {
    auto *idsValue = static_cast<GVariant *>(nullptr);
    auto *propertiesValue = static_cast<GVariant *>(nullptr);
    g_variant_get(parameters, "(@ai@as)", &idsValue, &propertiesValue);
    auto identifiers = std::vector<int>{};
    auto iterator = GVariantIter{};
    g_variant_iter_init(&iterator, idsValue);
    auto identifier = gint{0};
    while (g_variant_iter_next(&iterator, "i", &identifier)) {
      identifiers.push_back(identifier);
    }
    g_variant_unref(propertiesValue);
    g_variant_unref(idsValue);
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(@a(ia{sv}))",
                      buildMenuGroupProperties(identifiers)));
    return;
  }
  if (std::strcmp(methodName, "Event") == 0) {
    auto itemId = gint{0};
    auto *eventId = static_cast<const gchar *>(nullptr);
    auto *data = static_cast<GVariant *>(nullptr);
    auto timestamp = guint{0};
    g_variant_get(parameters, "(i&svu)", &itemId, &eventId, &data,
                  &timestamp);
    static_cast<void>(timestamp);
    handleMenuEvent(implementation, itemId, eventId);
    g_variant_unref(data);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (std::strcmp(methodName, "EventGroup") == 0) {
    auto *events = static_cast<GVariant *>(nullptr);
    g_variant_get(parameters, "(@a(isvu))", &events);
    auto iterator = GVariantIter{};
    g_variant_iter_init(&iterator, events);
    auto *event = static_cast<GVariant *>(nullptr);
    while ((event = g_variant_iter_next_value(&iterator)) != nullptr) {
      auto itemId = gint{0};
      auto *eventId = static_cast<const gchar *>(nullptr);
      auto *data = static_cast<GVariant *>(nullptr);
      auto timestamp = guint{0};
      g_variant_get(event, "(i&svu)", &itemId, &eventId, &data,
                    &timestamp);
      static_cast<void>(timestamp);
      handleMenuEvent(implementation, itemId, eventId);
      g_variant_unref(data);
      g_variant_unref(event);
    }
    g_variant_unref(events);
    auto errors = GVariantBuilder{};
    g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(ai)", &errors));
    return;
  }
  if (std::strcmp(methodName, "AboutToShow") == 0) {
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(b)", FALSE));
    return;
  }
  if (std::strcmp(methodName, "AboutToShowGroup") == 0) {
    auto updates = GVariantBuilder{};
    g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
    auto errors = GVariantBuilder{};
    g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(aiai)", &updates, &errors));
    return;
  }
  g_dbus_method_invocation_return_error_literal(
      invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
      "Unsupported DBusMenu method");
}

static void onDbusMethodCall(
    GDBusConnection *, const gchar *, const gchar *,
    const gchar *interfaceName, const gchar *methodName,
    GVariant *parameters, GDBusMethodInvocation *invocation,
    gpointer userData) {
  auto *implementation =
      static_cast<TrayBackendImplementation *>(userData);
  if (std::strcmp(interfaceName, kStatusNotifierItemInterface) == 0) {
    handleStatusNotifierMethod(implementation, methodName, invocation);
    return;
  }
  if (std::strcmp(interfaceName, kDbusMenuInterface) == 0) {
    handleDbusMenuMethod(implementation, methodName, parameters,
                         invocation);
    return;
  }
  g_dbus_method_invocation_return_error_literal(
      invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
      "Unsupported tray interface");
}

static GVariant *statusNotifierProperty(
    TrayBackendImplementation *implementation, const gchar *propertyName,
    GError **error) {
  if (std::strcmp(propertyName, "Category") == 0) {
    return g_variant_new_string("ApplicationStatus");
  }
  if (std::strcmp(propertyName, "Id") == 0) {
    return g_variant_new_string(implementation->options.identifier.c_str());
  }
  if (std::strcmp(propertyName, "Title") == 0) {
    return g_variant_new_string(implementation->options.title.c_str());
  }
  if (std::strcmp(propertyName, "Status") == 0) {
    return g_variant_new_string(
        statusNotifierStatus(implementation->iconState));
  }
  if (std::strcmp(propertyName, "WindowId") == 0) {
    return g_variant_new_uint32(0);
  }
  if (std::strcmp(propertyName, "Menu") == 0) {
    return g_variant_new_object_path(kStatusNotifierMenuPath);
  }
  if (std::strcmp(propertyName, "ItemIsMenu") == 0) {
    return g_variant_new_boolean(FALSE);
  }
  if (std::strcmp(propertyName, "IconName") == 0) {
    const auto name = trayIconName(implementation->iconState);
    return g_variant_new_string(std::string(name).c_str());
  }
  if (std::strcmp(propertyName, "IconPixmap") == 0) {
    return buildTrayIconPixmapVariant(
        createTrayIconPixmaps(implementation->iconState));
  }
  if (std::strcmp(propertyName, "OverlayIconName") == 0) {
    return g_variant_new_string("");
  }
  if (std::strcmp(propertyName, "OverlayIconPixmap") == 0) {
    return buildTrayIconPixmapVariant({});
  }
  if (std::strcmp(propertyName, "IconThemePath") == 0) {
    return g_variant_new_string("");
  }
  if (std::strcmp(propertyName, "IconAccessibleDesc") == 0) {
    return g_variant_new_string(implementation->tooltip.c_str());
  }
  if (std::strcmp(propertyName, "AttentionIconName") == 0) {
    if (implementation->iconState == TrayIconState::active) {
      return g_variant_new_string("");
    }
    const auto name = trayIconName(implementation->iconState);
    return g_variant_new_string(std::string(name).c_str());
  }
  if (std::strcmp(propertyName, "AttentionIconPixmap") == 0) {
    if (implementation->iconState == TrayIconState::active) {
      return buildTrayIconPixmapVariant({});
    }
    return buildTrayIconPixmapVariant(
        createTrayIconPixmaps(implementation->iconState));
  }
  if (std::strcmp(propertyName, "AttentionAccessibleDesc") == 0) {
    return g_variant_new_string(implementation->tooltip.c_str());
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "Unsupported StatusNotifierItem property");
  return nullptr;
}

static GVariant *dbusMenuProperty(const gchar *propertyName,
                                  GError **error) {
  if (std::strcmp(propertyName, "Status") == 0) {
    return g_variant_new_string("normal");
  }
  if (std::strcmp(propertyName, "TextDirection") == 0) {
    return g_variant_new_string("ltr");
  }
  if (std::strcmp(propertyName, "Version") == 0) {
    return g_variant_new_uint32(3);
  }
  if (std::strcmp(propertyName, "IconThemePath") == 0) {
    return g_variant_new_string("");
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "Unsupported DBusMenu property");
  return nullptr;
}

static GVariant *onDbusGetProperty(
    GDBusConnection *, const gchar *, const gchar *,
    const gchar *interfaceName, const gchar *propertyName,
    GError **error, gpointer userData) {
  auto *implementation =
      static_cast<TrayBackendImplementation *>(userData);
  if (std::strcmp(interfaceName, kStatusNotifierItemInterface) == 0) {
    return statusNotifierProperty(implementation, propertyName, error);
  }
  if (std::strcmp(interfaceName, kDbusMenuInterface) == 0) {
    return dbusMenuProperty(propertyName, error);
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "Unsupported tray property");
  return nullptr;
}

constexpr auto kStatusNotifierVtable = GDBusInterfaceVTable{
    onDbusMethodCall,
    onDbusGetProperty,
    nullptr,
    {nullptr},
};

constexpr auto kDbusMenuVtable = GDBusInterfaceVTable{
    onDbusMethodCall,
    onDbusGetProperty,
    nullptr,
    {nullptr},
};

static bool registerStatusNotifierObjects(
    TrayBackendImplementation *implementation) {
  auto *error = static_cast<GError *>(nullptr);
  implementation->itemRegistrationId =
      g_dbus_connection_register_object(
          implementation->connection, kStatusNotifierItemPath,
          statusNotifierInterfaceInfo(), &kStatusNotifierVtable,
          implementation, nullptr, &error);
  if (error != nullptr) {
    std::cerr << "pipetune-gtk: cannot register StatusNotifierItem: "
              << error->message << '\n';
    g_error_free(error);
    return false;
  }
  implementation->menuRegistrationId =
      g_dbus_connection_register_object(
          implementation->connection, kStatusNotifierMenuPath,
          dbusMenuInterfaceInfo(), &kDbusMenuVtable, implementation,
          nullptr, &error);
  if (error != nullptr) {
    std::cerr << "pipetune-gtk: cannot register StatusNotifier menu: "
              << error->message << '\n';
    g_error_free(error);
    return false;
  }
  return true;
}

static void unregisterStatusNotifierObjects(
    TrayBackendImplementation *implementation) {
  if (implementation->connection == nullptr) {
    return;
  }
  if (implementation->menuRegistrationId != 0) {
    g_dbus_connection_unregister_object(
        implementation->connection, implementation->menuRegistrationId);
    implementation->menuRegistrationId = 0;
  }
  if (implementation->itemRegistrationId != 0) {
    g_dbus_connection_unregister_object(
        implementation->connection, implementation->itemRegistrationId);
    implementation->itemRegistrationId = 0;
  }
}

static bool canUseXEmbed() {
  auto *display = gdk_display_get_default();
  return display != nullptr && GDK_IS_X11_DISPLAY(display);
}

static bool xembedTrayHostAvailable() {
  auto *display = gdk_display_get_default();
  if (display == nullptr || !GDK_IS_X11_DISPLAY(display)) {
    return false;
  }
  auto *screen = gdk_display_get_default_screen(display);
  if (screen == nullptr) {
    return false;
  }
  const auto selectionName =
      std::string("_NET_SYSTEM_TRAY_S") +
      std::to_string(gdk_x11_screen_get_screen_number(screen));
  const auto selection = gdk_atom_intern(selectionName.c_str(), TRUE);
  return selection != GDK_NONE &&
         gdk_selection_owner_get_for_display(display, selection) != nullptr;
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void updateStatusIcon(
    TrayBackendImplementation *implementation) {
  if (implementation->statusIcon == nullptr) {
    return;
  }
  auto *pixbuf = createTrayIconPixbuf(implementation->iconState, 24);
  if (pixbuf != nullptr) {
    gtk_status_icon_set_from_pixbuf(implementation->statusIcon, pixbuf);
    g_object_unref(pixbuf);
  }
  gtk_status_icon_set_tooltip_text(implementation->statusIcon,
                                   implementation->tooltip.c_str());
}

static void onStatusIconActivate(GtkStatusIcon *, gpointer userData) {
  activateBackend(static_cast<TrayBackendImplementation *>(userData));
}

static void onStatusIconPopup(GtkStatusIcon *statusIcon, guint button,
                              guint activateTime, gpointer userData) {
  auto *implementation =
      static_cast<TrayBackendImplementation *>(userData);
  gtk_menu_popup(GTK_MENU(implementation->statusMenu), nullptr, nullptr,
                 gtk_status_icon_position_menu, statusIcon, button,
                 activateTime);
}

static void onStatusIconOpen(GtkMenuItem *, gpointer userData) {
  activateBackend(static_cast<TrayBackendImplementation *>(userData));
}

static void onStatusIconQuit(GtkMenuItem *, gpointer userData) {
  quitBackend(static_cast<TrayBackendImplementation *>(userData));
}

static void onStatusIconEmbeddedChanged(GObject *object, GParamSpec *,
                                        gpointer userData) {
  setAvailability(
      static_cast<TrayBackendImplementation *>(userData),
      gtk_status_icon_is_embedded(GTK_STATUS_ICON(object)) != FALSE
          ? TrayBackendAvailabilityState::available
          : TrayBackendAvailabilityState::unavailable);
}

static void createXEmbedBackend(
    TrayBackendImplementation *implementation) {
  implementation->kind = TrayBackendKind::xembed;
  auto *pixbuf = createTrayIconPixbuf(implementation->iconState, 24);
  implementation->statusIcon =
      pixbuf == nullptr ? gtk_status_icon_new()
                        : gtk_status_icon_new_from_pixbuf(pixbuf);
  if (pixbuf != nullptr) {
    g_object_unref(pixbuf);
  }
  implementation->statusMenu = gtk_menu_new();
  gtk_status_icon_set_tooltip_text(implementation->statusIcon,
                                   implementation->tooltip.c_str());
  gtk_status_icon_set_title(implementation->statusIcon,
                            implementation->options.title.c_str());

  auto *openItem = gtk_menu_item_new_with_label("Open PipeTune");
  auto *quitItem = gtk_menu_item_new_with_label("Quit");
  gtk_menu_shell_append(GTK_MENU_SHELL(implementation->statusMenu),
                        openItem);
  gtk_menu_shell_append(GTK_MENU_SHELL(implementation->statusMenu),
                        quitItem);
  gtk_widget_show_all(implementation->statusMenu);
  g_signal_connect(implementation->statusIcon, "activate",
                   G_CALLBACK(onStatusIconActivate), implementation);
  g_signal_connect(implementation->statusIcon, "popup-menu",
                   G_CALLBACK(onStatusIconPopup), implementation);
  g_signal_connect(openItem, "activate", G_CALLBACK(onStatusIconOpen),
                   implementation);
  g_signal_connect(quitItem, "activate", G_CALLBACK(onStatusIconQuit),
                   implementation);
  implementation->embeddedSignalId = g_signal_connect(
      implementation->statusIcon, "notify::embedded",
      G_CALLBACK(onStatusIconEmbeddedChanged), implementation);
  gtk_status_icon_set_visible(implementation->statusIcon, TRUE);
  setAvailability(
      implementation,
      xembedTrayHostAvailable()
          ? TrayBackendAvailabilityState::available
          : TrayBackendAvailabilityState::unavailable);
}

static void destroyXEmbedBackend(
    TrayBackendImplementation *implementation) {
  if (implementation->statusMenu != nullptr) {
    gtk_widget_destroy(implementation->statusMenu);
    implementation->statusMenu = nullptr;
  }
  if (implementation->statusIcon != nullptr) {
    if (implementation->embeddedSignalId != 0) {
      g_signal_handler_disconnect(implementation->statusIcon,
                                  implementation->embeddedSignalId);
      implementation->embeddedSignalId = 0;
    }
    gtk_status_icon_set_visible(implementation->statusIcon, FALSE);
    g_object_unref(implementation->statusIcon);
    implementation->statusIcon = nullptr;
  }
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void createFallbackBackend(
    TrayBackendImplementation *implementation) {
  if (implementation->destroyed) {
    return;
  }
  const auto selected = selectTrayBackendKind({
      .hasStatusNotifierItem = false,
      .hasXEmbed = canUseXEmbed(),
  });
  if (selected == TrayBackendKind::xembed) {
    createXEmbedBackend(implementation);
    return;
  }
  implementation->kind = selected;
  setAvailability(implementation,
                  TrayBackendAvailabilityState::unavailable);
}

static void onStatusNotifierRegistered(GObject *source,
                                       GAsyncResult *result,
                                       gpointer userData) {
  auto holder = std::unique_ptr<
      std::shared_ptr<TrayBackendImplementation>>(
      static_cast<std::shared_ptr<TrayBackendImplementation> *>(
          userData));
  auto implementation = *holder;
  auto *error = static_cast<GError *>(nullptr);
  auto *reply = g_dbus_connection_call_finish(
      G_DBUS_CONNECTION(source), result, &error);
  if (reply != nullptr) {
    g_variant_unref(reply);
  }
  if (implementation->destroyed) {
    if (error != nullptr) {
      g_error_free(error);
    }
    return;
  }
  if (error != nullptr) {
    std::cerr << "pipetune-gtk: cannot register with SNI watcher: "
              << error->message << '\n';
    g_error_free(error);
    unregisterStatusNotifierObjects(implementation.get());
    createFallbackBackend(implementation.get());
    return;
  }
  implementation->kind = TrayBackendKind::statusNotifierItem;
  setAvailability(implementation.get(),
                  TrayBackendAvailabilityState::available);
}

static void registerStatusNotifier(
    const std::shared_ptr<TrayBackendImplementation> &implementation) {
  if (!registerStatusNotifierObjects(implementation.get())) {
    unregisterStatusNotifierObjects(implementation.get());
    createFallbackBackend(implementation.get());
    return;
  }
  g_dbus_connection_call(
      implementation->connection, kStatusNotifierWatcherService,
      kStatusNotifierWatcherPath, kStatusNotifierWatcherInterface,
      "RegisterStatusNotifierItem",
      g_variant_new("(s)", kStatusNotifierItemPath), nullptr,
      G_DBUS_CALL_FLAGS_NONE, -1, implementation->cancellable,
      onStatusNotifierRegistered,
      new std::shared_ptr<TrayBackendImplementation>(implementation));
}

static void onStatusNotifierHostProperty(GObject *source,
                                         GAsyncResult *result,
                                         gpointer userData) {
  auto holder = std::unique_ptr<
      std::shared_ptr<TrayBackendImplementation>>(
      static_cast<std::shared_ptr<TrayBackendImplementation> *>(
          userData));
  auto implementation = *holder;
  auto *error = static_cast<GError *>(nullptr);
  auto *reply = g_dbus_connection_call_finish(
      G_DBUS_CONNECTION(source), result, &error);
  if (implementation->destroyed) {
    if (reply != nullptr) {
      g_variant_unref(reply);
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    return;
  }
  if (reply == nullptr || error != nullptr) {
    if (reply != nullptr) {
      g_variant_unref(reply);
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    createFallbackBackend(implementation.get());
    return;
  }
  auto *property = static_cast<GVariant *>(nullptr);
  g_variant_get(reply, "(v)", &property);
  const auto available = g_variant_get_boolean(property) != FALSE;
  g_variant_unref(property);
  g_variant_unref(reply);
  if (!available) {
    createFallbackBackend(implementation.get());
    return;
  }
  registerStatusNotifier(implementation);
}

static void queryStatusNotifierHost(
    const std::shared_ptr<TrayBackendImplementation> &implementation) {
  g_dbus_connection_call(
      implementation->connection, kStatusNotifierWatcherService,
      kStatusNotifierWatcherPath, "org.freedesktop.DBus.Properties",
      "Get",
      g_variant_new("(ss)", kStatusNotifierWatcherInterface,
                    "IsStatusNotifierHostRegistered"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1,
      implementation->cancellable, onStatusNotifierHostProperty,
      new std::shared_ptr<TrayBackendImplementation>(implementation));
}

static void onStatusNotifierNameOwner(GObject *source,
                                      GAsyncResult *result,
                                      gpointer userData) {
  auto holder = std::unique_ptr<
      std::shared_ptr<TrayBackendImplementation>>(
      static_cast<std::shared_ptr<TrayBackendImplementation> *>(
          userData));
  auto implementation = *holder;
  auto *error = static_cast<GError *>(nullptr);
  auto *reply = g_dbus_connection_call_finish(
      G_DBUS_CONNECTION(source), result, &error);
  if (implementation->destroyed) {
    if (reply != nullptr) {
      g_variant_unref(reply);
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    return;
  }
  if (reply == nullptr || error != nullptr) {
    if (reply != nullptr) {
      g_variant_unref(reply);
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    createFallbackBackend(implementation.get());
    return;
  }
  auto hasOwner = gboolean{FALSE};
  g_variant_get(reply, "(b)", &hasOwner);
  g_variant_unref(reply);
  if (hasOwner == FALSE) {
    createFallbackBackend(implementation.get());
    return;
  }
  queryStatusNotifierHost(implementation);
}

static void discoverStatusNotifier(
    const std::shared_ptr<TrayBackendImplementation> &implementation) {
  g_dbus_connection_call(
      implementation->connection, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
      g_variant_new("(s)", kStatusNotifierWatcherService),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1,
      implementation->cancellable, onStatusNotifierNameOwner,
      new std::shared_ptr<TrayBackendImplementation>(implementation));
}

TrayBackendState *createTrayBackend(TrayBackendOptions options) {
  auto implementation =
      std::make_shared<TrayBackendImplementation>(std::move(options));
  implementation->connection = g_application_get_dbus_connection(
      implementation->options.application);
  if (implementation->connection != nullptr) {
    g_object_ref(implementation->connection);
  }
  auto *state = new TrayBackendState{.implementation = implementation};
  if (implementation->connection == nullptr) {
    createFallbackBackend(implementation.get());
    return state;
  }
  discoverStatusNotifier(implementation);
  return state;
}

static void emitStatusNotifierSignal(
    TrayBackendImplementation *implementation, const char *signal,
    GVariant *parameters) {
  if (implementation->connection == nullptr ||
      implementation->kind != TrayBackendKind::statusNotifierItem ||
      implementation->itemRegistrationId == 0) {
    return;
  }
  auto *error = static_cast<GError *>(nullptr);
  g_dbus_connection_emit_signal(
      implementation->connection, nullptr, kStatusNotifierItemPath,
      kStatusNotifierItemInterface, signal, parameters, &error);
  if (error != nullptr) {
    g_error_free(error);
  }
}

void updateTrayBackend(TrayBackendState *state, TrayIconState iconState,
                       std::string_view tooltip) {
  if (state == nullptr || state->implementation->destroyed) {
    return;
  }
  auto &implementation = *state->implementation;
  implementation.iconState = iconState;
  implementation.tooltip = tooltip;
  updateStatusIcon(&implementation);
  emitStatusNotifierSignal(&implementation, "NewIcon", nullptr);
  emitStatusNotifierSignal(&implementation, "NewAttentionIcon", nullptr);
  emitStatusNotifierSignal(
      &implementation, "NewStatus",
      g_variant_new("(s)", statusNotifierStatus(iconState)));
  emitStatusNotifierSignal(&implementation, "NewTitle", nullptr);
}

void destroyTrayBackend(TrayBackendState *state) {
  if (state == nullptr) {
    return;
  }
  auto implementation = state->implementation;
  implementation->destroyed = true;
  implementation->availability =
      TrayBackendAvailabilityState::unavailable;
  g_cancellable_cancel(implementation->cancellable);
  unregisterStatusNotifierObjects(implementation.get());
  destroyXEmbedBackend(implementation.get());
  g_clear_object(&implementation->connection);
  delete state;
}

TrayBackendKind trayBackendKind(const TrayBackendState *state) {
  return state == nullptr ? TrayBackendKind::none
                          : state->implementation->kind;
}

TrayBackendAvailabilityState
trayBackendAvailability(const TrayBackendState *state) {
  return state == nullptr
             ? TrayBackendAvailabilityState::unavailable
             : state->implementation->availability;
}

} // namespace pipetune_gtk
