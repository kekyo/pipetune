/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "remote-control-panel.h"
#include "localization.h"
#include "ui-message.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <qrencode.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pipetune_gtk {

struct RemoteControlPanel {
  GtkWidget *page;
  GtkSwitch *enabled;
  GtkWidget *endpoint;
  GtkComboBoxText *address;
  GtkSpinButton *port;
  GtkToggleButton *tls;
  GtkWidget *files;
  GtkFileChooser *certificate;
  GtkFileChooser *key;
  GtkLabel *statusLabel;
  GtkLabel *errorLabel;
  GtkWidget *pairing;
  GtkLabel *url;
  GtkImage *qr;
  GtkButton *copy;
  GtkButton *rotate;
  ControlClient *client = nullptr;
  std::optional<pipetune::RemoteListenerStatus> status = {};
  std::optional<std::uint64_t> requestedLinkRevision = {};
  std::uint64_t connectionGeneration = 0;
  std::uint64_t requestGeneration = 0;
  guint copyFeedbackTimer = 0;
  bool pending = false;
  bool updating = false;
  std::string link = {};
  std::string error = {};
};

static void renderPanel(RemoteControlPanel &panel);

static std::string selectedAddress(const RemoteControlPanel &panel) {
  const auto *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(panel.address));
  return id ? id : "";
}

static void selectAddress(RemoteControlPanel &panel, const std::string &address) {
  auto *combo = GTK_COMBO_BOX(panel.address);
  if (!gtk_combo_box_set_active_id(combo, address.c_str())) {
    gtk_combo_box_text_append(panel.address, address.c_str(), address.c_str());
    gtk_combo_box_set_active_id(combo, address.c_str());
  }
}

static void refreshAddresses(RemoteControlPanel &panel) {
  struct Address {
    bool loopback;
    std::string name;
    std::string ip;
  };
  const auto previous = selectedAddress(panel);
  std::vector<Address> addresses;
  ifaddrs *list = nullptr;
  if (getifaddrs(&list) == 0) {
    const auto owner = std::unique_ptr<ifaddrs, decltype(&freeifaddrs)>(list, freeifaddrs);
    for (auto *entry = list; entry; entry = entry->ifa_next) {
      if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !(entry->ifa_flags & IFF_UP)) continue;
      const auto &ip = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr)->sin_addr;
      char text[INET_ADDRSTRLEN];
      if (inet_ntop(AF_INET, &ip, text, sizeof(text))) {
        addresses.push_back({(entry->ifa_flags & IFF_LOOPBACK) != 0, entry->ifa_name, text});
      }
    }
  }
  // Prefer a LAN interface. Loopback remains available for same-PC editors.
  std::stable_sort(addresses.begin(), addresses.end(), [](const Address &a, const Address &b) {
    return a.loopback < b.loopback;
  });
  panel.updating = true;
  gtk_combo_box_text_remove_all(panel.address);
  for (const auto &address : addresses) {
    const auto label = address.name + "  ·  " + address.ip;
    gtk_combo_box_text_append(panel.address, address.ip.c_str(), label.c_str());
  }
  if (panel.status && panel.status->port) selectAddress(panel, panel.status->address);
  else if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(panel.address), previous.c_str()) && !addresses.empty()) {
    gtk_combo_box_set_active(GTK_COMBO_BOX(panel.address), 0);
  }
  panel.updating = false;
}

static std::string fileName(GtkFileChooser *chooser) {
  const auto name = std::unique_ptr<gchar, decltype(&g_free)>(gtk_file_chooser_get_filename(chooser), g_free);
  return name ? name.get() : "";
}

static void setFile(GtkFileChooser *chooser, const std::string &path) {
  if (path.empty()) gtk_file_chooser_unselect_all(chooser);
  else gtk_file_chooser_set_filename(chooser, path.c_str());
}

// The symbol is rendered locally at an integer scale with a four-module quiet zone.
static void showPairingQr(GtkImage *image, const std::string &url) {
  gtk_image_clear(image);
  if (url.empty()) return;
  const auto code = std::unique_ptr<QRcode, decltype(&QRcode_free)>(
      QRcode_encodeString8bit(url.c_str(), 0, QR_ECLEVEL_M), QRcode_free);
  if (!code) return;
  constexpr int scale = 3;
  constexpr int margin = 4;
  const int size = (code->width + 2 * margin) * scale;
  auto *pixels = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, size, size);
  if (!pixels) return;
  gdk_pixbuf_fill(pixels, 0xffffffffU);
  auto *data = gdk_pixbuf_get_pixels(pixels);
  const int stride = gdk_pixbuf_get_rowstride(pixels);
  for (int y = 0; y < code->width; ++y) {
    for (int x = 0; x < code->width; ++x) {
      if (!(code->data[y * code->width + x] & 1)) continue;
      for (int dy = 0; dy < scale; ++dy) {
        auto *start = data + ((y + margin) * scale + dy) * stride + (x + margin) * scale * 3;
        std::fill_n(start, 3 * scale, 0);
      }
    }
  }
  gtk_image_set_from_pixbuf(image, pixels);
  g_object_unref(pixels);
}

static void clearCopyFeedback(RemoteControlPanel &panel) {
  if (panel.copyFeedbackTimer) {
    g_source_remove(panel.copyFeedbackTimer);
    panel.copyFeedbackTimer = 0;
  }
  gtk_button_set_label(panel.copy, translate("Copy URL"));
}

static void setLink(RemoteControlPanel &panel, const std::string &link) {
  if (panel.link == link) return;
  panel.link = link;
  gtk_label_set_text(panel.url, link.c_str());
  clearCopyFeedback(panel);
  showPairingQr(panel.qr, link);
}

static void acceptStatus(RemoteControlPanel &panel, const pipetune::RemoteListenerStatus &next) {
  if (panel.status && next.revision < panel.status->revision) return;
  if (!panel.status || panel.status->revision != next.revision) {
    setLink(panel, {});
    panel.requestedLinkRevision.reset();
    panel.error.clear();
    if (next.port) {
      panel.updating = true;
      selectAddress(panel, next.address);
      gtk_spin_button_set_value(panel.port, next.port);
      gtk_toggle_button_set_active(panel.tls, !next.certificate.empty());
      setFile(panel.certificate, next.certificate);
      setFile(panel.key, next.key);
      panel.updating = false;
    }
  }
  panel.status = next;
}

static void onRemoteReply(const ControlClientReply &reply, void *data) {
  auto &panel = *static_cast<RemoteControlPanel *>(data);
  panel.pending = false;
  // A reply from a previous daemon or an older link must never restore its URL.
  if (panel.status && panel.requestGeneration == panel.connectionGeneration) {
    if (!reply.transportError.empty() || !reply.response.valid || !reply.response.success) {
      panel.error = reply.transportError.empty() ? reply.response.error : reply.transportError;
    } else {
      const auto &next = reply.response.status.remoteListener;
      if (next.revision >= panel.status->revision) {
        acceptStatus(panel, next);
        if (next.port && gtk_widget_get_mapped(panel.page)) setLink(panel, reply.response.pairingLink);
      }
    }
  }
  renderPanel(panel);
}

static void request(RemoteControlPanel &panel, const pipetune::RemoteListenerOptions &options) {
  if (panel.pending || !panel.client || !panel.status) return;
  panel.pending = true;
  panel.requestGeneration = panel.connectionGeneration;
  panel.error.clear();
  requestRemoteListenerAsync(panel.client, options, onRemoteReply, &panel);
  renderPanel(panel);
}

static void renderPanel(RemoteControlPanel &panel) {
  if (panel.updating) return;
  const bool enabled = panel.status && panel.status->port != 0;
  const bool tls = gtk_toggle_button_get_active(panel.tls);
  const bool editable = panel.status.has_value() && !panel.pending;
  const bool hasAddress = !selectedAddress(panel).empty();
  const bool hasTlsFiles = !tls || enabled || (!fileName(panel.certificate).empty() && !fileName(panel.key).empty());
  panel.updating = true;
  gtk_switch_set_active(panel.enabled, enabled);
  gtk_switch_set_state(panel.enabled, enabled);
  gtk_widget_set_sensitive(GTK_WIDGET(panel.enabled), editable && (enabled || (hasAddress && hasTlsFiles)));
  gtk_widget_set_sensitive(panel.endpoint, editable && !enabled);
  gtk_widget_set_visible(panel.files, tls);
  gtk_widget_set_visible(panel.pairing, enabled && !panel.link.empty());
  gtk_widget_set_sensitive(panel.pairing, editable);
  auto status = std::string{};
  if (!panel.status) status = translate("PipeTune is disconnected.");
  else if (enabled) status = formatUiMessage(localizedMessage("Listening · Connected devices: {0}",
                                                               {std::to_string(panel.status->clients)}));
  else if (!hasAddress) status = translate("No local IPv4 address is available.");
  else if (!hasTlsFiles) status = translate("Choose a certificate and private key to enable HTTPS.");
  else status = translate("Remote control is off.");
  gtk_label_set_text(panel.statusLabel, status.c_str());
  gtk_label_set_text(panel.errorLabel, panel.error.c_str());
  gtk_widget_set_visible(GTK_WIDGET(panel.errorLabel), !panel.error.empty());
  panel.updating = false;
  if (enabled && !panel.pending && panel.link.empty() && gtk_widget_get_mapped(panel.page) &&
      panel.requestedLinkRevision != panel.status->revision) {
    panel.requestedLinkRevision = panel.status->revision;
    request(panel, {.operation = "link"});
  }
}

static gboolean onSwitch(GtkSwitch *, gboolean enable, gpointer data) {
  auto &panel = *static_cast<RemoteControlPanel *>(data);
  if (panel.updating) return FALSE;
  if (!enable) {
    request(panel, {.operation = "disable"});
  } else {
    const bool tls = gtk_toggle_button_get_active(panel.tls);
    request(panel, {.operation = "enable", .address = selectedAddress(panel),
                    .port = static_cast<std::uint32_t>(gtk_spin_button_get_value_as_int(panel.port)),
                    .certificate = tls ? fileName(panel.certificate) : "",
                    .key = tls ? fileName(panel.key) : ""});
  }
  return TRUE;
}

RemoteControlPanel *createRemoteControlPanel(GtkBuilder *builder) {
  const auto get = [builder](const char *id) { return gtk_builder_get_object(builder, id); };
  auto *panel = new RemoteControlPanel{
      .page = GTK_WIDGET(get("remotePage")), .enabled = GTK_SWITCH(get("remoteEnabled")),
      .endpoint = GTK_WIDGET(get("remoteEndpoint")), .address = GTK_COMBO_BOX_TEXT(get("remoteAddress")),
      .port = GTK_SPIN_BUTTON(get("remotePort")), .tls = GTK_TOGGLE_BUTTON(get("remoteTls")),
      .files = GTK_WIDGET(get("remoteTlsFiles")), .certificate = GTK_FILE_CHOOSER(get("remoteCert")),
      .key = GTK_FILE_CHOOSER(get("remoteKey")), .statusLabel = GTK_LABEL(get("remoteStatus")),
      .errorLabel = GTK_LABEL(get("remoteError")), .pairing = GTK_WIDGET(get("remotePairing")),
      .url = GTK_LABEL(get("remoteUrl")), .qr = GTK_IMAGE(get("remoteQr")),
      .copy = GTK_BUTTON(get("remoteCopy")), .rotate = GTK_BUTTON(get("remoteRotate"))};
  gtk_widget_set_direction(GTK_WIDGET(panel->url), GTK_TEXT_DIR_LTR);
  for (auto *group : {panel->files, panel->pairing}) {
    gtk_container_foreach(GTK_CONTAINER(group), +[](GtkWidget *child, gpointer) { gtk_widget_show_all(child); }, nullptr);
  }
  g_signal_connect(panel->enabled, "state-set", G_CALLBACK(onSwitch), panel);
  const auto update = +[](GtkWidget *, gpointer data) { renderPanel(*static_cast<RemoteControlPanel *>(data)); };
  g_signal_connect(panel->address, "changed", G_CALLBACK(update), panel);
  g_signal_connect(panel->tls, "toggled", G_CALLBACK(update), panel);
  g_signal_connect(panel->certificate, "file-set", G_CALLBACK(update), panel);
  g_signal_connect(panel->key, "file-set", G_CALLBACK(update), panel);
  g_signal_connect(panel->page, "map", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    auto &page = *static_cast<RemoteControlPanel *>(data);
    refreshAddresses(page);
    renderPanel(page);
  }), panel);
  g_signal_connect(panel->page, "unmap", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    auto &page = *static_cast<RemoteControlPanel *>(data);
    setLink(page, {});
    page.requestedLinkRevision.reset();
  }), panel);
  g_signal_connect(panel->copy, "clicked", G_CALLBACK(+[](GtkButton *button, gpointer data) {
    auto &page = *static_cast<RemoteControlPanel *>(data);
    if (!page.link.empty()) {
      gtk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button), GDK_SELECTION_CLIPBOARD),
                             page.link.c_str(), -1);
      clearCopyFeedback(page);
      gtk_button_set_label(button, translate("Copied"));
      page.copyFeedbackTimer = g_timeout_add(1800, +[](gpointer arg) -> gboolean {
        auto &current = *static_cast<RemoteControlPanel *>(arg);
        current.copyFeedbackTimer = 0;
        gtk_button_set_label(current.copy, translate("Copy URL"));
        return G_SOURCE_REMOVE;
      }, &page);
    }
  }), panel);
  g_signal_connect(panel->rotate, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    request(*static_cast<RemoteControlPanel *>(data), {.operation = "rotate"});
  }), panel);
  renderPanel(*panel);
  return panel;
}

void updateRemoteControlPanel(RemoteControlPanel *panel, ControlClient *client,
                              const pipetune::RemoteListenerStatus *listener) {
  panel->client = client;
  if (listener) {
    if (panel->status && *panel->status == *listener) return;
    acceptStatus(*panel, *listener);
  } else {
    if (!panel->status) return;
    ++panel->connectionGeneration;
    panel->status.reset();
    panel->requestedLinkRevision.reset();
    panel->error.clear();
    setLink(*panel, {});
  }
  renderPanel(*panel);
}

void destroyRemoteControlPanel(RemoteControlPanel *panel) {
  if (!panel) return;
  clearCopyFeedback(*panel);
  for (auto *widget : {G_OBJECT(panel->enabled), G_OBJECT(panel->address), G_OBJECT(panel->tls),
                        G_OBJECT(panel->certificate), G_OBJECT(panel->key), G_OBJECT(panel->page),
                        G_OBJECT(panel->copy), G_OBJECT(panel->rotate)}) {
    g_signal_handlers_disconnect_by_data(widget, panel);
  }
  delete panel;
}

} // namespace pipetune_gtk
