#ifndef PIPETUNE_GTK_TRAY_BACKEND_H
#define PIPETUNE_GTK_TRAY_BACKEND_H

#include <gio/gio.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pipetune_gtk {

/**
 * Identifies the selected tray transport.
 */
enum class TrayBackendKind {
  /** No usable tray transport is available. */
  none,
  /** Freedesktop StatusNotifierItem over D-Bus. */
  statusNotifierItem,
  /** Legacy GtkStatusIcon over XEmbed. */
  xembed
};

/**
 * Identifies whether a tray host can retain the hidden application.
 */
enum class TrayBackendAvailabilityState {
  /** Discovery or registration is still running. */
  pending,
  /** No usable tray host exists. */
  unavailable,
  /** A tray host currently retains the application. */
  available
};

/**
 * Describes tray transports available in the current session.
 */
struct TrayBackendAvailability {
  /** True when an SNI watcher and host are available. */
  bool hasStatusNotifierItem;
  /** True when the X11 display can host GtkStatusIcon. */
  bool hasXEmbed;
};

/**
 * Selects whether the tray artwork retains its source colors.
 */
enum class TrayIconColorMode {
  /** Render the source artwork unchanged. */
  color,
  /** Render the source artwork in grayscale. */
  grayscale
};

/**
 * Selects the semantic state shown by the tray icon.
 */
enum class TrayIconState {
  /** Connected and healthy. */
  active,
  /** Connected but carrying a warning or error. */
  attention,
  /** PipeTune daemon unavailable. */
  disconnected
};

/**
 * Stores one StatusNotifierItem icon pixmap in ARGB byte order.
 */
struct TrayIconPixmap {
  /** Pixmap width in pixels. */
  int width;
  /** Pixmap height in pixels. */
  int height;
  /** Consecutive alpha, red, green, and blue bytes. */
  std::vector<std::uint8_t> argbPixels;
};

/**
 * Receives tray backend actions.
 */
struct TrayBackendCallbacks {
  /** Opens or presents the PipeTune window. */
  std::function<void()> activate;
  /** Requests an explicit application quit. */
  std::function<void()> quit;
  /** Reports discovery and host availability changes. */
  std::function<void(TrayBackendAvailabilityState)> availabilityChanged;
};

/**
 * Configures one tray backend.
 */
struct TrayBackendOptions {
  /** Registered application whose session bus connection is used for SNI. */
  GApplication *application;
  /** Stable StatusNotifierItem identifier. */
  std::string identifier;
  /** User-visible title. */
  std::string title;
  /** Initial semantic icon state. */
  TrayIconState iconState;
  /** Initial artwork color mode. */
  TrayIconColorMode colorMode;
  /** Initial tooltip text. */
  std::string tooltip;
  /** Backend callbacks. */
  TrayBackendCallbacks callbacks;
};

/** Opaque tray backend lifetime state. */
struct TrayBackendState;

/**
 * Returns the GtkApplication identifier.
 *
 * @return Stable reverse-DNS application identifier.
 */
const char *applicationId();

/**
 * Chooses SNI first, then GtkStatusIcon/XEmbed.
 *
 * @param availability Available transports.
 * @return Preferred backend kind.
 */
TrayBackendKind
selectTrayBackendKind(const TrayBackendAvailability &availability);

/**
 * Maps a color mode to the public icon theme name.
 *
 * @param colorMode Requested artwork color mode.
 * @return Stable icon name, or empty when a pixmap must be used.
 */
std::string_view trayIconName(TrayIconColorMode colorMode);

/**
 * Converts RGB or RGBA pixbuf bytes to SNI ARGB bytes.
 *
 * @param pixels Source pixel bytes.
 * @param width Source width.
 * @param height Source height.
 * @param rowstride Bytes between source rows.
 * @param channelCount Three for RGB or four for RGBA.
 * @return Converted bytes, or empty for invalid input.
 */
std::vector<std::uint8_t>
convertTrayIconPixelsToArgb(const std::uint8_t *pixels, int width,
                            int height, int rowstride,
                            int channelCount);

/**
 * Builds an SNI `a(iiay)` icon-pixmap value.
 *
 * @param pixmaps Valid ARGB pixmaps.
 * @return Floating GVariant containing valid entries.
 */
GVariant *
buildTrayIconPixmapVariant(const std::vector<TrayIconPixmap> &pixmaps);

/**
 * Loads the embedded PipeTune artwork at the SNI icon sizes.
 *
 * @param colorMode Requested artwork color mode.
 * @return Valid ARGB pixmaps, or an empty vector when the artwork cannot
 *         be loaded.
 */
std::vector<TrayIconPixmap>
loadTrayIconPixmaps(TrayIconColorMode colorMode);

/**
 * Starts asynchronous SNI discovery with GtkStatusIcon fallback.
 *
 * @param options Application identity, state, and callbacks.
 * @return Backend state to release with destroyTrayBackend().
 */
TrayBackendState *createTrayBackend(TrayBackendOptions options);

/**
 * Updates the SNI and GtkStatusIcon presentation.
 *
 * @param state Backend state, or null.
 * @param iconState New semantic state.
 * @param colorMode New artwork color mode.
 * @param tooltip New tooltip text.
 */
void updateTrayBackend(TrayBackendState *state, TrayIconState iconState,
                       TrayIconColorMode colorMode,
                       std::string_view tooltip);

/**
 * Stops registration and releases backend resources.
 *
 * @param state Backend state, or null.
 */
void destroyTrayBackend(TrayBackendState *state);

/**
 * Returns the selected backend.
 *
 * @param state Backend state, or null.
 * @return Current backend kind.
 */
TrayBackendKind trayBackendKind(const TrayBackendState *state);

/**
 * Returns current tray-host availability.
 *
 * @param state Backend state, or null.
 * @return Current availability.
 */
TrayBackendAvailabilityState
trayBackendAvailability(const TrayBackendState *state);

} // namespace pipetune_gtk

#endif
